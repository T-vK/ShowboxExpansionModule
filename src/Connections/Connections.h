#pragma once

#include <Arduino.h>
#include "BlePeers/BlePeers.h"
#include "BleHid/BleHid.h"
#include "MultiMidi/BleMidiHostPolicy.h"

class MultiMidi;

// Pairing, transport switches, and HID sending.
// MIDI transports live inside MultiMidi.
class Connections : public BleMidiHostPolicy {
public:
    void attach(MultiMidi* midi);
    void load();
    void startAfterBle();
    void tick();

    bool shouldConnect(const char* name, const char* address) override;

    String statusJson();
    bool setOption(const String& name, bool enabled, String& error);
    void requestScan();
    bool connect(const String& address, const String& name, int midi, int hid, String& error);
    bool pair(const String& address, const String& name, String& error);
    bool setPeerRoles(const String& address, int midi, int serialMidi, int hid, String& error);
    bool disconnectAddress(const String& address, String& error);
    bool forget(const String& address, String& error);
    bool setPeerAuto(const String& address, bool enabled, String& error);
    bool queueKeys(const String& text, String& error);
    bool queueMouse(int x, int y, int buttons, String& error);
    bool usbDeviceOn() const { return usbDeviceEnabled; }
    bool usbHostOn() const { return usbHostEnabled; }
    bool usbHidOn() const { return usbHidEnabled; }

private:
    void saveHid();
    void saveUsbMode(bool device, bool host, bool hid);
    void scheduleReboot(const char* reason);
    bool sameAddress(const char* left, const char* right) const;
    bool anyHostHid() const;

    MultiMidi* multiMidi = nullptr;
    BlePeers peers;
    BleHid hid;
    bool hidEnabled = false;
    bool usbDeviceEnabled = false;
    bool usbHostEnabled = false;
    bool usbHidEnabled = false;
    bool rebootPending = false;
    uint32_t rebootAt = 0;
};
