#include "RestApiRouter.h"
#include "Connections/MidiPage.h"

static bool requireParam(AsyncWebServerRequest* request, const char* name, String& out) {
    if (!request->hasParam(name)) {
        request->send(400, "text/plain", String("Missing parameter: ") + name);
        return false;
    }
    const AsyncWebParameter* param = request->getParam(name);
    if (param == nullptr) {
        request->send(400, "text/plain", String("Missing parameter: ") + name);
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

static bool readSwitch(AsyncWebServerRequest* request, const char* name, bool& present, bool& enabled) {
    present = request->hasParam(name);
    if (!present) {
        return true;
    }
    String raw = request->getParam(name)->value();
    raw.toLowerCase();
    if (raw == "1" || raw == "true" || raw == "on") {
        enabled = true;
        return true;
    }
    if (raw == "0" || raw == "false" || raw == "off") {
        enabled = false;
        return true;
    }
    return false;
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

    _server->on("/midi", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", MIDI_PAGE);
    });

    _server->on("^/api/v1/midi$", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        request->send(200, "application/json", _connections->statusJson());
    });

    _server->on("/api/v1/midi/settings", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        const char* names[] = {
            "din", "wifi", "bleAdvertise", "bleCentral", "bleMidiHost", "bleMidiPeripheral", "bleClassic",
            "autoConnect", "bleHid", "usb", "usbDevice", "usbHost", "usbMidiHost", "usbMidiPeripheral",
            "usbMidi2Host", "usbMidi2Peripheral", "ethernet", "ethernetMidi", "ethernetMidi2",
            "wifiMidi2", "usbHid"
        };
        bool any = false;
        for (const char* name : names) {
            bool present = false;
            bool enabled = false;
            if (!readSwitch(request, name, present, enabled)) {
                request->send(400, "text/plain", String("Bad value for ") + name);
                return;
            }
            if (!present) {
                continue;
            }
            any = true;
            String error;
            if (!_connections->setOption(name, enabled, error)) {
                request->send(400, "text/plain", error);
                return;
            }
        }
        if (!any) {
            request->send(400, "text/plain", "No setting in the request.");
            return;
        }
        request->send(200, "application/json", _connections->statusJson());
    });

    _server->on("/api/v1/midi/ble/scan", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        _connections->requestScan();
        request->send(200, "application/json", _connections->statusJson());
    });

    _server->on("/api/v1/midi/ble/devices", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        request->send(200, "application/json", _connections->statusJson());
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
        request->send(200, "application/json", _connections->statusJson());
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
        request->send(200, "application/json", _connections->statusJson());
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
        _connections->forget(address, error);
        request->send(200, "application/json", _connections->statusJson());
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
        request->send(200, "application/json", _connections->statusJson());
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
        if (!midiOk || !serialOk || !hidOk || midi == -2 || serial == -2 || hid == -2) {
            request->send(400, "text/plain", "Bad role value.");
            return;
        }
        String error;
        if (!_connections->setPeerRoles(address, midi, serial, hid, error)) {
            request->send(400, "text/plain", error);
            return;
        }
        request->send(200, "application/json", _connections->statusJson());
    });

    _server->on("/api/v1/hid/keyboard", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_connections == nullptr) {
            request->send(503, "text/plain", "MIDI is not available.");
            return;
        }
        String text;
        if (!requireParam(request, "text", text)) {
            return;
        }
        String error;
        if (!_connections->queueKeys(text, error)) {
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
        int x = request->hasParam("x") ? request->getParam("x")->value().toInt() : 0;
        int y = request->hasParam("y") ? request->getParam("y")->value().toInt() : 0;
        int buttons = request->hasParam("buttons") ? request->getParam("buttons")->value().toInt() : 0;
        String error;
        if (!_connections->queueMouse(x, y, buttons, error)) {
            request->send(409, "text/plain", error);
            return;
        }
        request->send(200, "text/plain", "ok");
    });

    // Remote UI
    _server->on("/remote-ui", HTTP_GET, [this](AsyncWebServerRequest* request) {
        // dynamically generate an html/JS page that will generate a page that will allow the user to view/controll all the entities of the showbox
        // this will be a single page application that will use the REST API to get and set the values of the entities
        // the page will be generated right here:

        String page = "<!DOCTYPE html><html><head><title>Showbox Remote UI</title></head><body>";
        page += "<p><a href=\"/midi\">Connections</a></p><script>\n";
        page += "async function main() {\n";
            page += "const entities = [";
                for (uint8_t i = 0; i <= FX_BYPASS; i++) {
                    entity_id entityId = static_cast<entity_id>(i);
                    std::string entityName = entity_id_to_string[entityId];
                    page += "'" + String(entityName.c_str()) + "',";
                }
            page += "];\n";
            page += "const entityTypes = {";
                for (uint8_t i = 0; i <= FX_BYPASS; i++) {
                    entity_id entityId = static_cast<entity_id>(i);
                    entity_data_type dataType = entity_type_mapping[entityId];
                    std::string entityName = entity_id_to_string[entityId];
                    std::string type = entity_data_type_to_string[dataType];
                    page += "'" + String(entityName.c_str()) + "': '" + String(type.c_str()) + "',";
                }
            page += "};\n";
            // generate interactive controls for every entity that can get/set entity values through the api (get example /api/v1/showbox/entities/FRONT_LED - set example /api/v1/showbox/entities/FRONT_LED?value=true)
            // request all entities and their values, generate the controls based on the type of the entity, add event listeners to the controls to send the values to the api
            page += "for (const entity of entities) {\n";
                page += "const entityType = entityTypes[entity];\n";
                page += "const container = document.createElement('div');\n";
                page += "container.innerHTML = entity + ': ';\n";
                page += "document.body.appendChild(container);\n";
                page += "const input = document.createElement('input');\n";
                page += "input.id = entity;\n";
                page += "if (entityType === 'BOOL') {\n";
                    page += "input.type = 'checkbox';\n";
                    page += "input.addEventListener('change', async (event) => {\n";
                        page += "const value = event.target.checked;\n";
                        page += "await fetch('/api/v1/showbox/entities/' + entity + '?value=' + value);\n";
                    page += "});\n";
                    page += "container.appendChild(input);\n";
                    page += "const value = await fetch('/api/v1/showbox/entities/' + entity).then(response => response.text());\n";
                    page += "input.checked = value === 'true';\n";
                page += "} else if (entityType === 'UINT8') {\n";
                    page += "input.type = 'number';\n";
                    page += "input.addEventListener('change', async (event) => {\n";
                        page += "const value = event.target.value;\n";
                        page += "await fetch('/api/v1/showbox/entities/' + entity + '?value=' + value);\n";
                    page += "});\n";
                    page += "container.appendChild(input);\n";
                    page += "const value = await fetch('/api/v1/showbox/entities/' + entity).then(response => response.text());\n";
                    page += "input.value = value;\n";
                page += "} else if (entityType === 'FLOAT') {\n";
                    page += "input.type = 'number';\n";
                    page += "input.step = '0.01';\n";
                    page += "input.addEventListener('change', async (event) => {\n";
                        page += "const value = event.target.value;\n";
                        page += "await fetch('/api/v1/showbox/entities/' + entity + '?value=' + value);\n";
                    page += "});\n";
                    page += "container.appendChild(input);\n";
                    page += "const value = await fetch('/api/v1/showbox/entities/' + entity).then(response => response.text());\n";
                    page += "input.value = value;\n";
                page += "}\n";
            page += "}\n";
        page += "}\n";
        page += "main().catch(console.error);\n";
        page += "</script></body></html>";
        request->send(200, "text/html", page);
    });
}

void RestApiRouter::setDebugSerial(Print* serial) {
    Debug = serial;
}