#pragma once

#include <Arduino.h>

class Print;
namespace midi { class MidiAction; }

// Bluetooth Classic Serial Port Profile MIDI.
// ESP32-S3 has no Classic radio, so supported() is false there and nothing is advertised.
// The transport stays in MultiMidi so the library can turn it on for chips that have Classic.
class BleClassicMidi {
public:
    static bool supported();
    bool enable(const char* name);
    void begin(midi::MidiAction* action, Print* debug);
    void tick();
    void write(const uint8_t* data, int length);
    bool isEnabled() const;

private:
    bool enabled = false;
    const char* deviceName = "BleMidi";
    midi::MidiAction* action = nullptr;
    Print* debug = nullptr;
};
