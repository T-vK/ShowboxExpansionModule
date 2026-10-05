#include "EthernetMidi.h"

#include <ETH.h>
#include <cstring>

void EthernetMidi::setDebug(Print* serial) {
    Debug = serial;
}

void EthernetMidi::setPins(uint8_t /*sclk*/, uint8_t /*miso*/, uint8_t /*mosi*/, uint8_t /*cs*/, uint8_t /*reset*/) {
    pinsSet = true;
}

void EthernetMidi::setEnabled(bool enabled) {
    enabledFlag = enabled;
    if (!enabled) {
        setStatus("Off");
    }
}

void EthernetMidi::setStatus(const char* text) {
    strncpy(statusText, text, sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
}

void EthernetMidi::tick(midi::MidiAction* /*action*/) {
    if (!enabledFlag) {
        setStatus("Off");
        return;
    }
    if (ETH.hasIP() && ETH.localIP() != IPAddress()) {
        IPAddress ip = ETH.localIP();
        snprintf(statusText, sizeof(statusText), "%u.%u.%u.%u, ports 5004 and 5005",
            ip[0], ip[1], ip[2], ip[3]);
        haveIp = true;
        linkUp = true;
        return;
    }
    haveIp = false;
    if (ETH.linkUp()) {
        linkUp = true;
        setStatus("Ethernet is up, waiting for DHCP");
        return;
    }
    linkUp = false;
    setStatus("Ethernet is up, waiting for a link");
}

void EthernetMidi::write(midi::MidiMessage* /*msg*/, int /*len*/) {
    // AppleMIDI on lwIP already sends on every interface that has a route.
}
