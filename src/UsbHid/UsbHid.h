#pragma once

#include <stdint.h>

// USB keyboard and mouse. This takes the USB port, so serial/JTAG, USB MIDI device, and USB MIDI host stay off.
class UsbHid {
public:
    static bool start();
    static bool running();
    static bool sendText(const char* text);
    static bool sendMouse(int8_t x, int8_t y, uint8_t buttons);
};
