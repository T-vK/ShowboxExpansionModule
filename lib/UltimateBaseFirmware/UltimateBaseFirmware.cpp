#include "UltimateBaseFirmware.h"

UltimateBaseFirmware::UltimateBaseFirmware(const char* name)
    : _name(name), _version(nullptr), 
      _server(nullptr), _wifiManager(nullptr),
      _serial(Serial), _improvSerial(nullptr), Debug() {
        Debug.setPrint1(&Serial);
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
    _customDebugSerial = true;
}

void UltimateBaseFirmware::setDeviceName(const char* deviceName) {
    _deviceName = deviceName;
}

void UltimateBaseFirmware::setDeviceUrl(const char* deviceUrl) {
    _deviceUrl = deviceUrl;
}

// Begin method to initialize components
void UltimateBaseFirmware::begin() {
    initializeDeviceInfo();
    String info = generateInfoString();
    Debug.println(info);
    
    Debug.println("[  OK  ] Initializing ImprovSerial...");
    initializeImprovSerial();
    Debug.println("[ INFO ] ImprovSerial initialized.");

    Debug.println("[ INFO ] Initializing WifiManager...");
    initializeWifi();
    Debug.println("[  OK  ] WifiManager initialized.");
}

// Main loop for periodic tasks (e.g., OTA updates, Improv WiFi, Remote Debug)
void UltimateBaseFirmware::loop() {
    if (_improvSerial) {
        _improvSerial->handleSerial();
    }
    if (_wifiManager) {
        _wifiManager->process();
    }
    //ElegantOTA.loop();
    checkWiFi();
    if (_firstWiFiConnect) {
        ArduinoOTA.handle();
        if (!_customDebugSerial) {
            _remoteDebug.handle();
        }
    }
}

// Check WiFi connection
void UltimateBaseFirmware::checkWiFi() {
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

void UltimateBaseFirmware::onFirstWiFiConnect() {
    _wifiCheckInterval = 5000;
    if (!_customDebugSerial) {
        Debug.println("[ INFO ] Initializing Debug Serial...");
        initializeDebugSerial();
        Debug.println("[  OK  ] Debug Serial initialized.");
    }

    Debug.println("[ INFO ] Initializing Web Server...");
    initializeWebServer();
    Debug.println("[ INFO ] Web Server initialized.");

    Debug.println("[  OK  ] Initializing OTA...");
    initializeOTA();
    Debug.println("[ INFO ] OTA initialized.");
    
    Debug.println("[ INFO ] Starting Web Server...");
    _server->begin();
    Debug.println("[  OK  ] Web Server started.");

    Debug.println("[ INFO ] Initializing MDNS...");
    initializeMdns();
    Debug.println("[  OK  ] MDNS initialized.");
}

void UltimateBaseFirmware::onWiFiConnected() {
    Debug.println("[ INFO ] WiFi connected!");
}

void UltimateBaseFirmware::onWiFiDisconnected() {
    Debug.println("[ INFO ] WiFi disconnected!");
}

// Initialize Debug Serial
void UltimateBaseFirmware::initializeDebugSerial() {
    _remoteDebug.setResetCmdEnabled(true);
    _remoteDebug.setSerialEnabled(false);
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

// Initialize WiFi in both STA and AP modes, with saved credentials via ImprovWiFi
void UltimateBaseFirmware::initializeWifi() {
    if (_wifiManager == nullptr) {
        _wifiManager = new WiFiManager();  // Create a default WiFiManager instance if not set
    }

    WiFi.mode(WIFI_STA);  // Explicitly set WiFi to station mode (STA)
    WiFi.hostname(_hostName);
    //WiFi.hostname()
    _wifiManager->setConfigPortalBlocking(false);  // Non-blocking captive portal
    _wifiManager->setConfigPortalTimeout(60);      // Portal times out after 60 seconds

    // Attempt to auto-connect to saved WiFi credentials, or start the captive portal
    if (_wifiManager->autoConnect("AutoConnectAP")) {
        Debug.println("Connected to WiFi!");
    } else {
        Debug.println("Failed to connect to WiFi. Captive portal running.");
    }
}

// Generate info string
String UltimateBaseFirmware::generateInfoString() {
    String info = "Firmware Name: " + String(_name) + "\n";
    info += "Firmware Version: " + String(_version) + "\n";
    info += "Chip Family: " + String(_chipFamily) + "\n";
    info += "Device Name: " + String(_deviceName) + "\n";
    info += "Host Name: " + String(_hostName) + "\n";
    info += "Device URL: http://" + String(_hostName) + "/\n";
    return info;
}

// Initialize Web Server (Async WebServer)
void UltimateBaseFirmware::initializeWebServer() {
    if (_server == nullptr) {
        _server = new AsyncWebServer(80);
    }

    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String response = generateInfoString();
        request->send(200, "text/plain", response);
        //request->send(200, "text/plain", "Hi! This is the UltimateBaseFirmware Web Server.");
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
    MDNS.begin(_hostName);
    MDNS.addService("telnet", "tcp", 23); // RemoteDebug Telnet
    MDNS.addService("http", "tcp", 80); // Web Server
    MDNS.enableArduino(); // OTA Serial
}