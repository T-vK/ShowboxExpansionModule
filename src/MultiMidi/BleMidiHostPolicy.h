#pragma once

// Decides which advertising peripherals the BLE MIDI host should connect to.
// The app implements this. MultiMidi does not store bonds itself.
class BleMidiHostPolicy {
public:
    virtual ~BleMidiHostPolicy() = default;
    virtual bool shouldConnect(const char* name, const char* address) = 0;
};
