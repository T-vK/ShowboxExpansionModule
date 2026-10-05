#pragma once

    // Decides which advertising peripherals the BLE MIDI host should connect to.
// The app implements this. MultiMidi does not store bonds itself.
class BleMidiHostPolicy {
public:
    virtual ~BleMidiHostPolicy() = default;
    virtual bool shouldConnect(const char* name, const char* address) = 0;
    // 0 skip, 1 auto, 2 reserved/pinned auto.
    virtual int connectScore(const char* name, const char* address) {
        return shouldConnect(name, address) ? 1 : 0;
    }
    virtual int maxOutgoing() { return 2; }
    // True when a saved peripheral has Auto Connect. Background scans stay off otherwise.
    virtual bool wantsAutoScan() { return false; }
    // Next remembered Auto Connect peripheral that is not live. Pinned first.
    virtual bool nextAutoOutgoing(char* address, size_t addressSize, char* name, size_t nameSize) {
        (void)address;
        (void)addressSize;
        (void)name;
        (void)nameSize;
        return false;
    }
    // Drop the newest unreserved peripheral so a reserved one can take the slot.
    virtual bool yieldOutgoing() { return false; }
    virtual void note(const char* text) {
        (void)text;
    }
};
