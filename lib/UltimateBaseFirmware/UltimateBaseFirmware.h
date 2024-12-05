#ifndef ULTIMATE_BASE_FIRMWARE_H
#define ULTIMATE_BASE_FIRMWARE_H

#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
//#include <ElegantOTA.h>
#include <DynamicMultiPrint.h>
#include <RemoteDebug.h>
#include <ImprovWiFiLibrary.h>

#define USE_MDNS true
#if defined ESP8266
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#elif defined ESP32
#include <DNSServer.h>
#include "ESPmDNS.h"
#endif // ESP32

#include <ArduinoOTA.h>

class UltimateBaseFirmware {
public:
    // Constructor
    UltimateBaseFirmware(const char* name);

    // Setter methods for custom configurations
    void setCustomServer(AsyncWebServer* server);
    void setCustomWiFiManager(WiFiManager* wifiManager);
    void setCustomSerial(Stream& serial);
    void setCustomImprovSerial(ImprovWiFi* improvSerial);
    void setCustomDebugSerial(Print& debug);
    void setDeviceName(const char* deviceName);
    void setDeviceUrl(const char* deviceUrl);
    void setHostName(const char* hostName);
    void setVersion(const char* version);

    // Getter methods
    const char* getHostName();
    AsyncWebServer* getWebServer();

    // Main lifecycle methods
    void begin();
    void tick();

    // Utility methods
    String generateInfoString();

    // Debug serial
    //Print& Debug = Serial;
    DynamicMultiPrint Debug = DynamicMultiPrint();
private:
    const char* _name;
    const char* _version;

    AsyncWebServer* _server;
    WiFiManager* _wifiManager;
    Stream& _serial;
    ImprovWiFi* _improvSerial;
    const char* _deviceName;
    const char* _deviceUrl;
    const char* _hostName;
    const char* _chipFamily;
    bool _customDebugSerial = false;
    bool _customServer = false;
    wl_status_t _lastWiFiStatus = WL_DISCONNECTED;
    unsigned long _lastWifiCheck = 0;
    unsigned long _wifiCheckInterval = 100;
    bool _firstWiFiConnect = false;
    RemoteDebug _remoteDebug;
    unsigned long _remoteDebugLastConnect = 0;

    void initializeDebugSerial();
    void initializeDeviceInfo();
    void initializeWifi();
    void initializeWebServer();
    void initializeOTA();
    void initializeImprovSerial();
    void initializeMdns();
    void initializeCaptivePortal();

    void setDebugSerial(Print& debug);

    void checkWiFi();

    // Callbacks
    void onFirstWiFiConnect();
    void onWiFiConnected();
    void onWiFiDisconnected();
};

#endif // ULTIMATE_BASE_FIRMWARE_H