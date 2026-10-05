#include "RestApiRouter.h"
#include "Actions/ActionRules.h"
#include "WebUi/WebUi.h"
#include "DebugLog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <cstring>
#include <cstdio>
#include <esp_heap_caps.h>

extern volatile bool gMdnsUp;
extern volatile bool gSetupDone;

static const AsyncWebParameter* findParam(AsyncWebServerRequest* request, const char* name) {
    if (request->hasParam(name)) {
        return request->getParam(name);
    }
    if (request->hasParam(name, true)) {
        return request->getParam(name, true);
    }
    return nullptr;
}

static bool sendMidiStatus(AsyncWebServerRequest* request, Connections* connections) {
    if (connections == nullptr) {
        request->send(503, "text/plain", "MIDI is not available.");
        return false;
    }
    size_t len = 0;
    // Never rebuild the ~5 KB MIDI JSON on the HTTP thread. Parallel
    // /services needs the same internal malloc that statusJson() eats.
    const uint8_t* body = connections->statusCacheBytes(&len);
    if (body == nullptr || len == 0) {
        request->send(503, "text/plain", "The module is busy.");
        return false;
    }
    // Snapshot: beginResponse(uint8_t*) is AsyncProgmemResponse and keeps the
    // live PSRAM cache pointer. A later cache rebuild then hangs the last TCP
    // chunk, so /midi arrives truncated and looks empty.
    String json;
    json.concat(reinterpret_cast<const char*>(body), len);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
    return true;
}

static bool requireParam(AsyncWebServerRequest* request, const char* name, String& out) {
    const AsyncWebParameter* param = findParam(request, name);
    if (param == nullptr) {
        request->send(400, "text/plain", "Missing parameter.");
        return false;
    }
    out = param->value();
    return true;
}

RestApiRouter::RestApiRouter() {}

void RestApiRouter::setWebServer(AsyncWebServer* server) {
    _server = server;
}

void RestApiRouter::setShowbox(MackieShowbox* showbox) {
    _showbox = showbox;
}

void RestApiRouter::setConnections(Connections* connections) {
    _connections = connections;
}

void RestApiRouter::setActions(ActionRules* actions) {
    _actions = actions;
}

static bool parseOnOff(const char* raw, bool& enabled) {
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    if ((raw[0] == '1' || raw[0] == '0') && raw[1] == '\0') {
        enabled = raw[0] == '1';
        return true;
    }
    if (strcasecmp(raw, "true") == 0 || strcasecmp(raw, "on") == 0) {
        enabled = true;
        return true;
    }
    if (strcasecmp(raw, "false") == 0 || strcasecmp(raw, "off") == 0) {
        enabled = false;
        return true;
    }
    return false;
}

static bool readSwitch(AsyncWebServerRequest* request, const char* name, bool& present, bool& enabled) {
    const AsyncWebParameter* param = findParam(request, name);
    present = param != nullptr;
    if (!present) {
        return true;
    }
    return parseOnOff(param->value().c_str(), enabled);
}

static int readTri(AsyncWebServerRequest* request, const char* name, bool& ok) {
    bool present = false;
    bool enabled = false;
    ok = readSwitch(request, name, present, enabled);
    if (!ok) {
        return -2;
    }
    if (!present) {
        return -1;
    }
    return enabled ? 1 : 0;
}

static bool readIntParam(AsyncWebServerRequest* request, const char* name, bool& present, int& value) {
    const AsyncWebParameter* param = findParam(request, name);
    present = param != nullptr;
    if (!present) {
        return true;
    }
    value = param->value().toInt();
    return true;
}

