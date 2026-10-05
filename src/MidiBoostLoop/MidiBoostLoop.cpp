#include "MidiBoostLoop.h"
#include "Actions/ActionRules.h"
#include "MultiMidi/MidiInbound.h"
#include <Preferences.h>
#include <cmath>

MidiBoostLoop* MidiBoostLoop::instance = nullptr;

namespace {
constexpr const char* kPrefsNamespace = "midiboost";
constexpr const char* kBoostActiveKey = "boostOn";
constexpr const char* kBaseGainKey = "baseGain";

const float kGainSteps[] = {
    Showbox::InputGain::POSITION_1,
    Showbox::InputGain::POSITION_2,
    Showbox::InputGain::POSITION_3,
    Showbox::InputGain::POSITION_4,
    Showbox::InputGain::POSITION_5,
    Showbox::InputGain::POSITION_6,
    Showbox::InputGain::POSITION_7,
    Showbox::InputGain::POSITION_8,
    Showbox::InputGain::POSITION_9,
    Showbox::InputGain::POSITION_10,
    Showbox::InputGain::POSITION_11,
    Showbox::InputGain::POSITION_12,
    Showbox::InputGain::POSITION_13,
    Showbox::InputGain::POSITION_14,
    Showbox::InputGain::POSITION_15,
    Showbox::InputGain::POSITION_16,
};
}

MidiBoostLoop::MidiBoostLoop(MackieShowbox* showbox, uint8_t channel,
    uint8_t recPlayOverdubCc, uint8_t stopCc, uint8_t deleteCc,
    uint8_t boostOnCc, uint8_t boostOffCc)
    : showbox(showbox),
      midiChannel(channel),
      recPlayOverdubCc(recPlayOverdubCc),
      stopCc(stopCc),
      deleteCc(deleteCc),
      boostOnCc(boostOnCc),
      boostOffCc(boostOffCc) {}

void MidiBoostLoop::setNextHandlers(NoteCallback noteOn, NoteCallback noteOff,
    ControlChangeCallback controlChange, PitchBendCallback pitchBend) {
    nextNoteOn = noteOn;
    nextNoteOff = noteOff;
    nextControlChange = controlChange;
    nextPitchBend = pitchBend;
}

void MidiBoostLoop::begin(MultiMidi* midiIn) {
    midi = midiIn;
    instance = this;
    loadBoostState();
    boostActive = false;
    boostWanted = false;
    autoBoostOn = false;
    job = Job::None;
    mode = modeFromState(showbox->getLooperState());

    midi->action.setCallbackOnControlChange(MidiBoostLoop::onControlChange);
    midi->action.setCallbackOnNoteOn(MidiBoostLoop::onNoteOn);
    midi->action.setCallbackOnNoteOff(MidiBoostLoop::onNoteOff);
    midi->action.setCallbackOnPitchBend(MidiBoostLoop::onPitchBend);

    boostNeedAnnounce = true;
    announceAt = millis() + 250;
    lastOutputGeneration = midi->outputGeneration();

    Debug->printf("MIDI boost/loop CC on channel %u, value >= %u: rec/play/overdub %u, stop %u, delete %u, boost on %u, boost off %u, %+d steps\n",
        midiChannel + 1, kPressValue, recPlayOverdubCc, stopCc, deleteCc, boostOnCc, boostOffCc, kBoostSteps);
    Debug->printf("Looper start state %s (%u) mode=%s\n",
        stateName(showbox->getLooperState()),
        (unsigned)showbox->getUint8EntityValue(entity_id::LOOPER_STATE),
        modeName(mode));
}

void MidiBoostLoop::tick() {
    runJob();
    syncModeFromLive();
    syncBoost();
    if (boostApply) {
        applyBoost();
    }
    uint32_t gen = (midi != nullptr) ? midi->outputGeneration() : 0;
    if (gen != lastOutputGeneration) {
        lastOutputGeneration = gen;
        announceAt = millis() + 200;
        boostNeedAnnounce = true;
    }
    if (boostNeedAnnounce && announceAt != 0 && (int32_t)(millis() - announceAt) >= 0 && job == Job::None) {
        boostNeedAnnounce = false;
        announceAt = 0;
        publishBoost();
    }
    if (boostDirty && job == Job::None && !showbox->uartHot()) {
        boostDirty = false;
        saveBoostState();
    }
}

void MidiBoostLoop::setDebugSerial(Print* serial) {
    Debug = serial;
}

bool MidiBoostLoop::isBoostActive() const {
    return boostActive;
}

