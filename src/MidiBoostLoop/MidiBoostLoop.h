#ifndef MIDI_BOOST_LOOP_H
#define MIDI_BOOST_LOOP_H

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"

// Channel 16 CC 102/103/104 footswitch looper, plus channel-2 gain boost.
class MidiBoostLoop {
public:
    using NoteCallback = void (*)(uint8_t channel, uint8_t note, uint8_t velocity);
    using ControlChangeCallback = void (*)(uint8_t channel, uint8_t controller, uint8_t value);
    using PitchBendCallback = void (*)(uint8_t channel, uint8_t value);

    // channel is 0-based.
    MidiBoostLoop(MackieShowbox* showbox, uint8_t channel,
        uint8_t recPlayOverdubCc, uint8_t stopCc, uint8_t deleteCc,
        uint8_t boostOnCc, uint8_t boostOffCc);

    void begin(MultiMidi* midi);
    void tick();
    void setDebugSerial(Print* serial);

    void setNextHandlers(NoteCallback noteOn, NoteCallback noteOff,
        ControlChangeCallback controlChange, PitchBendCallback pitchBend);

    bool isBoostActive() const;

private:
    enum class Mode : uint8_t {
        Empty,
        Recording,
        Overdub,
        Playing,
        Stopped
    };
    enum class Job : uint8_t {
        None,
        Tap,
        Tap2,
        Stop,
        Delete
    };

    static void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    static void onPitchBend(uint8_t channel, uint8_t value);

    void logIncoming(const char* kind, uint8_t channel, uint8_t a, uint8_t b);
    void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void sendLooperNow(uint8_t controller);
    void handleRecordPlayOverdub();
    void handleStop();
    void handleDelete();
    bool acceptPress();
    void syncModeFromLive();
    void queueJob(Job job);
    void runJob();
    void sendButton(looper_button_action action);
    const char* buttonName(looper_button_action action) const;
    Mode modeFromState(looper_state state) const;
    const char* modeName(Mode mode) const;
    const char* stateName(looper_state state) const;
    bool isLooperCc(uint8_t controller) const;
    bool isBoostCc(uint8_t controller) const;
    bool isAutoBoostCc(uint8_t controller) const;
    bool isBoostToggleCc(uint8_t controller) const;
    bool isBoostEcho(uint8_t controller, uint8_t value) const;
    bool autoBoostWanted() const;
    void toggleAutoBoost();
    void setBoost(bool on);
    void syncBoost();
    void applyBoost();
    void publishBoost();

    void loadBoostState();
    void saveBoostState();
    float clampGain(float gain) const;
    int nearestStep(float gain) const;

    MackieShowbox* showbox;
    MultiMidi* midi = nullptr;
    Print* Debug = &Serial;

    uint8_t midiChannel;
    uint8_t recPlayOverdubCc;
    uint8_t stopCc;
    uint8_t deleteCc;
    uint8_t boostOnCc;
    uint8_t boostOffCc;

    bool boostActive = false;
    bool boostWanted = false;
    bool boostApply = false;
    bool publishing = false;
    bool boostDirty = false;
    bool boostNeedAnnounce = false;
    bool autoBoostOn = false;
    bool autoBoostEnabled = true;
    uint8_t lastPublishedBoostCc = 0;
    uint32_t lastOutputGeneration = 0;
    uint32_t announceAt = 0;
    uint32_t ignoreBoostEchoUntil = 0;
    Mode mode = Mode::Empty;
    Job job = Job::None;
    uint8_t jobStep = 0;
    uint32_t jobAt = 0;
    uint32_t lastPressMs = 0;
    float baseGain = 0.0f;
    float boostedGain = 0.0f;

    NoteCallback nextNoteOn = nullptr;
    NoteCallback nextNoteOff = nullptr;
    ControlChangeCallback nextControlChange = nullptr;
    PitchBendCallback nextPitchBend = nullptr;

    static constexpr uint8_t kInputChannel2 = 1;
    static constexpr uint8_t kPressValue = 64;
    static constexpr uint8_t kAutoBoostCc = 107;
    static constexpr uint8_t kBoostToggleCc = 108;
    static constexpr int kBoostSteps = 2;
    static constexpr int kStepCount = 16;
    static constexpr float kMinGain = Showbox::InputGain::POSITION_1;
    static constexpr float kMaxGain = Showbox::InputGain::POSITION_16;
    static constexpr uint32_t kPressDebounceMs = 40;
    static constexpr uint32_t kBoostEchoMs = 1000;

    static MidiBoostLoop* instance;
};

#endif // MIDI_BOOST_LOOP_H
