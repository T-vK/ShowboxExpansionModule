#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace midi {
struct MidiMessage;
class MidiAction;
}

// RTP-MIDI / AppleMIDI on the W5500. Wi-Fi AppleMIDI keeps its own sockets.
class EthernetMidi {
public:
    void setDebug(Print* serial);
    void setEnabled(bool enabled);
    bool enabled() const { return enabledFlag; }
    const char* status() const { return statusText; }
    void tick(midi::MidiAction* action);
    void write(midi::MidiMessage* msg, int len);

private:
    void startChip();
    void pollLink();
    void pollDhcp();
    void openMidiSockets();
    void handleSession(const uint8_t* data, int length, const uint8_t ip[4], uint16_t port, bool dataPort);
    void handleMidi(const uint8_t* data, int length, midi::MidiAction* action);
    void setStatus(const char* text);
    bool sendTo(int sock, const uint8_t dest[4], uint16_t port, const uint8_t* data, int len);
    int recvFrom(int sock, uint8_t from[4], uint16_t* port, uint8_t* data, int maxLen);

    Print* Debug = nullptr;
    bool enabledFlag = false;
    bool chipUp = false;
    bool linkUp = false;
    bool haveIp = false;
    bool socketsOpen = false;
    bool session = false;
    uint8_t mac[6] = {};
    uint8_t ip[4] = {};
    uint8_t gateway[4] = {};
    uint8_t mask[4] = {};
    uint8_t peerIp[4] = {};
    uint16_t controlPort = 0;
    uint16_t dataPort = 0;
    uint32_t ourSsrc = 0x53584D31;
    uint16_t sequence = 0;
    uint32_t dhcpXid = 0;
    uint8_t dhcpServer[4] = {};
    uint8_t offeredIp[4] = {};
    uint32_t leaseMs = 0;
    uint32_t dhcpDeadline = 0;
    uint32_t renewAt = 0;
    int dhcpState = 0;
    uint32_t chipTriedAt = 0;
    char statusText[96] = "Off";
};