void MidiBoostLoop::logIncoming(const char* kind, uint8_t channel, uint8_t a, uint8_t b) {
    Debug->printf("MIDI in %s %s ch=%u d1=%u d2=%u\n",
        midiInboundSource(), kind, (unsigned)(channel + 1), (unsigned)a, (unsigned)b);
}

void MidiBoostLoop::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (instance == nullptr) {
        return;
    }
    instance->logIncoming("noteOn", channel, note, velocity);
    if (instance->nextNoteOn != nullptr) {
        instance->nextNoteOn(channel, note, velocity);
    }
    actionRulesOnMidi("noteOn", channel, note, velocity);
}

void MidiBoostLoop::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (instance == nullptr) {
        return;
    }
    instance->logIncoming("noteOff", channel, note, velocity);
    if (instance->nextNoteOff != nullptr) {
        instance->nextNoteOff(channel, note, velocity);
    }
    actionRulesOnMidi("noteOff", channel, note, velocity);
}

void MidiBoostLoop::onControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (instance == nullptr || instance->publishing) {
        return;
    }
    if (instance->isBoostEcho(controller, value)) {
        return;
    }
    instance->logIncoming("cc", channel, controller, value);
    instance->handleControlChange(channel, controller, value);
}

void MidiBoostLoop::onPitchBend(uint8_t channel, uint8_t value) {
    if (instance == nullptr) {
        return;
    }
    instance->logIncoming("pitch", channel, value, 0);
    if (instance->nextPitchBend != nullptr) {
        instance->nextPitchBend(channel, value);
    }
}

bool MidiBoostLoop::isLooperCc(uint8_t controller) const {
    return controller == recPlayOverdubCc || controller == stopCc || controller == deleteCc
        || controller == boostOnCc || controller == boostOffCc;
}

bool MidiBoostLoop::isBoostCc(uint8_t controller) const {
    return controller == boostOnCc || controller == boostOffCc;
}

bool MidiBoostLoop::isBoostEcho(uint8_t controller, uint8_t value) const {
    if (!isBoostCc(controller) || value < kPressValue || lastPublishedBoostCc == 0) {
        return false;
    }
    if ((int32_t)(millis() - ignoreBoostEchoUntil) >= 0) {
        return false;
    }
    return controller == lastPublishedBoostCc;
}

bool MidiBoostLoop::acceptPress() {
    uint32_t now = millis();
    if (lastPressMs != 0 && (int32_t)(now - lastPressMs) < (int32_t)kPressDebounceMs) {
        Debug->println("MIDI looper CC ignored (debounce)");
        return false;
    }
    lastPressMs = now;
    return true;
}

void MidiBoostLoop::handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    (void)channel;
    if (value >= kPressValue && isBoostCc(controller)) {
        bool on = controller == boostOnCc;
        Debug->printf("Boost CC %u -> %s\n", (unsigned)controller, on ? "on" : "off");
        setBoost(on);
        return;
    }
    if (value >= kPressValue && isLooperCc(controller)) {
        if (!acceptPress()) {
            return;
        }
        if (controller == recPlayOverdubCc) {
            handleRecordPlayOverdub();
            return;
        }
        if (controller == stopCc) {
            handleStop();
            return;
        }
        if (controller == deleteCc) {
            handleDelete();
            return;
        }
    }
    if (isLooperCc(controller)) {
        return;
    }
    if (nextControlChange != nullptr) {
        nextControlChange(channel, controller, value);
    }
    actionRulesOnMidi("cc", channel, controller, value);
}

MidiBoostLoop::Mode MidiBoostLoop::modeFromState(looper_state state) const {
    switch (state) {
        case RECORD_INITIAL_LOOP:
            return Mode::Recording;
        case RECORD_OVERDUB:
            return Mode::Overdub;
        case PLAY:
            return Mode::Playing;
        case STOP_PLAYING:
            return Mode::Stopped;
        default:
            return Mode::Empty;
    }
}

const char* MidiBoostLoop::modeName(Mode current) const {
    switch (current) {
        case Mode::Recording: return "recording";
        case Mode::Overdub: return "overdub";
        case Mode::Playing: return "playing";
        case Mode::Stopped: return "stopped";
        default: return "empty";
    }
}

const char* MidiBoostLoop::stateName(looper_state state) const {
    switch (state) {
        case RECORD_INITIAL_LOOP: return "RECORD_INITIAL_LOOP";
        case RECORD_OVERDUB: return "RECORD_OVERDUB";
        case PLAY: return "PLAY";
        case STOP_PLAYING: return "STOP_PLAYING";
        case DELETE: return "EMPTY";
        default: return "UNKNOWN";
    }
}

