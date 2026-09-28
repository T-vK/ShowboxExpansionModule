#pragma once

#include <stdint.h>

namespace midi {
struct MidiMessage;
class MidiAction;
}

// USB MIDI device on the ESP32-S3 USB port.
// That port is shared with USB serial/JTAG, so this only starts when the
// setting was saved and the sketch skipped the serial/JTAG port.
class UsbMidi {
public:
    static bool start();
    static bool running();
    static void tick(midi::MidiAction* action);
    static void write(midi::MidiMessage* msg, int len);
};
