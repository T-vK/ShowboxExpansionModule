#include "UltimateBaseFirmware.h"
#include <WiFi.h>
#include <esp_wifi.h>

UltimateBaseFirmware::UltimateBaseFirmware(const char* name)
    : _name(name), _version(nullptr), 
      _server(nullptr), _wifiManager(nullptr),
      _serial(Serial), _improvSerial(nullptr), Debug() {
        Debug.setPrint1(&Serial);
        _wifiLog.attach(&Debug);
      }

// Setter methods for customization
void UltimateBaseFirmware::setVersion(const char* version) {
    _version = version;
}

void UltimateBaseFirmware::setHostName(const char* hostName) {
    _hostName = hostName;
}

void UltimateBaseFirmware::setCustomServer(AsyncWebServer* server) {
    _server = server;
    _customServer = true;
}

void UltimateBaseFirmware::setCustomWiFiManager(WiFiManager* wifiManager) {
    _wifiManager = wifiManager;
}

void UltimateBaseFirmware::setCustomSerial(Stream& serial) {
    _serial = serial;
}

void UltimateBaseFirmware::setCustomImprovSerial(ImprovWiFi* improvSerial) {
    _improvSerial = improvSerial;
}

void UltimateBaseFirmware::setCustomDebugSerial(Print& debug) {
    Debug.setPrint1(&debug);
    Debug.setPrint2(nullptr);
    Debug.setPrint2Live(false);
    Debug.setBuffer2(false);
    _customDebugSerial = true;
}

void UltimateBaseFirmware::setWifiLogHistory(bool enable) {
    _wifiLogHistory = enable;
    if (!_customDebugSerial) {
        Debug.setBuffer2(enable);
    }
}

void UltimateBaseFirmware::setDeviceName(const char* deviceName) {
    _deviceName = deviceName;
}

void UltimateBaseFirmware::setDeviceUrl(const char* deviceUrl) {
    _deviceUrl = deviceUrl;
}

// Getter methods
AsyncWebServer* UltimateBaseFirmware::getWebServer() {
    return _server;
}

const char* UltimateBaseFirmware::getHostName() {
    return _hostName;
}

// Begin method to initialize components
void UltimateBaseFirmware::begin() {
    if (!_customDebugSerial) {
        Debug.setBuffer2(_wifiLogHistory);
    }
    initializeDeviceInfo();
    String info = generateInfoString();
    Debug.println(info);
    
    Debug.println("[  OK  ] Initializing ImprovSerial...");
    initializeImprovSerial();
    Debug.println("[ INFO ] ImprovSerial initialized.");

    Debug.println("[ INFO ] Initializing WifiManager...");
    initializeWifi();
    Debug.println(_wifiRadioEnabled ? "[  OK  ] WifiManager initialized." : "[ INFO ] Wi-Fi radio is off.");
}

// Main loop for periodic tasks (e.g., OTA updates, Improv WiFi, Remote Debug)
void UltimateBaseFirmware::tick() {
    if (_improvSerial) {
        _improvSerial->handleSerial();
    }
    if (_wifiManager) {
        _wifiManager->process();
    }
    //ElegantOTA.loop();
    checkWiFi();
    if (_lanServicesUp) {
        ArduinoOTA.handle();
        if (!_customDebugSerial) {
            // A telnet client whose peer vanished (laptop asleep, half-open PCB)
            // makes every Debug print block in lwip_select and starves the main
            // loop. The loop-stall detector quarantines telnet writes; while
            // quarantined the stalled client is dropped and history stays
            // buffered until it reconnects.
            if (_telnetQuarantineUntil != 0) {
                if ((int32_t)(millis() - _telnetQuarantineUntil) >= 0) {
                    _telnetQuarantineUntil = 0;
                    _telnetQuarantineDropped = false;
                } else {
                    if (!_telnetQuarantineDropped) {
                        _remoteDebug.disconnect(true);
                        _telnetQuarantineDropped = true;
                    }
                    Debug.setPrint2Live(false);
                    _remoteDebugWasConnected = false;
                    return;
                }
            }
            _remoteDebug.handle();
            bool connected = _remoteDebug.isConnected();
            Debug.setPrint2Live(connected);
            if (connected && !_remoteDebugWasConnected) {
                size_t buffered = Debug.wifiBuffered();
                uint32_t dropped = Debug.wifiDropped();
                if (buffered > 0) {
                    _remoteDebug.printf("\n--- Wi-Fi serial history: %u bytes", static_cast<unsigned>(buffered));
                    if (dropped > 0) {
                        _remoteDebug.printf(" (%u oldest dropped)", static_cast<unsigned>(dropped));
                    }
                    _remoteDebug.print(" ---\n");
                } else {
                    _remoteDebug.print("\n--- Wi-Fi serial connected ---\n");
                }
            }
            if (connected) {
                Debug.drainPrint2(8192);
            }
            _remoteDebugWasConnected = connected;
        }
    }
}

void UltimateBaseFirmware::quarantineTelnet(uint32_t ms) {
    _telnetQuarantineUntil = millis() + ms;
    _telnetQuarantineDropped = false;
}