const char* MidiBoostLoop::buttonName(looper_button_action action) const {
    switch (action) {
        case looper_button_action::DOWN: return "DOWN";
        case looper_button_action::UP: return "UP";
        case looper_button_action::DOUBLE_PRESS: return "DOUBLE_PRESS";
        case looper_button_action::LONG_PRESS: return "LONG_PRESS";
        default: return "?";
    }
}

void MidiBoostLoop::sendButton(looper_button_action action) {
    Debug->printf("Looper LOOPER_BUTTON %s (live %s)\n",
        buttonName(action), stateName(showbox->getLooperState()));
    showbox->sendLooperButtonAction(action);
}

void MidiBoostLoop::syncModeFromLive() {
    if (job != Job::None) {
        return;
    }
    Mode live = modeFromState(showbox->getLooperState());
    if (live == mode) {
        return;
    }
    Debug->printf("Looper live state %s (%u) mode %s -> %s\n",
        stateName(showbox->getLooperState()),
        (unsigned)showbox->getUint8EntityValue(entity_id::LOOPER_STATE),
        modeName(mode), modeName(live));
    mode = live;
}

void MidiBoostLoop::queueJob(Job next) {
    job = next;
    jobStep = 0;
    jobAt = millis();
}

bool MidiBoostLoop::looperIsPlay() const {
    return showbox->getLooperState() == PLAY;
}

void MidiBoostLoop::runJob() {
    if (job == Job::None) {
        return;
    }
    if ((int32_t)(millis() - jobAt) < 0) {
        return;
    }
    if (job == Job::Tap) {
        if (jobStep == 0) {
            sendButton(looper_button_action::DOWN);
            jobStep = 1;
            jobAt = millis() + kTapGapMs;
            return;
        }
        sendButton(looper_button_action::UP);
        job = Job::None;
        return;
    }
    if (job == Job::EndRecThenOverdub) {
        if (jobStep == 0) {
            sendButton(looper_button_action::DOWN);
            jobStep = 1;
            jobAt = millis() + kTapGapMs;
            return;
        }
        if (jobStep == 1) {
            sendButton(looper_button_action::UP);
            jobStep = 2;
            jobAt = millis() + kOverdubMinMs;
            Debug->println("Looper rec ended, waiting for PLAY before overdub tap");
            return;
        }
        if (jobStep == 2) {
            bool ready = looperIsPlay();
            bool timedOut = (int32_t)(millis() - jobAt) >= (int32_t)(kOverdubMaxMs - kOverdubMinMs);
            if (!ready && !timedOut) {
                return;
            }
            Debug->printf("Looper overdub tap (live %s%s)\n",
                stateName(showbox->getLooperState()),
                timedOut && !ready ? ", timed out" : "");
            sendButton(looper_button_action::DOWN);
            jobStep = 3;
            jobAt = millis() + kTapGapMs;
            return;
        }
        sendButton(looper_button_action::UP);
        job = Job::None;
        Debug->println("Looper overdub");
        return;
    }
    if (job == Job::Stop) {
        sendButton(looper_button_action::DOUBLE_PRESS);
        job = Job::None;
        Debug->println("Looper stop");
        return;
    }
    if (job == Job::Delete) {
        sendButton(looper_button_action::LONG_PRESS);
        job = Job::None;
        Debug->println("Looper delete");
    }
}

void MidiBoostLoop::handleRecordPlayOverdub() {
    if (job != Job::None) {
        Debug->println("Looper 102 ignored (busy)");
        return;
    }
    syncModeFromLive();
    switch (mode) {
        case Mode::Empty:
            mode = Mode::Recording;
            queueJob(Job::Tap);
            Debug->printf("Looper rec (button tap, live %s)\n",
                stateName(showbox->getLooperState()));
            break;
        case Mode::Recording:
            mode = Mode::Overdub;
            queueJob(Job::EndRecThenOverdub);
            Debug->println("Looper rec -> play -> overdub");
            break;
        case Mode::Playing:
            mode = Mode::Overdub;
            queueJob(Job::Tap);
            Debug->println("Looper overdub (button tap)");
            break;
        case Mode::Overdub:
            mode = Mode::Playing;
            queueJob(Job::Tap);
            Debug->println("Looper play after overdub (button tap)");
            break;
        case Mode::Stopped:
            mode = Mode::Playing;
            queueJob(Job::Tap);
            Debug->println("Looper play from stop (button tap)");
            break;
    }
}

