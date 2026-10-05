#include "WiredNet.h"

#include "pin_definitions.h"
#include <ETH.h>
#include <Network.h>
#include <SPI.h>
#include <WiFi.h>
#include "DebugLog.h"

namespace {
bool gotIp = false;
bool cableUp = false;
}

void WiredNet::setStatus(const char* text) {
    strncpy(statusText, text, sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
}

void WiredNet::begin() {
    if (began) {
        return;
    }
    Network.onEvent([](arduino_event_id_t event, arduino_event_info_t /*info*/) {
        if (event == ARDUINO_EVENT_ETH_START) {
            ETH.setHostname("showbox");
        } else if (event == ARDUINO_EVENT_ETH_CONNECTED) {
            cableUp = true;
        } else if (event == ARDUINO_EVENT_ETH_GOT_IP) {
            cableUp = true;
            gotIp = true;
        } else if (event == ARDUINO_EVENT_ETH_LOST_IP || event == ARDUINO_EVENT_ETH_DISCONNECTED
            || event == ARDUINO_EVENT_ETH_STOP) {
            if (event != ARDUINO_EVENT_ETH_LOST_IP) {
                cableUp = false;
            }
            gotIp = false;
        }
    });
    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI);
    if (!ETH.begin(ETH_PHY_W5500, 1, W5500_nSS, W5500_nINT, W5500_nRESET, SPI)) {
        setStatus("Ethernet PHY did not start");
        debugLog("eth begin failed");
        return;
    }
    began = true;
    setStatus("Ethernet is up, waiting for a link");
    debugLog("eth begin ok");
}

void WiredNet::tick() {
    if (!began) {
        return;
    }
    if (hasIp()) {
        IPAddress ip = ETH.localIP();
        snprintf(statusText, sizeof(statusText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else if (linkUp()) {
        setStatus("Ethernet is up, waiting for DHCP");
    } else {
        setStatus("Ethernet is up, waiting for a link");
    }
}

bool WiredNet::linkUp() const {
    return began && (cableUp || ETH.linkUp());
}

bool WiredNet::hasIp() const {
    return began && (gotIp || ETH.hasIP()) && ETH.localIP() != IPAddress();
}

IPAddress WiredNet::localIP() const {
    return hasIp() ? ETH.localIP() : IPAddress();
}

bool lanHasIp() {
    return WiFi.localIP() != IPAddress() || (ETH.hasIP() && ETH.localIP() != IPAddress());
}

IPAddress lanIP() {
    if (WiFi.localIP() != IPAddress()) {
        return WiFi.localIP();
    }
    if (ETH.hasIP() && ETH.localIP() != IPAddress()) {
        return ETH.localIP();
    }
    return IPAddress();
}