WiFiClient* UltimateBaseFirmware::telnetClient() {
    return _remoteDebug.getTelnetClient();
}

void UltimateBaseFirmware::flushWifiLog() {
    if (_customDebugSerial || !_lanServicesUp) {
        return;
    }
    if (!_remoteDebug.isConnected()) {
        return;
    }
    Debug.setPrint2Live(true);
    for (int i = 0; i < 8; i++) {
        if (Debug.drainPrint2(2048) == 0) {
            break;
        }
    }
    WiFiClient* client = _remoteDebug.getTelnetClient();
    if (client != nullptr && client->connected()) {
        client->flush();
    }
}

// Check WiFi connection
void UltimateBaseFirmware::checkWiFi() {
    if (!_wifiRadioEnabled) {
        return;
    }
    if (millis() - _lastWifiCheck > _wifiCheckInterval) {
        wl_status_t status = WiFi.status();
        if (status != _lastWiFiStatus) {
            if (status == WL_CONNECTED) {
                onWiFiConnected();
                if (!_firstWiFiConnect) {
                    onFirstWiFiConnect();
                    _firstWiFiConnect = true;
                }
            } else {
                onWiFiDisconnected();
            }
            _lastWiFiStatus = status;
        }
        _lastWifiCheck = millis();
    }
}

void UltimateBaseFirmware::startLanServices() {
    if (_lanServicesUp) {
        return;
    }
    _lanServicesUp = true;
    if (!_customDebugSerial) {
        Debug.println("[ INFO ] Initializing Debug Serial...");
        initializeDebugSerial();
        Debug.println("[  OK  ] Debug Serial initialized.");
    }

    Debug.println("[ INFO ] Preparing Web Server...");
    initializeWebServer();
    Debug.println("[ INFO ] Web Server prepared.");

    Debug.println("[  OK  ] Initializing OTA...");
    initializeOTA();
    Debug.println("[ INFO ] OTA initialized.");

    if (_server != nullptr) {
        Debug.println("[ INFO ] Starting Web Server...");
        _server->begin();
        Debug.println("[  OK  ] Web Server started.");
    }

    Debug.println("[ INFO ] Initializing MDNS...");
    initializeMdns();
    Debug.println("[  OK  ] MDNS initialized.");
}

// Callback for first WiFi connection
void UltimateBaseFirmware::onFirstWiFiConnect() {
    _wifiCheckInterval = 5000;
    startLanServices();
}

// Callback for WiFi connected
void UltimateBaseFirmware::onWiFiConnected() {
    Debug.println("[ INFO ] WiFi connected!");
}

// Callback for WiFi disconnected
void UltimateBaseFirmware::onWiFiDisconnected() {
    Debug.println("[ INFO ] WiFi disconnected!");
}

// Initialize Debug Serial
void UltimateBaseFirmware::initializeDebugSerial() {
    _remoteDebug.setResetCmdEnabled(true);
    _remoteDebug.setSerialEnabled(false);
    _remoteDebug.showRaw(true);
    _remoteDebug.begin(_hostName);
    Debug.setPrint1(&_serial);
    Debug.setPrint2(&_remoteDebug);
}

// Initialize Device Info
void UltimateBaseFirmware::initializeDeviceInfo() {
    #if defined(ESP32)
        #if CONFIG_IDF_TARGET_ESP32
            _chipFamily = "ESP32";
        #elif CONFIG_IDF_TARGET_ESP32C3
            _chipFamily = "ESP32-C3";
        #elif CONFIG_IDF_TARGET_ESP32S2
            _chipFamily = "ESP32-S2";
        #elif CONFIG_IDF_TARGET_ESP32S3
            _chipFamily = "ESP32-S3";
        #else
            return;
        #endif
    #elif defined(ESP8266)
        _chipFamily = "ESP8266";
    #else
        return;
    #endif

    if ( _version == nullptr) {
        _version = "0.0.0";
    }

    if (_deviceName == nullptr) {
        _deviceName = _chipFamily;
    }

    if (_deviceUrl == nullptr) {
        _deviceUrl = "http://{LOCAL_IPV4}/";
    }

    if (_hostName == nullptr) { // if not set, then set it to _name + ".local" as CONST char* not char*
        _hostName = (const char*)malloc(strlen(_name) + 7); // 7 is the length of ".local" + null terminator
        strcpy((char*)_hostName, _name);
        strcat((char*)_hostName, ".local");
    }
}

void UltimateBaseFirmware::setWifiRadioEnabled(bool enabled) {
    _wifiRadioEnabled = enabled;
}

void UltimateBaseFirmware::enableWifiRadio() {
    _wifiRadioEnabled = true;
    if (_wifiManager == nullptr) {
        initializeWifi();
        return;
    }
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    WiFi.reconnect();
}

void UltimateBaseFirmware::disableWifiRadio() {
    stopSetupPortal();
    _wifiRadioEnabled = false;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    Debug.println("[ INFO ] Wi-Fi radio is off.");
}

