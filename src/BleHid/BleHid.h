#pragma once

#include <Arduino.h>

class BLEHIDDevice;
class BLECharacteristic;
class BLEServer;

// BLE HID peripheral: this module is a keyboard and mouse.
// A computer connects to ShowboxMod. Reports are queued and sent from tick().
class BleHid {
public:
    void begin(BLEServer* server, Print* debug);
    void setEnabled(bool enabled);
    bool enabled() const;
    bool ready() const;
    void tick();

    bool queueText(const char* text);
    bool queueMouse(int8_t x, int8_t y, uint8_t buttons);

private:
    struct Report {
        uint8_t id;
        uint8_t data[8];
        uint8_t length;
    };

    bool ensureStarted();
    bool push(uint8_t id, const uint8_t* data, uint8_t length);
    void send(const Report& report);

    Print* debug = &Serial;
    BLEServer* server = nullptr;
    BLEHIDDevice* device = nullptr;
    BLECharacteristic* keyboard = nullptr;
    BLECharacteristic* mouse = nullptr;
    bool started = false;
    bool on = false;
    bool securityReady = false;
    unsigned long lastSentMs = 0;

    static constexpr int kQueueSize = 64;
    Report queue[kQueueSize] = {};
    int queueHead = 0;
    int queueCount = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
