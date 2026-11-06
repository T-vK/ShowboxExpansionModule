#pragma once

#include <stdint.h>

namespace midi {
struct MidiMessage;
class MidiAction;
}

// USB MIDI host. Uses the same USB port as USB MIDI device, USB HID, and serial/JTAG.
class UsbMidiHost {
public:
    static bool start();
    static bool running();
    static bool connected();
    static const char* status();
    static const char* deviceName();
    static uint32_t rxCount();
    static uint32_t lastRxMs();
    static void tick(midi::MidiAction* action);
    static void write(midi::MidiMessage* msg, int len);
};