void MidiBoostLoop::handleStop() {
    if (job != Job::None) {
        Debug->println("Looper 103 ignored (busy)");
        return;
    }
    syncModeFromLive();
    Debug->printf("Looper stop (live %s mode %s)\n",
        stateName(showbox->getLooperState()), modeName(mode));
    mode = (mode == Mode::Recording || mode == Mode::Empty) ? Mode::Empty : Mode::Stopped;
    queueJob(Job::Stop);
}

void MidiBoostLoop::handleDelete() {
    if (job != Job::None) {
        Debug->println("Looper 104 ignored (busy)");
        return;
    }
    syncModeFromLive();
    Debug->printf("Looper delete (live %s mode %s)\n",
        stateName(showbox->getLooperState()), modeName(mode));
    mode = Mode::Empty;
    queueJob(Job::Delete);
}

void MidiBoostLoop::setBoost(bool on) {
    boostWanted = on;
    boostApply = true;
}

bool MidiBoostLoop::autoBoostWanted() const {
    return mode == Mode::Playing && job == Job::None;
}

void MidiBoostLoop::syncBoost() {
    // Auto on/off only when play actually starts or ends. A manual CC 105/106
    // is left alone until the next rec/overdub/stop/play transition.
    bool want = autoBoostWanted();
    if (want == autoBoostOn) {
        return;
    }
    autoBoostOn = want;
    setBoost(want);
}

void MidiBoostLoop::applyBoost() {
    boostApply = false;
    float current = clampGain(showbox->getInputGain(kInputChannel2));
    if (boostWanted) {
        if (boostActive) {
            return;
        }
        baseGain = current;
        int step = nearestStep(baseGain) + kBoostSteps;
        if (step >= kStepCount) {
            step = kStepCount - 1;
        }
        boostedGain = kGainSteps[step];
        showbox->setInputGain(kInputChannel2, boostedGain);
        boostActive = true;
        Debug->printf("Boost on: channel 2 %.1f -> %.1f\n", baseGain, boostedGain);
    } else {
        if (!boostActive) {
            return;
        }
        if (nearestStep(current) == nearestStep(boostedGain)) {
            showbox->setInputGain(kInputChannel2, clampGain(baseGain));
            Debug->printf("Boost off: channel 2 restored to %.1f\n", baseGain);
        } else {
            baseGain = current;
            Debug->printf("Boost off: gain was moved, new base %.1f\n", baseGain);
        }
        boostActive = false;
    }
    boostDirty = true;
    publishBoost();
}

void MidiBoostLoop::publishBoost() {
    if (midi == nullptr || publishing) {
        return;
    }
    publishing = true;
    uint8_t controller = boostActive ? boostOnCc : boostOffCc;
    lastPublishedBoostCc = controller;
    ignoreBoostEchoUntil = millis() + kBoostEchoMs;
    Debug->printf("MIDI out cc ch=%u d1=%u d2=127 (%s)\n",
        (unsigned)(midiChannel + 1), (unsigned)controller, boostActive ? "boost on" : "boost off");
    midi->controlChange(controller, 127, static_cast<int8_t>(midiChannel));
    publishing = false;
}

void MidiBoostLoop::loadBoostState() {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
        if (prefs.isKey(kBaseGainKey)) {
            baseGain = prefs.getFloat(kBaseGainKey, 0.0f);
        }
        prefs.end();
        return;
    }
    Preferences legacy;
    if (!legacy.begin("airstep", true)) {
        return;
    }
    if (legacy.isKey(kBaseGainKey)) {
        baseGain = legacy.getFloat(kBaseGainKey, 0.0f);
    }
    legacy.end();
}

void MidiBoostLoop::saveBoostState() {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, false)) {
        Debug->println("Failed to save MIDI boost state");
        return;
    }
    prefs.putBool(kBoostActiveKey, boostActive);
    prefs.putFloat(kBaseGainKey, baseGain);
    prefs.end();
}

float MidiBoostLoop::clampGain(float gain) const {
    if (gain < kMinGain) {
        return kMinGain;
    }
    if (gain > kMaxGain) {
        return kMaxGain;
    }
    return gain;
}

int MidiBoostLoop::nearestStep(float gain) const {
    int best = 0;
    float bestDistance = fabsf(gain - kGainSteps[0]);
    for (int i = 1; i < kStepCount; i++) {
        float distance = fabsf(gain - kGainSteps[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}
