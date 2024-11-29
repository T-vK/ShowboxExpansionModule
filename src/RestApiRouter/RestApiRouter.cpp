#include "RestApiRouter.h"

RestApiRouter::RestApiRouter() {}

void RestApiRouter::setWebServer(AsyncWebServer* server) {
    _server = server;
}

void RestApiRouter::setShowbox(MackieShowbox* showbox) {
    _showbox = showbox;
}

String RestApiRouter::getBodyParam(AsyncWebServerRequest* request, const String& paramName) {
    // Check for POST or PUT request
    if (request->method() == HTTP_PUT || request->method() == HTTP_POST) {
        // Try to access the request body directly
        String body = request->getParam("plain")->value();
        if (body.length() > 0) {
            // If the body has data, try parsing the body
            return parseFormData(body, paramName);
        }
    }
    return ""; // Return empty if not found
}

// Helper function to parse form-like data (e.g. value=true)
String RestApiRouter::parseFormData(const String& body, const String& paramName) {
    String paramPrefix = paramName + "=";
    int startPos = body.indexOf(paramPrefix);
    if (startPos != -1) {
        startPos += paramPrefix.length();
        int endPos = body.indexOf("&", startPos);
        if (endPos == -1) {
            endPos = body.length();
        }
        return body.substring(startPos, endPos);  // Extract the value of the parameter
    }
    return ""; // If parameter not found
}


void RestApiRouter::setup() {
    // Entity GET routes
    _server->on("^/api/v1/showbox/entities/([a-zA-Z0-9_]+)$", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->print("GET /api/v1/showbox/entities/");
        String entityName = request->pathArg(0);
        Debug->println(entityName);
        // Get entity ID from entity name
        entity_id entityId = string_to_entity_id[entityName.c_str()];
        // Get entity type from entity ID
        entity_data_type entityType = entity_type_mapping[entityId];
        // Get entity value from showbox
        if (entityType == BOOL) {
            bool value = _showbox->getBoolEntityValue(entityId);
            request->send(200, "text/plain", value ? "true" : "false");
        } else if (entityType == UINT8) {
            uint8_t value = _showbox->getUint8EntityValue(entityId);
            request->send(200, "text/plain", String(value));
        } else if (entityType == FLOAT) {
            float value = _showbox->getFloatEntityValue(entityId);
            request->send(200, "text/plain", String(value));
        } else {
            request->send(404, "text/plain", "Entity not found.");
        }
    });

    // Entity Update routes
    _server->on("^/api/v1/showbox/entities/([a-zA-Z0-9_]+)$", HTTP_PUT, [this](AsyncWebServerRequest* request) {
        Debug->print("PUT /api/v1/showbox/entities/");
        String entityName = request->pathArg(0);
        std::string entityNameStr = entityName.c_str();
        Debug->println(entityName);
        // Get entity ID from entity name
        entity_id entityId = string_to_entity_id[entityNameStr];
        Debug->print("Entity ID: ");
        Debug->println(entityId);
        // Get entity type from entity ID
        entity_data_type entityType = entity_type_mapping[entityId];
        Debug->print("Entity Type: ");
        Debug->println(entityType);

        // Get entity value from request body
        String stringValue = getBodyParam(request, "value");

        if (stringValue == "") {
            request->send(400, "text/plain", "Missing value parameter.");
            return;
        }

        if (entityType == BOOL) {
            Debug->println("Setting bool value...");
            bool value = stringValue == "true";
            _showbox->setEntityValue(entityId, value);
            bool newValue = _showbox->getBoolEntityValue(entityId);
            request->send(200, "text/plain", newValue ? "true" : "false");
        } else if (entityType == UINT8) {
            uint8_t value = stringValue.toInt();
            _showbox->setEntityValue(entityId, value);
            uint8_t newValue = _showbox->getUint8EntityValue(entityId);
            request->send(200, "text/plain", String(newValue));
        } else if (entityType == FLOAT) {
            float value = stringValue.toFloat();
            _showbox->setEntityValue(entityId, value);
            float newValue = _showbox->getFloatEntityValue(entityId);
            request->send(200, "text/plain", String(newValue));
        } else {
            request->send(404, "text/plain", "Entity not found.");
        }
    });

    // Looper route
    _server->on("/api/v1/showbox/action/looper_button", HTTP_POST, [this](AsyncWebServerRequest* request) {
        Debug->println("POST /api/v1/showbox/action/looper_button");
        String actionName = getBodyParam(request, "action");
        if (actionName == "") {
            request->send(400, "text/plain", "Missing action parameter.");
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
    _server->on("/api/v1/showbox/action/sdcard", HTTP_POST, [this](AsyncWebServerRequest* request) {
        Debug->println("POST /api/v1/showbox/action/sdcard");
        _showbox->toggleSdCardRecord();
        request->send(200, "text/plain", "SD Card action sent.");
    });

    // Snapshot route
    _server->on("/api/v1/showbox/action/snapshot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        Debug->println("POST /api/v1/showbox/action/snapshot");
        String actionName = getBodyParam(request, "action");
        if (actionName == "") {
            request->send(400, "text/plain", "Missing action parameter.");
            return;
        }
        String slotName = getBodyParam(request, "slot");
        if (slotName == "") {
            request->send(400, "text/plain", "Missing slot parameter.");
            return;
        }
        snapshot_action action = static_cast<snapshot_action>(actionName.toInt());
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
    _server->on("/api/v1/showbox/action/tuner", HTTP_POST, [this](AsyncWebServerRequest* request) {
        Debug->println("POST /api/v1/showbox/action/tuner");
        String actionName = getBodyParam(request, "action");
        String chanName = getBodyParam(request, "channel");
        tuner_action action = static_cast<tuner_action>(actionName.toInt());
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
}

void RestApiRouter::setDebugSerial(Print* serial) {
    Debug = serial;
}