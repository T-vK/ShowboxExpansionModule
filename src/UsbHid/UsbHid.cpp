#include "UsbHid.h"

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

namespace {
bool started = false;
USBHIDKeyboard* keyboard = nullptr;
USBHIDMouse* mouse = nullptr;
}

bool UsbHid::start() {
    if (started) {
        return true;
    }
    keyboard = new USBHIDKeyboard();
    mouse = new USBHIDMouse();
    USB.manufacturerName("Showbox");
    USB.productName("ShowboxMod");
    USB.VID(0x303A);
    USB.PID(0x0002);
    if (!USB.begin()) {
        return false;
    }
    keyboard->begin();
    mouse->begin();
    started = true;
    return true;
}

bool UsbHid::running() {
    return started;
}

bool UsbHid::sendText(const char* text) {
    if (!started || keyboard == nullptr || text == nullptr) {
        return false;
    }
    return keyboard->print(text) > 0 || text[0] == '\0';
}

bool UsbHid::sendMouse(int8_t x, int8_t y, uint8_t buttons) {
    if (!started || mouse == nullptr) {
        return false;
    }
    mouse->move(x, y);
    if (buttons & 0x01) {
        mouse->click(MOUSE_LEFT);
    }
    return true;
}
