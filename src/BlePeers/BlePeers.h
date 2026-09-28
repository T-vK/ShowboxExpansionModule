#pragma once

#include <Arduino.h>

// Saved Bluetooth devices and the role this module uses with each one.
// midi: connect out to that device as a BLE MIDI host.
// hid: that device is a computer using this module as a keyboard and mouse.
class BlePeers {
public:
    static constexpr int kMax = 8;

    struct Peer {
        char name[32];
        char address[18];
        bool midi;
        bool serialMidi;
        bool hid;
        bool autoConnect;
        bool host;
    };

    void load();
    int copy(Peer* out, int max) const;
    bool find(const char* address, Peer* out) const;
    // Negative values leave that flag unchanged. serialMidi stays off on this chip.
    bool upsert(const char* address, const char* name, int midi, int hid, int autoConnect, int serialMidi = -1, int host = -1);
    bool forget(const char* address);
    int midiPeerCount() const;
    bool matchesMidiAuto(const char* address) const;

private:
    void save() const;
    int indexOf(const char* address) const;

    Peer peers[kMax] = {};
    int count = 0;
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
