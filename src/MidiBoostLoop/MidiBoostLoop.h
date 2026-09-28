#ifndef MIDI_BOOST_LOOP_H
#define MIDI_BOOST_LOOP_H

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"

// Listens on every MIDI input for five Control Change presses.
// A press is value >= 64. A release of 0 is ignored.
// Boost adds three channel-2 input-gain steps, then restores the gain it captured.
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

    // Messages this module does not handle are forwarded here. Set before begin().
    void setNextHandlers(NoteCallback noteOn, NoteCallback noteOff,
        ControlChangeCallback controlChange, PitchBendCallback pitchBend);

    bool isBoostActive() const;

private:
    static void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    static void onPitchBend(uint8_t channel, uint8_t value);

    void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void handleRecordPlayOverdub();
    void handleStop();
    void handleDelete();
    void setBoost(bool on);
    void publishBoost();
    void applyLooperState(uint8_t state);

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
    bool publishing = false;
    bool stateSeeded = false;
    uint8_t lastLooperState = 0;
    uint32_t startedAt = 0;
    float baseGain = 0.0f;

    NoteCallback nextNoteOn = nullptr;
    NoteCallback nextNoteOff = nullptr;
    ControlChangeCallback nextControlChange = nullptr;
    PitchBendCallback nextPitchBend = nullptr;

    static constexpr uint8_t kInputChannel2 = 1;
    static constexpr uint8_t kPressValue = 64;
    static constexpr int kBoostSteps = 3;
    static constexpr int kStepCount = 16;
    static constexpr float kMinGain = Showbox::InputGain::POSITION_1;
    static constexpr float kMaxGain = Showbox::InputGain::POSITION_16;

    static MidiBoostLoop* instance;
};

#endif // MIDI_BOOST_LOOP_H
