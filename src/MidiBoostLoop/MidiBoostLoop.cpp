#include "MidiBoostLoop.h"
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
    startedAt = millis();
    loadBoostState();

    midi->action.setCallbackOnNoteOn(MidiBoostLoop::onNoteOn);
    midi->action.setCallbackOnNoteOff(MidiBoostLoop::onNoteOff);
    midi->action.setCallbackOnControlChange(MidiBoostLoop::onControlChange);
    midi->action.setCallbackOnPitchBend(MidiBoostLoop::onPitchBend);

    Debug->printf("MIDI boost/loop CC on channel %u, value >= %u: rec/play/overdub %u, stop %u, delete %u, boost on %u, boost off %u, %+d steps\n",
        midiChannel + 1, kPressValue, recPlayOverdubCc, stopCc, deleteCc, boostOnCc, boostOffCc, kBoostSteps);
    Debug->printf("Channel 2 boost is %s (restores %.1f)\n",
        boostActive ? "on" : "off", baseGain);
}

void MidiBoostLoop::tick() {
    uint8_t state = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
    if (!stateSeeded) {
        if (millis() - startedAt < 1500) {
            return;
        }
        lastLooperState = state;
        stateSeeded = true;
        return;
    }
    if (state == lastLooperState) {
        return;
    }
    lastLooperState = state;
    applyLooperState(state);
}

void MidiBoostLoop::setDebugSerial(Print* serial) {
    Debug = serial;
}

bool MidiBoostLoop::isBoostActive() const {
    return boostActive;
}

void MidiBoostLoop::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (instance != nullptr && instance->nextNoteOn != nullptr) {
        instance->nextNoteOn(channel, note, velocity);
    }
}

void MidiBoostLoop::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (instance != nullptr && instance->nextNoteOff != nullptr) {
        instance->nextNoteOff(channel, note, velocity);
    }
}

void MidiBoostLoop::onControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (instance == nullptr || instance->publishing) {
        return;
    }
    instance->handleControlChange(channel, controller, value);
}

void MidiBoostLoop::onPitchBend(uint8_t channel, uint8_t value) {
    if (instance != nullptr && instance->nextPitchBend != nullptr) {
        instance->nextPitchBend(channel, value);
    }
}

void MidiBoostLoop::handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (channel == midiChannel && value >= kPressValue) {
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
        if (controller == boostOnCc) {
            setBoost(true);
            return;
        }
        if (controller == boostOffCc) {
            setBoost(false);
            return;
        }
    }
    if (nextControlChange != nullptr) {
        nextControlChange(channel, controller, value);
    }
}

void MidiBoostLoop::handleRecordPlayOverdub() {
    showbox->sendLooperButtonAction(looper_button_action::DOWN);
    showbox->sendLooperButtonAction(looper_button_action::UP);
    Debug->println("Looper rec/play/overdub");
}

void MidiBoostLoop::handleStop() {
    uint8_t currentState = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
    if (currentState == (uint8_t)PLAY || currentState == (uint8_t)RECORD_INITIAL_LOOP || currentState == (uint8_t)RECORD_OVERDUB) {
        showbox->sendLooperButtonAction(looper_button_action::DOUBLE_PRESS);
        Debug->println("Looper stop");
        setBoost(false);
    }
}

void MidiBoostLoop::handleDelete() {
    showbox->sendLooperButtonAction(looper_button_action::LONG_PRESS);
    Debug->println("Looper delete");
}

void MidiBoostLoop::applyLooperState(uint8_t state) {
    if (state == (uint8_t)PLAY) {
        Debug->println("Looper is playing, boost on");
        setBoost(true);
        return;
    }
    if (state == (uint8_t)RECORD_INITIAL_LOOP || state == (uint8_t)RECORD_OVERDUB) {
        Debug->println("Looper is recording, boost off");
        setBoost(false);
        return;
    }
    if (state == (uint8_t)STOP_PLAYING || state == (uint8_t)DELETE) {
        Debug->println("Looper is stopped, boost off");
        setBoost(false);
    }
}

void MidiBoostLoop::setBoost(bool on) {
    if (on == boostActive) {
        return;
    }
    if (on) {
        baseGain = showbox->getInputGain(kInputChannel2);
        int step = nearestStep(baseGain) + kBoostSteps;
        if (step >= kStepCount) {
            step = kStepCount - 1;
        }
        float boosted = kGainSteps[step];
        showbox->setInputGain(kInputChannel2, boosted);
        boostActive = true;
        Debug->printf("Boost on: channel 2 %.1f -> %.1f\n", baseGain, boosted);
    } else {
        float restored = clampGain(baseGain);
        showbox->setInputGain(kInputChannel2, restored);
        boostActive = false;
        Debug->printf("Boost off: channel 2 restored to %.1f\n", restored);
    }
    saveBoostState();
    publishBoost();
}

void MidiBoostLoop::publishBoost() {
    if (midi == nullptr || publishing) {
        return;
    }
    publishing = true;
    uint8_t controller = boostActive ? boostOnCc : boostOffCc;
    midi->controlChange(controller, 127, static_cast<int8_t>(midiChannel));
    publishing = false;
}

void MidiBoostLoop::loadBoostState() {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
        if (prefs.isKey(kBoostActiveKey)) {
            boostActive = prefs.getBool(kBoostActiveKey, false);
            baseGain = prefs.getFloat(kBaseGainKey, 0.0f);
            prefs.end();
            return;
        }
        prefs.end();
    }
    // Older firmware stored the boost under this namespace. Copy it once so a
    // remembered boost is not applied on top of itself.
    Preferences legacy;
    if (!legacy.begin("airstep", true)) {
        return;
    }
    if (legacy.isKey(kBoostActiveKey) && legacy.getBool(kBoostActiveKey, false)) {
        boostActive = true;
        baseGain = legacy.getFloat(kBaseGainKey, 0.0f);
    }
    legacy.end();
    saveBoostState();
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
