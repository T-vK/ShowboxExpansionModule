#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// W5500 on the ESP32 IP stack. HTTP, OTA, mDNS, telnet, and AppleMIDI use
// the same sockets as Wi-Fi once this has an address.
class WiredNet {
public:
    void begin();
    void tick();
    bool started() const { return began; }
    bool linkUp() const;
    bool hasIp() const;
    IPAddress localIP() const;
    const char* status() const { return statusText; }

private:
    void setStatus(const char* text);
    bool began = false;
    char statusText[96] = "Off";
};

bool lanHasIp();
IPAddress lanIP();
