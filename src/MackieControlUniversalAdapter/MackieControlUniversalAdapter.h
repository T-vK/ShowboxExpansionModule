#ifndef MACKIE_CONTROL_UNIVERSAL_ADAPTER_H
#define MACKIE_CONTROL_UNIVERSAL_ADAPTER_H

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"

class MackieControlUniversalAdapter {
public:
    // Constructor
    MackieControlUniversalAdapter(MackieShowbox* showbox);

    // Initialization method
    void begin();

    // Update method, to be called in the loop
    void tick();

    // Set the debug serial
    void setDebugSerial(Print* serial);

    // Set midi object
    void setMidi(MultiMidi* midi);

    // MCU state (menu, channel, etc.)
    

private:
    MackieShowbox* showbox;
    Print* Debug = &Serial;
    MultiMidi* midi = nullptr;
    MidiCallbackAction action;

    // Midi callbacks
    static void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    static void onPitchBend(uint8_t channel, uint8_t value);    
    static MackieControlUniversalAdapter* instance;
};

#endif // MACKIE_CONTROL_UNIVERSAL_ADAPTER_H