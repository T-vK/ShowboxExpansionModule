#include "RestApiRouter.h"

RestApiRouter::RestApiRouter() {}

void RestApiRouter::setWebServer(AsyncWebServer* server) {
    _server = server;
}

void RestApiRouter::setShowbox(MackieShowbox* showbox) {
    _showbox = showbox;
}

void RestApiRouter::setup() {
    // Entity routes
    _server->on("^/api/v1/showbox/entities/([a-zA-Z0-9_]+)$", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->print("GET /api/v1/showbox/entities/");
        String entityName = request->pathArg(0);
        std::string entityNameStr = entityName.c_str();
        Debug->println(entityName);
        // Get entity ID from entity name
        entity_id entityId = string_to_entity_id[entityNameStr];
        Debug->print("Entity ID: ");
        Debug->println(entityId);

        // Validate entity ID
        if (entityId == 0 && entityName != "FRONT_LED") {
            Debug->println("Entity not found.");
            request->send(404, "text/plain", "Entity not found.");
            return;
        }

        // Get entity type from entity ID
        entity_data_type entityType = entity_type_mapping[entityId];
        Debug->print("Entity Type: ");
        Debug->println(entityType);

        if(request->hasParam("value")) {
            // Get entity value from request body
            const AsyncWebParameter* p = request->getParam("value");
            String stringValue = p->value();
            Debug->print("Value: ");
            Debug->println(stringValue);

            if (entityType == BOOL) {
                bool value = stringValue == "true";
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
        String actionName = request->getParam("action")->value();
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
        String actionName = request->getParam("action")->value();
        snapshot_action action = static_cast<snapshot_action>(actionName.toInt());
        String slotName = request->getParam("slot")->value();
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
        String actionName = request->getParam("action")->value();
        tuner_action action = static_cast<tuner_action>(actionName.toInt());
        String chanName = request->getParam("chan")->value();
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

    // Remote UI
    _server->on("/remote-ui", HTTP_GET, [this](AsyncWebServerRequest* request) {
        // dynamically generate an html/JS page that will generate a page that will allow the user to view/controll all the entities of the showbox
        // this will be a single page application that will use the REST API to get and set the values of the entities
        // the page will be generated right here:

        String page = "<!DOCTYPE html><html><head><title>Showbox Remote UI</title></head><body><script>\n";
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