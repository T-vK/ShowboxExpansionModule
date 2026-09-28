#pragma once

namespace midi {
struct MidiMessage;
class MidiAction;
}

// USB MIDI host. Uses the same USB port as USB MIDI device, USB HID, and serial/JTAG.
class UsbMidiHost {
public:
    static bool start();
    static bool running();
    static const char* status();
    static void tick(midi::MidiAction* action);
    static void write(midi::MidiMessage* msg, int len);
};