void RestApiRouter::setup() {
    // Entity routes
    _server->on("^/api/v1/showbox/entities/([a-zA-Z0-9_]+)$", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->print("GET /api/v1/showbox/entities/");
        String entityName = request->pathArg(0);
        std::string entityNameStr = entityName.c_str();
        Debug->println(entityName);
        auto entityIt = string_to_entity_id.find(entityNameStr);
        if (entityIt == string_to_entity_id.end()) {
            Debug->println("Entity not found.");
            request->send(404, "text/plain", "Entity not found.");
            return;
        }
        entity_id entityId = entityIt->second;
        Debug->print("Entity ID: ");
        Debug->println(entityId);

        auto entityTypeIt = entity_type_mapping.find(entityId);
        if (entityTypeIt == entity_type_mapping.end()) {
            request->send(404, "text/plain", "Entity not found.");
            return;
        }
        entity_data_type entityType = entityTypeIt->second;
        Debug->print("Entity Type: ");
        Debug->println(entityType);

        if (request->hasParam("value")) {
            const AsyncWebParameter* p = request->getParam("value");
            if (p == nullptr) {
                request->send(400, "text/plain", "Missing parameter: value");
                return;
            }
            String stringValue = p->value();
            Debug->print("Value: ");
            Debug->println(stringValue);

            if (entityType == BOOL) {
                String normalized = stringValue;
                normalized.toLowerCase();
                bool value;
                if (normalized == "true" || normalized == "1") {
                    value = true;
                } else if (normalized == "false" || normalized == "0") {
                    value = false;
                } else {
                    request->send(400, "text/plain", "Invalid boolean value.");
                    return;
                }
                _showbox->setEntityValue(entityId, value);
            } else if (entityType == UINT8) {
                uint8_t value = stringValue.toInt();
                _showbox->setEntityValue(entityId, value);
            } else if (entityType == FLOAT) {
                float value = stringValue.toFloat();
                _showbox->setEntityValue(entityId, value);
            }
        } else {
            Debug->println("No value provided.");
        }

        // Return current/new values
        if (entityType == BOOL) {
            bool value = _showbox->getBoolEntityValue(entityId);
            Debug->print("New Value: ");
            Debug->println(value);
            request->send(200, "text/plain", value ? "true" : "false");
        } else if (entityType == UINT8) {
            uint8_t value = _showbox->getUint8EntityValue(entityId);
            Debug->print("New Value: ");
            Debug->println(value);
            request->send(200, "text/plain", String(value));
        } else if (entityType == FLOAT) {
            float value = _showbox->getFloatEntityValue(entityId);
            Debug->print("New Value: ");
            Debug->println(value);
            request->send(200, "text/plain", String(value));
        }
    });

    _server->on("/api/v1/showbox/entities", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_showbox == nullptr) {
            request->send(503, "text/plain", "Showbox is not available.");
            return;
        }
        String json = "{\"entities\":[";
        bool first = true;
        for (uint8_t i = 0; i <= FX_BYPASS; i++) {
            entity_id entityId = static_cast<entity_id>(i);
            auto nameIt = entity_id_to_string.find(entityId);
            auto typeIt = entity_type_mapping.find(entityId);
            if (nameIt == entity_id_to_string.end() || typeIt == entity_type_mapping.end()) {
                continue;
            }
            if (!first) {
                json += ",";
            }
            first = false;
            json += "{\"id\":\"";
            json += nameIt->second.c_str();
            json += "\",\"type\":\"";
            auto typeName = entity_data_type_to_string.find(typeIt->second);
            json += typeName == entity_data_type_to_string.end() ? "FLOAT" : typeName->second.c_str();
            json += "\",\"value\":";
            if (typeIt->second == BOOL) {
                json += _showbox->getBoolEntityValue(entityId) ? "true" : "false";
            } else if (typeIt->second == UINT8) {
                json += String(_showbox->getUint8EntityValue(entityId));
            } else {
                char num[24];
                snprintf(num, sizeof(num), "%.6g", _showbox->getFloatEntityValue(entityId));
                json += num;
            }
            json += "}";
        }
        json += "]}";
        request->send(200, "application/json", json);
    });

    // Looper route
    _server->on("/api/v1/showbox/action/looper_button", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/looper_button");
        String actionName;
        if (!requireParam(request, "action", actionName)) {
            return;
        }
        if (actionName == "DOWN") {
            _showbox->sendLooperButtonAction(looper_button_action::DOWN);
        } else if (actionName == "UP") {
            _showbox->sendLooperButtonAction(looper_button_action::UP);
        } else if (actionName == "DOUBLE_PRESS") {
            _showbox->sendLooperButtonAction(looper_button_action::DOUBLE_PRESS);
        } else if (actionName == "LONG_PRESS") {
            _showbox->sendLooperButtonAction(looper_button_action::LONG_PRESS);
        } else {
            request->send(400, "text/plain", "Invalid action.");
            return;
        }
        request->send(200, "text/plain", "Looper action sent.");
    });

    // SD Card route
    _server->on("/api/v1/showbox/action/sdcard", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/sdcard");
        _showbox->toggleSdCardRecord();
        request->send(200, "text/plain", "SD Card action sent.");
    });

    // Snapshot route
    _server->on("/api/v1/showbox/action/snapshot", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/snapshot");
        String actionName;
        String slotName;
        if (!requireParam(request, "action", actionName) || !requireParam(request, "slot", slotName)) {
            return;
        }
        snapshot_slot slot = static_cast<snapshot_slot>(slotName.toInt());
        if (actionName == "RECALL") {
            _showbox->snapshotAction(snapshot_action::RECALL, slot);
        } else if (actionName == "SAVE") {
            _showbox->snapshotAction(snapshot_action::SAVE, slot);
        } else {
            request->send(400, "text/plain", "Invalid action.");
            return;
        }
        request->send(200, "text/plain", "Snapshot action sent.");
    });

    // Tuner route
    _server->on("/api/v1/showbox/action/tuner", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/tuner");
        String actionName;
        String chanName;
        if (!requireParam(request, "action", actionName) || !requireParam(request, "chan", chanName)) {
            return;
        }
        tuner_chan chan = static_cast<tuner_chan>(chanName.toInt());
        if (actionName == "TURN_ON") {
            _showbox->tunerAction(tuner_action::TURN_ON, chan);
        } else if (actionName == "TURN_OFF") {
            _showbox->tunerAction(tuner_action::TURN_OFF, chan);
        } else {
            request->send(400, "text/plain", "Invalid action.");
            return;
        }
        request->send(200, "text/plain", "Tuner action sent.");
    });

    _server->on("/api/v1/showbox/link", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_showbox == nullptr) {
            request->send(503, "application/json", "{\"ok\":false}");
            return;
        }
        uint32_t base = _showbox->baseAgeMs();
        uint32_t mix = _showbox->mixerAgeMs();
        uint32_t hb = _showbox->heartbeatAgeMs();
        char body[1600];
        snprintf(body, sizeof(body),
            "{\"ok\":%s,\"iso\":%d,\"lock\":%d,\"baseAgeMs\":%u,\"mixerAgeMs\":%u,\"heartbeatAgeMs\":%u,\"baseTxAgeMs\":%u,\"mixerTxAgeMs\":%u,\"parsing\":%s,\"pendToMixer\":%u,\"pendToBase\":%u,\"ringBase\":%u,\"ringMix\":%u,\"holdBase\":%u,\"holdMix\":%u,\"cutBase\":%u,\"cutMix\":%u,\"rxBase\":%u,\"rxMix\":%u,\"dropBase\":%u,\"dropMix\":%u,\"ovfBase\":%u,\"ovfMix\":%u,\"frmBase\":%u,\"frmMix\":%u,\"gapB\":%u,\"gapM\":%u,\"goodM\":%u,\"goodB\":%u,\"badM\":%u,\"badB\":%u,\"fifoBase\":%u,\"fifoMix\":%u,\"intBase\":%u,\"intMix\":%u,\"pause\":%s,\"pauses\":%u,\"initM\":%u,\"initB\":%u,\"hbM\":%u,\"hbB\":%u,\"entM\":%u,\"reqM\":%u,\"rest\":%u,\"mdns\":%s,\"wifi\":%d,\"setup\":%s,\"heap\":%u,\"largest\":%u}",
            _showbox->linkOk(1500) ? "true" : "false",
            gIso,
            UART_SERVICE_LOCK,
            (unsigned)base, (unsigned)mix, (unsigned)hb,
            (unsigned)_showbox->baseTxAgeMs(),
            (unsigned)_showbox->mixerTxAgeMs(),
            _showbox->parsing() ? "true" : "false",
            (unsigned)_showbox->pendingToMixer(),
            (unsigned)_showbox->pendingToBase(),
            (unsigned)_showbox->ringFromBase(),
            (unsigned)_showbox->ringFromMixer(),
            (unsigned)_showbox->holdFromBase(),
            (unsigned)_showbox->holdFromMixer(),
            (unsigned)_showbox->cutThroughFromBase(),
            (unsigned)_showbox->cutThroughFromMixer(),
            (unsigned)_showbox->rxFromBase(),
            (unsigned)_showbox->rxFromMixer(),
            (unsigned)_showbox->droppedFromBase(),
            (unsigned)_showbox->droppedFromMixer(),
            (unsigned)_showbox->overflowFromBase(),
            (unsigned)_showbox->overflowFromMixer(),
            (unsigned)_showbox->frameErrFromBase(),
            (unsigned)_showbox->frameErrFromMixer(),
            (unsigned)_showbox->maxGapFromBase(),
            (unsigned)_showbox->maxGapFromMixer(),
            (unsigned)_showbox->goodFromMixer(),
            (unsigned)_showbox->goodFromBase(),
            (unsigned)_showbox->badFromMixer(),
            (unsigned)_showbox->badFromBase(),
            (unsigned)_showbox->hwRxFifoBase(),
            (unsigned)_showbox->hwRxFifoMixer(),
            (unsigned)_showbox->hwIntStBase(),
            (unsigned)_showbox->hwIntStMixer(),
            _showbox->txToBasePaused() ? "true" : "false",
            (unsigned)_showbox->txPauseCount(),
            (unsigned)_showbox->initFromMixer(),
            (unsigned)_showbox->initFromBase(),
            (unsigned)_showbox->hbFromMixer(),
            (unsigned)_showbox->hbFromBase(),
            (unsigned)_showbox->entityFromMixer(),
            (unsigned)_showbox->requestFromMixer(),
            (unsigned)_showbox->restoreCountFromMixer(),
            gMdnsUp ? "true" : "false",
            (int)WiFi.status(),
            gSetupDone ? "true" : "false",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", body);
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("Connection", "close");
        request->send(response);
    });

    _server->on("/api/v1/debug/iso", HTTP_GET, [](AsyncWebServerRequest* request) {
        char body[48];
        snprintf(body, sizeof(body), "{\"iso\":%d}", gIso);
        request->send(200, "application/json", body);
    });

    _server->on("/api/v1/debug/iso", HTTP_POST, [](AsyncWebServerRequest* request) {
        const AsyncWebParameter* p = findParam(request, "level");
        if (p == nullptr) {
            p = findParam(request, "iso");
        }
        if (p == nullptr) {
            request->send(400, "text/plain", "Missing iso level.");
            return;
        }
        int level = atoi(p->value().c_str());
        if (level < 0 || level > 13) {
            request->send(400, "text/plain", "iso level must be 0-13.");
            return;
        }
        // Never write NVS from the HTTP task. Flash from AsyncTCP used to
        // reboot before the value landed, so every iso POST came back as 0.
        gIsoPending = level;
        gIsoRebootAt = millis() + 1500;
        char body[80];
        snprintf(body, sizeof(body), "{\"ok\":true,\"iso\":%d,\"reboot\":true}", level);
        request->send(200, "application/json", body);
    });

    _server->on("/api/v1/debug/uartsoak", HTTP_GET, [](AsyncWebServerRequest* request) {
        char body[512];
        if (gSoakRunning) {
            request->send(200, "application/json", "{\"done\":false,\"running\":true}");
            return;
        }
        uartSoakResult(body, sizeof(body));
        request->send(200, "application/json", body);
    });
    _server->on("/api/v1/debug/uartsoak", HTTP_POST, [](AsyncWebServerRequest* request) {
        int seconds = 40;
        const AsyncWebParameter* p = findParam(request, "seconds");
        if (p != nullptr) {
            seconds = atoi(p->value().c_str());
        }
        if (seconds < 5) {
            seconds = 5;
        }
        if (seconds > 90) {
            seconds = 90;
        }
        if (gSoakRunning) {
            request->send(409, "application/json", "{\"ok\":false,\"running\":true}");
            return;
        }
        gSoakSeconds = seconds;
        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":true,\"running\":true,\"seconds\":%d,\"wifi\":\"stopping\"}", seconds);
        request->send(200, "application/json", body);
    });

    _server->on("/api/v1/debug", HTTP_GET, [](AsyncWebServerRequest* request) {
        static char tmp[6144];
        debugLogCopy(tmp, sizeof(tmp));
        size_t n = strlen(tmp);
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain",
            reinterpret_cast<uint8_t*>(tmp), n);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Battery Level route
    _server->on("/api/v1/showbox/status/battery", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/status/battery");
        float batteryLevel = _showbox->getBatteryLevel();
        request->send(200, "text/plain", String(batteryLevel));
    });

    // SD Card State route
    _server->on("/api/v1/showbox/status/sdcard", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/status/sdcard");
        sd_card_state state = _showbox->getSdCardState();
        if (state == sd_card_state::NOT_DETECTED) {
            request->send(200, "text/plain", "NOT_DETECTED");
        } else if (state == sd_card_state::DETECTED) {
            request->send(200, "text/plain", "DETECTED");
        } else if (state == sd_card_state::RECORDING) {
            request->send(200, "text/plain", "RECORDING");
        } else {
            request->send(500, "text/plain", "Unknown state.");
        }
    });

    _server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });

    _server->on("/midi", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/services", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });

    _server->on("^/api/v1/midi$", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendMidiStatus(request, _connections);
    });

    _server->on("/api/v1/midi/settings", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        static const char* names[] = {
            "usbSafeTest", "din", "wifi", "bleAdvertise", "bleCentral", "bleMidiHost", "bleMidiPeripheral", "bleClassic",
            "autoConnect", "bleHid", "bleHidKeyboard", "bleHidMouse", "bleHidAbs", "bleHidDigitizer",
            "usbHidKeyboard", "usbHidMouse", "usbHidAbs", "usbHidDigitizer",
            "usb", "usbDevice", "usbHost", "usbMidiHost", "usbMidiPeripheral",
            "usbMidi2Host", "usbMidi2Peripheral", "ethernet", "ethernetMidi", "ethernetMidi2",
            "wifiMidi2", "usbHid", "wifiLogHistory", "uartPackets", "acceptNewHosts", "interceptor"
        };
        bool any = false;
        for (const char* name : names) {
            bool present = false;
            bool enabled = false;
            if (!readSwitch(request, name, present, enabled)) {
                request->send(400, "text/plain", "Bad setting value.");
                return;
            }
            if (!present) {
                continue;
            }
            any = true;
            _connections->queueOption(name, enabled);
        }
        if (!any) {
            request->send(400, "text/plain", "No setting in the request.");
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/scan", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        _connections->requestScan();
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/devices", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendMidiStatus(request, _connections);
    });

    _server->on("/api/v1/midi/ble/connect", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String address;
        if (!requireParam(request, "address", address)) {
            return;
        }
        String name = request->hasParam("name") ? request->getParam("name")->value() : "";
        bool midiOk = true;
        bool hidOk = true;
        int midi = readTri(request, "midi", midiOk);
        int hid = readTri(request, "hid", hidOk);
        if (!midiOk || !hidOk || midi == -2 || hid == -2) {
            request->send(400, "text/plain", "Bad MIDI or HID value.");
            return;
        }
        bool pairPresent = false;
        bool pairOn = false;
        if (!readSwitch(request, "pair", pairPresent, pairOn)) {
            request->send(400, "text/plain", "Bad pair value.");
            return;
        }
        String error;
        bool ok = pairPresent && pairOn
            ? _connections->pair(address, name, error)
            : _connections->connect(address, name, midi, hid, error);
        if (!ok) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/disconnect", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String address;
        if (!requireParam(request, "address", address)) {
            return;
        }
        String error;
        if (!_connections->disconnectAddress(address, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/forget", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String address;
        if (!requireParam(request, "address", address)) {
            return;
        }
        String error;
        if (!_connections->queueForget(address, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/auto", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String address;
        if (!requireParam(request, "address", address)) {
            return;
        }
        bool present = false;
        bool enabled = false;
        if (!readSwitch(request, "enabled", present, enabled) || !present) {
            request->send(400, "text/plain", "Missing enabled.");
            return;
        }
        String error;
        if (!_connections->setPeerAuto(address, enabled, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/role", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String address;
        if (!requireParam(request, "address", address)) {
            return;
        }
        bool midiOk = true;
        bool serialOk = true;
        bool hidOk = true;
        int midi = readTri(request, "midi", midiOk);
        int serial = readTri(request, "serial", serialOk);
        int hid = readTri(request, "hid", hidOk);
        bool kbOk = true;
        bool mouseOk = true;
        bool absOk = true;
        bool digOk = true;
        int hidKeyboard = readTri(request, "hidKeyboard", kbOk);
        int hidMouse = readTri(request, "hidMouse", mouseOk);
        int hidAbs = readTri(request, "hidAbs", absOk);
        int hidDigitizer = readTri(request, "hidDigitizer", digOk);
        bool pinnedOk = true;
        int pinned = readTri(request, "pinned", pinnedOk);
        if (!midiOk || !serialOk || !hidOk || !kbOk || !mouseOk || !absOk || !digOk || !pinnedOk
            || midi == -2 || serial == -2 || hid == -2 || hidKeyboard == -2 || hidMouse == -2
            || hidAbs == -2 || hidDigitizer == -2 || pinned == -2) {
            request->send(400, "text/plain", "Bad role value.");
            return;
        }
        String error;
        if (!_connections->queuePeerRoles(address, midi, serial, hid, hidKeyboard, hidMouse, hidAbs, hidDigitizer, pinned, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/ble/slots", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        bool hostsPresent = false;
        bool periPresent = false;
        int hosts = 0;
        int peri = 0;
        if (!readIntParam(request, "hosts", hostsPresent, hosts) || !readIntParam(request, "peripherals", periPresent, peri)
            || !hostsPresent || !periPresent) {
            request->send(400, "text/plain", "Missing hosts or peripherals.");
            return;
        }
        String error;
        if (!_connections->queueSlots(hosts, peri, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/midi/test", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String error;
        if (!_connections->queueMidiTest(error)) {
            request->send(409, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/hid/keyboard", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String key;
        String text;
        const AsyncWebParameter* keyParam = findParam(request, "key");
        const AsyncWebParameter* textParam = findParam(request, "text");
        if (keyParam != nullptr) {
            key = keyParam->value();
        }
        if (textParam != nullptr) {
            text = textParam->value();
        }
        if (key.length() == 0 && text.length() == 0) {
            request->send(400, "text/plain", "Missing parameter.");
            return;
        }
        String error;
        bool ok = key.length() > 0 ? _connections->queueKey(key.c_str(), error)
            : _connections->queueKeys(text, error);
        if (!ok) {
            request->send(409, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/hid/mouse", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        int x = findParam(request, "x") ? findParam(request, "x")->value().toInt() : 0;
        int y = findParam(request, "y") ? findParam(request, "y")->value().toInt() : 0;
        int buttons = findParam(request, "buttons") ? findParam(request, "buttons")->value().toInt() : 0;
        int wheel = findParam(request, "wheel") ? findParam(request, "wheel")->value().toInt() : 0;
        int pan = findParam(request, "pan") ? findParam(request, "pan")->value().toInt() : 0;
        String error;
        if (!_connections->queueMouse(x, y, buttons, error, wheel, pan)) {
            request->send(409, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/api/v1/hid/touch", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "HID is not available.");
            return;
        }
        String action = findParam(request, "action") ? findParam(request, "action")->value() : "tap";
        int x = findParam(request, "x") ? findParam(request, "x")->value().toInt() : 5000;
        int y = findParam(request, "y") ? findParam(request, "y")->value().toInt() : 5000;
        int x2 = findParam(request, "x2") ? findParam(request, "x2")->value().toInt() : 7000;
        int y2 = findParam(request, "y2") ? findParam(request, "y2")->value().toInt() : 5000;
        char viaBuf[12] = {};
        if (const AsyncWebParameter* viaParam = findParam(request, "via")) {
            strncpy(viaBuf, viaParam->value().c_str(), sizeof(viaBuf) - 1);
        }
        String error;
        if (!_connections->queueTouch(action, x, y, x2, y2, error, viaBuf[0] != '\0' ? viaBuf : nullptr)) {
            request->send(409, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    _server->on("/actions", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });

    _server->on("/api/v1/actions", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_actions == nullptr) {
            request->send(503, "text/plain", "Reactions are not available.");
            return;
        }
        String json;
        String error;
        if (!_actions->snapshotJson(json, error)) {
            request->send(503, "text/plain", error.length() ? error : "The module did not answer.");
            return;
        }
        request->send(200, "application/json", json);
    });

    _server->on("/api/v1/actions/delete", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_actions == nullptr) {
            request->send(503, "text/plain", "Reactions are not available.");
            return;
        }
        String id;
        if (!requireParam(request, "id", id)) {
            return;
        }
        String error;
        if (!_actions->removeRule(id.toInt(), error)) {
            request->send(400, "text/plain", error);
            return;
        }
        String json;
        if (!_actions->snapshotJson(json, error)) {
            request->send(503, "text/plain", error.length() ? error : "The module did not answer.");
            return;
        }
        request->send(200, "application/json", json);
    });

    _server->on("/api/v1/actions/run", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_actions == nullptr) {
            request->send(503, "text/plain", "Reactions are not available.");
            return;
        }
        String id;
        if (!requireParam(request, "id", id)) {
            return;
        }
        String error;
        if (!_actions->runRule(id.toInt(), error)) {
            request->send(400, "text/plain", error);
            return;
        }
        String json;
        if (!_actions->snapshotJson(json, error)) {
            request->send(503, "text/plain", error.length() ? error : "The module did not answer.");
            return;
        }
        request->send(200, "application/json", json);
    });

    _server->on("/api/v1/actions", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_actions == nullptr) {
            request->send(503, "text/plain", "Reactions are not available.");
            return;
        }
        if (request->contentLength() > 3900) {
            request->send(400, "text/plain", "That reaction is too large to store.");
            return;
        }
        const char* raw = static_cast<const char*>(request->_tempObject);
        if (raw == nullptr || raw[0] == '\0') {
            request->send(400, "text/plain", "That reaction is not valid JSON.");
            return;
        }
        String error;
        if (!_actions->saveJson(raw, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "application/json", error);
    }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (total == 0 || total > 3900) {
            return;
        }
        if (index == 0) {
            void* buffer = malloc(total + 1);
            request->_tempObject = buffer;
            if (buffer != nullptr) {
                static_cast<char*>(buffer)[total] = '\0';
            }
        }
        if (request->_tempObject == nullptr || index + len > total) {
            return;
        }
        memcpy(static_cast<char*>(request->_tempObject) + index, data, len);
    });

    _server->on("/remote-ui", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
}

void RestApiRouter::setDebugSerial(Print* serial) {
    Debug = serial;
}