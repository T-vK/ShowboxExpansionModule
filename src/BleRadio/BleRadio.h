#pragma once

#include <Arduino.h>
#include <BLEServer.h>

// Shared BLE advertising for MIDI peripheral and HID peripheral.
// One radio, one name (ShowboxMod). Each service can be included or left out.
class BleRadio : public BLEServerCallbacks {
public:
    static constexpr int kMaxIncoming = 4;

    struct Incoming {
        uint16_t connId;
        char address[18];
    };

    static BleRadio& instance();

    void setMidiPeripheral(bool enabled);
    void setHidPeripheral(bool enabled);
    bool midiPeripheral() const;
    bool hidPeripheral() const;
    void apply();
    void restartWhenIdle();
    void service();
    void attach(BLEServer* server);

    int copyIncoming(Incoming* out, int max) const;
    bool disconnectIncoming(const char* address);

    void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override;
    void onDisconnect(BLEServer* server, ble_gap_conn_desc* desc) override;

private:
    BLEServer* server = nullptr;
    bool midiOn = true;
    bool hidOn = false;
    bool advertised = false;
    bool advertisedMidi = false;
    bool advertisedHid = false;
    volatile bool restartAdvertising = false;
    volatile bool pendingInterval = false;
    uint16_t pendingHandle = 0;
    Incoming incoming[kMaxIncoming] = {};
    int incomingCount = 0;
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
