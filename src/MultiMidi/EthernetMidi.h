#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace midi {
struct MidiMessage;
class MidiAction;
}

// Ethernet MIDI status. RTP-MIDI / AppleMIDI uses the shared lwIP sockets
// once the W5500 is on the ESP32 IP stack.
class EthernetMidi {
public:
    void setDebug(Print* serial);
    void setPins(uint8_t sclk, uint8_t miso, uint8_t mosi, uint8_t cs, uint8_t reset);
    void setEnabled(bool enabled);
    bool enabled() const { return enabledFlag; }
    const char* status() const { return statusText; }
    void tick(midi::MidiAction* action);
    void write(midi::MidiMessage* msg, int len);

private:
    void setStatus(const char* text);

    Print* Debug = nullptr;
    bool pinsSet = false;
    bool enabledFlag = false;
    bool linkUp = false;
    bool haveIp = false;
    char statusText[96] = "Off";
};
