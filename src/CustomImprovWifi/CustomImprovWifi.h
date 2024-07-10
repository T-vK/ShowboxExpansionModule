#ifndef CUSTOM_IMPROV_WIFI_H
#define CUSTOM_IMPROV_WIFI_H

#include <WiFi.h>
#include "ImprovWiFiLibrary.h"

class CustomImprovWifi {
public:
    CustomImprovWifi();
    void setupImprov();
    void handleLoop();
    static bool connectWifi(const char* ssid, const char* password);
    
private:
    static void onImprovWiFiErrorCb(ImprovTypes::Error err);
    static void onImprovWiFiConnectedCb(const char* ssid, const char* password);
    void blinkLed(int d, int times);
    static CustomImprovWifi* instance;  // Pointer to the current instance
};

#endif // CUSTOM_IMPROV_WIFI_H