bool UltimateBaseFirmware::startSetupPortal(uint32_t timeoutMs) {
    if (!_wifiRadioEnabled) {
        return false;
    }
    if (_wifiManager == nullptr) {
        initializeWifi();
    }
    if (_wifiManager == nullptr) {
        return false;
    }
    _wifiManager->setConfigPortalTimeout((timeoutMs + 999) / 1000);
    _wifiManager->startConfigPortal("ShowboxExpansionModuleAP", "showbox");
    return _wifiManager->getConfigPortalActive();
}

void UltimateBaseFirmware::stopSetupPortal() {
    if (_wifiManager != nullptr) {
        _wifiManager->stopConfigPortal();
    }
}

bool UltimateBaseFirmware::setupPortalOpen() const {
    return _wifiManager != nullptr && _wifiManager->getConfigPortalActive();
}

// Initialize WiFi in both STA and AP modes, with saved credentials via ImprovWiFi
void UltimateBaseFirmware::initializeWifi() {
    if (!_wifiRadioEnabled) {
        WiFi.mode(WIFI_OFF);
        Debug.println("[ INFO ] Wi-Fi radio is off.");
        return;
    }
    if (_wifiManager == nullptr) {
        _wifiManager = new WiFiManager(Debug);  // Create a default WiFiManager instance if not set
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.hostname(_hostName);
    _wifiManager->setHttpPort(8080);
    _wifiManager->setConfigPortalBlocking(false);
    _wifiManager->setConfigPortalTimeout(0);
    _wifiManager->setEnableConfigPortal(false);

    if (_wifiManager->autoConnect("ShowboxExpansionModuleAP", "showbox")) {
        Debug.println("Connected to WiFi!");
    } else {
        Debug.println("Failed to connect to WiFi. Mixer hotspot will stay up.");
    }
}

// Generate info string
String UltimateBaseFirmware::generateInfoString() {
    String info = "Firmware Name: " + String(_name) + "\n";
    info += "Firmware Version: " + String(_version) + "\n";
    info += "Chip Family: " + String(_chipFamily) + "\n";
    info += "Device Name: " + String(_deviceName) + "\n";
    info += "Host Name: " + String(_hostName) + "\n";
    info += "Device URL: http://" + String(_hostName) + ".local/\n";
    return info;
}

// Initialize Web Server (Async WebServer)
void UltimateBaseFirmware::initializeWebServer() {
    if (_server == nullptr) {
        _server = new AsyncWebServer(80);
    }

    _server->on("/ubfw", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String response = generateInfoString();
        request->send(200, "text/plain", response);
    });
}

// Initialize OTA support
void UltimateBaseFirmware::initializeOTA() {
    //ElegantOTA.begin(_server);
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
            type = "sketch";
        else // U_SPIFFS
            type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("[ INFO ] Start updating " + type);
    }).onEnd([]() {
        Serial.println("\nEnd");
    }).onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[ INFO ] Progress: %u%%\r", (progress / (total / 100)));
    }).onError([](ota_error_t error) {
        Serial.print("[FAILED] ");
        if (error == OTA_AUTH_ERROR) Serial.print("Auth error");
        else if (error == OTA_BEGIN_ERROR) Serial.print("Begin error");
        else if (error == OTA_CONNECT_ERROR) Serial.print("Connect error");
        else if (error == OTA_RECEIVE_ERROR) Serial.print("Receive error");
        else if (error == OTA_END_ERROR) Serial.print("End error");
        else Serial.print("Unknown error");
        Serial.printf(" - Error code: %u\n", error);
    });

    ArduinoOTA.begin();
}

// Initialize ImprovWiFi
void UltimateBaseFirmware::initializeImprovSerial() {
    if (_improvSerial == nullptr) {
        // use improvWifi with _serial
        Stream *serialPointer = &_serial;
        _improvSerial = new ImprovWiFi(serialPointer);
    }

    ImprovTypes::ChipFamily chipFamilyImprovType;

    #if defined(ESP32)
        #if CONFIG_IDF_TARGET_ESP32
            chipFamilyImprovType = ImprovTypes::ChipFamily::CF_ESP32;
        #elif CONFIG_IDF_TARGET_ESP32C3
            chipFamilyImprovType = ImprovTypes::ChipFamily::CF_ESP32_C3;
        #elif CONFIG_IDF_TARGET_ESP32S2
            chipFamilyImprovType = ImprovTypes::ChipFamily::CF_ESP32_S2;
        #elif CONFIG_IDF_TARGET_ESP32S3
            chipFamilyImprovType = ImprovTypes::ChipFamily::CF_ESP32_S3;
        #else
            return;
        #endif
    #elif defined(ESP8266)
        chipFamilyImprovType = ImprovTypes::ChipFamily::CF_ESP8266;
    #else
        return;
    #endif

    _improvSerial->setDeviceInfo(chipFamilyImprovType, _name, _version, _deviceName, _deviceUrl);
}

// Initialize MDNS
void UltimateBaseFirmware::initializeMdns() {
    // mDNS is started from keepMdns() after the HTTP routes are up. Doing it
    // here races Wi-Fi and then Bluetooth, which drops UDP 5353.
}