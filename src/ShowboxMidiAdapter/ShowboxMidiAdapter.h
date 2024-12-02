#ifndef SHOWBOX_MIDI_ADAPTER_H
#define SHOWBOX_MIDI_ADAPTER_H

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"

class ShowboxMidiAdapter {
public:
    // Constructor
    ShowboxMidiAdapter(MackieShowbox* showbox);

    // Initialization method allows passing hw rx/tx pins
    void begin(uint8_t rxPin, uint8_t txPin);

    // Update method, to be called in the loop
    void tick();

    // Set the debug serial
    void setDebugSerial(Print* serial);

private:
    MackieShowbox* showbox;
    Print* Debug = &Serial;
    MultiMidi midi = MultiMidi();
    MidiCallbackAction action;

    // Callback methods
    static void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    static void onPitchBend(uint8_t channel, uint8_t value);
    
    static ShowboxMidiAdapter* instance;
};

#endif // SHOWBOX_MIDI_ADAPTER_H
