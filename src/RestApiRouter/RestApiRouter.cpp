#include "RestApiRouter.h"
#include "Actions/ActionRules.h"
#include "CaptivePortal/CaptivePortal.h"
#include "LiveSocket/LiveSocket.h"
#include "WebUi/WebUi.h"
#include "DebugLog.h"
#include "System/LanKeep.h"
#include "System/HttpsGate.h"
#include "System/Diag.h"
#include <BleRadio.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Preferences.h>
#include <MultiMidi/MultiMidi.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <esp_heap_caps.h>
#include <esp_system.h>

extern volatile bool gMdnsUp;
extern volatile bool gSetupDone;
extern BleRadio bleRadio;

static const AsyncWebParameter* findParam(AsyncWebServerRequest* request, const char* name) {
    if (request->hasParam(name)) {
        return request->getParam(name);
    }
    if (request->hasParam(name, true)) {
        return request->getParam(name, true);
    }
    return nullptr;
}

static String requestHost(AsyncWebServerRequest* request) {
    if (!request->hasHeader("Host")) {
        return String();
    }
    String host = request->getHeader("Host")->value();
    host.toLowerCase();
    const int colon = host.indexOf(':');
    if (colon > 0) {
        host.remove(colon);
    }
    return host;
}

static bool isOsProbePath(const String& path) {
    return path == "/generate_204" || path == "/gen_204"
        || path == "/hotspot-detect.html" || path == "/library/test/success.html"
        || path == "/connecttest.txt" || path == "/ncsi.txt"
        || path == "/canonical.html" || path == "/success.txt"
        || path == "/kindle-wifi/wifistub.html" || path == "/redirect"
        || path == "/fwlink";
}

static bool isOsProbeHost(const String& host) {
    if (host.length() == 0) {
        return false;
    }
    if (host.startsWith("connectivitycheck.") || host.startsWith("captive.")) {
        return true;
    }
    return host == "clients3.google.com" || host == "clients4.google.com"
        || host == "android.clients.google.com"
        || host == "www.msftconnecttest.com" || host == "ipv6.msftconnecttest.com"
        || host == "www.msftncsi.com" || host == "detectportal.firefox.com"
        || host == "www.apple.com" || host.endsWith(".apple.com")
        || host == "connectivity.samsung.com.cn";
}

static void sendOsProbeReply(AsyncWebServerRequest* request) {
    const String& path = request->url();
    AsyncWebServerResponse* response = nullptr;
    if (path == "/generate_204" || path == "/gen_204") {
        response = request->beginResponse(204);
    } else if (path == "/connecttest.txt") {
        response = request->beginResponse(200, "text/plain", "Microsoft Connect Test");
    } else if (path == "/ncsi.txt") {
        response = request->beginResponse(200, "text/plain", "Microsoft NCSI");
    } else if (path == "/success.txt") {
        response = request->beginResponse(200, "text/plain", "success");
    } else {
        response = request->beginResponse(200, "text/html",
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    }
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

static void sendNoContent(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(204);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

static bool sendOsProbe(AsyncWebServerRequest* request) {
    const String path = request->url();
    const String host = requestHost(request);
    if (isOsProbePath(path)) {
        sendOsProbeReply(request);
        return true;
    }
    if (isOsProbeHost(host) && (path == "/" || path.length() == 0)) {
        sendNoContent(request);
        return true;
    }
    return false;
}

static bool redirectHotspotBrowser(CaptivePortal* portal, AsyncWebServerRequest* request) {
    if (portal == nullptr || request->client() == nullptr) {
        return false;
    }
    if (!portal->fromAccessPoint(request->client()->localIP())) {
        return false;
    }
    const String host = requestHost(request);
    const String apIp = WiFi.softAPIP().toString();
    if (host.length() == 0 || host == apIp || host == "showbox.local" || host == "showbox") {
        return false;
    }
    if (isOsProbeHost(host)) {
        sendNoContent(request);
        return true;
    }
    request->redirect(String("http://") + apIp + "/");
    return true;
}

static bool jsonBody(AsyncWebServerRequest* request, JsonDocument& doc) {
    const char* raw = static_cast<const char*>(request->_tempObject);
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    return deserializeJson(doc, raw) == DeserializationError::Ok;
}

static const char* kChanIds[] = {"ch1", "ch2", "ch3", "ch4", "aux", "loop", "phones", "main"};
static const size_t kChanCount = sizeof(kChanIds) / sizeof(kChanIds[0]);

static char gChanNames[8][20];
static char gSnapNames[5][20];
static volatile bool gNamesDirty = false;
static portMUX_TYPE gNamesMux = portMUX_INITIALIZER_UNLOCKED;
static char gConsoleCmd[96];
static volatile bool gConsoleCmdReady = false;
static volatile bool gConsoleReboot = false;

static void sanitizeName(char* out, size_t cap, const char* in) {
    if (out == nullptr || cap == 0) {
        return;
    }
    size_t n = 0;
    if (in != nullptr) {
        for (; in[n] != '\0' && n + 1 < cap && n < 16; n++) {
            char c = in[n];
            if (c < 32 || c > 126 || c == '"' || c == '\\') {
                c = ' ';
            }
            out[n] = c;
        }
        while (n > 0 && out[n - 1] == ' ') {
            n--;
        }
    }
    out[n] = '\0';
}

static void snapshotLabel(char* out, size_t cap, int slot, const char* stored) {
    if (out == nullptr || cap == 0) {
        return;
    }
    if (stored != nullptr && stored[0] != '\0') {
        sanitizeName(out, cap, stored);
        if (out[0] != '\0') {
            return;
        }
    }
    out[0] = static_cast<char>('1' + slot);
    if (cap > 1) {
        out[1] = '\0';
    }
}

static void loadNamesFromNvs() {
    char chan[8][20];
    char snap[5][20];
    memset(chan, 0, sizeof(chan));
    memset(snap, 0, sizeof(snap));
    Preferences prefs;
    if (prefs.begin("mixui", false)) {
        for (size_t i = 0; i < kChanCount; i++) {
            char val[20];
            val[0] = '\0';
            prefs.getString(kChanIds[i], val, sizeof(val));
            sanitizeName(chan[i], sizeof(chan[i]), val);
        }
        for (int i = 0; i < 5; i++) {
            char key[4] = {'s', static_cast<char>('0' + i), 0};
            char val[20];
            val[0] = '\0';
            prefs.getString(key, val, sizeof(val));
            sanitizeName(snap[i], sizeof(snap[i]), val);
        }
        prefs.end();
    }
    portENTER_CRITICAL(&gNamesMux);
    memcpy(gChanNames, chan, sizeof(gChanNames));
    memcpy(gSnapNames, snap, sizeof(gSnapNames));
    gNamesDirty = false;
    portEXIT_CRITICAL(&gNamesMux);
}

static void saveNamesToNvs() {
    char chan[8][20];
    char snap[5][20];
    portENTER_CRITICAL(&gNamesMux);
    memcpy(chan, gChanNames, sizeof(chan));
    memcpy(snap, gSnapNames, sizeof(snap));
    gNamesDirty = false;
    portEXIT_CRITICAL(&gNamesMux);
    Preferences prefs;
    if (!prefs.begin("mixui", false)) {
        portENTER_CRITICAL(&gNamesMux);
        gNamesDirty = true;
        portEXIT_CRITICAL(&gNamesMux);
        return;
    }
    for (size_t i = 0; i < kChanCount; i++) {
        if (chan[i][0] == '\0') {
            prefs.remove(kChanIds[i]);
        } else {
            prefs.putString(kChanIds[i], chan[i]);
        }
    }
    for (int i = 0; i < 5; i++) {
        char key[4] = {'s', static_cast<char>('0' + i), 0};
        if (snap[i][0] == '\0') {
            prefs.remove(key);
        } else {
            prefs.putString(key, snap[i]);
        }
    }
    prefs.end();
}

static size_t fillNamesJson(char* out, size_t cap) {
    if (out == nullptr || cap < 40) {
        return 0;
    }
    char chan[8][20];
    char snap[5][20];
    portENTER_CRITICAL(&gNamesMux);
    memcpy(chan, gChanNames, sizeof(chan));
    memcpy(snap, gSnapNames, sizeof(snap));
    portEXIT_CRITICAL(&gNamesMux);
    size_t n = 0;
    auto add = [&](const char* text) {
        size_t len = strlen(text);
        if (n + len + 1 >= cap) {
            return;
        }
        memcpy(out + n, text, len);
        n += len;
        out[n] = '\0';
    };
    add("{\"type\":\"names\",\"channels\":{");
    for (size_t i = 0; i < kChanCount; i++) {
        if (i > 0) {
            add(",");
        }
        add("\"");
        add(kChanIds[i]);
        add("\":\"");
        add(chan[i]);
        add("\"");
    }
    add("},\"snapshots\":[");
    for (int i = 0; i < 5; i++) {
        char val[20];
        snapshotLabel(val, sizeof(val), i, snap[i]);
        if (i > 0) {
            add(",");
        }
        add("\"");
        add(val);
        add("\"");
    }
    add("]}");
    return n;
}

static void publishNames(LiveSocket* live) {
    if (live == nullptr) {
        return;
    }
    char json[640];
    if (fillNamesJson(json, sizeof(json)) > 0) {
        live->setNamesJson(json);
    }
}

static void broadcastEntity(LiveSocket* live, MackieShowbox* showbox, entity_id entityId, const char* name) {
    if (live == nullptr || showbox == nullptr || name == nullptr) {
        return;
    }
    auto typeIt = entity_type_mapping.find(entityId);
    if (typeIt == entity_type_mapping.end()) {
        return;
    }
    char value[24];
    if (typeIt->second == BOOL) {
        snprintf(value, sizeof(value), "%s", showbox->getBoolEntityValue(entityId) ? "true" : "false");
    } else if (typeIt->second == UINT8) {
        snprintf(value, sizeof(value), "%u", (unsigned)showbox->getUint8EntityValue(entityId));
    } else {
        snprintf(value, sizeof(value), "%.6g", (double)showbox->getFloatEntityValue(entityId));
    }
    live->pushEntityJson(name, value);
}

static void runConsoleCmd(const char* raw, MackieShowbox* showbox) {
    char cmd[96];
    size_t n = 0;
    if (raw != nullptr) {
        while (raw[n] != '\0' && n + 1 < sizeof(cmd)) {
            char c = raw[n];
            if (c < 32 || c > 126) {
                c = ' ';
            }
            cmd[n++] = c;
        }
    }
    while (n > 0 && cmd[n - 1] == ' ') {
        n--;
    }
    cmd[n] = '\0';
    if (cmd[0] == '\0') {
        return;
    }
    Print* out = ::Debug;
    if (out == nullptr) {
        out = &Serial;
    }
    out->printf("> %s\n", cmd);
    if (strcasecmp(cmd, "help") == 0) {
        out->println("help  heap  status  clear  reboot  iso [0-13]  soak [seconds]");
        return;
    }
    if (strcasecmp(cmd, "heap") == 0) {
        out->printf("heap=%u largest=%u psram=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }
    if (strcasecmp(cmd, "status") == 0) {
        out->printf("ip=%s rssi=%d mdns=%d iso=%d\n",
            WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), gMdnsUp ? 1 : 0, gIso);
        if (showbox != nullptr) {
            out->printf("uart baseAge=%u mixerAge=%u link=%d batt=%.0f\n",
                (unsigned)showbox->baseAgeMs(), (unsigned)showbox->mixerAgeMs(),
                showbox->linkOk(1500) ? 1 : 0, (double)showbox->getBatteryLevel());
        }
        return;
    }
    if (strcasecmp(cmd, "clear") == 0) {
        consoleClear();
        out->println("log cleared");
        return;
    }
    if (strcasecmp(cmd, "reboot") == 0) {
        out->println("rebooting");
        gConsoleReboot = true;
        return;
    }
    if (strncasecmp(cmd, "iso", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        const char* arg = cmd + 3;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg == '\0') {
            out->printf("iso=%d\n", gIso);
            return;
        }
        int level = atoi(arg);
        if (level < 0 || level > 13) {
            out->println("iso level must be 0-13");
            return;
        }
        gIsoPending = level;
        gIsoRebootAt = millis() + 1500;
        out->printf("iso=%d reboot\n", level);
        return;
    }
    if (strncasecmp(cmd, "soak", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        int seconds = 40;
        const char* arg = cmd + 4;
        while (*arg == ' ') {
            arg++;
        }
        if (*arg != '\0') {
            seconds = atoi(arg);
        }
        if (seconds < 5) seconds = 5;
        if (seconds > 90) seconds = 90;
        if (gSoakRunning) {
            out->println("soak already running");
            return;
        }
        gSoakSeconds = seconds;
        out->printf("soak %ds wifi stopping\n", seconds);
        return;
    }
    out->println("unknown command — type help");
}

static void collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (total == 0 || total > 4096) {
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
}

static bool sendJsonOk(AsyncWebServerRequest* request, const String& json) {
    sendJsonChunked(request, json.c_str(), json.length());
    return true;
}

static bool sendMidiStatus(AsyncWebServerRequest* request, Connections* connections) {
    lanNoteHttp();
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
    // Snapshot, then BasicResponse copies into its own String. The uint8_t*
    // overload is Progmem and keeps the live cache pointer, so a later rebuild
    // hangs the last TCP chunk.
    char* copy = static_cast<char*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        request->send(503, "text/plain", "The module is busy.");
        return false;
    }
    memcpy(copy, body, len);
    copy[len] = '\0';
    request->_tempObject = copy;
    AsyncWebServerResponse* response = request->beginResponse(
        "application/json", len,
        [copy, len](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= len) {
                return 0;
            }
            size_t n = len - index;
            if (n > maxLen) {
                n = maxLen;
            }
            if (n > 256) {
                n = 256;
            }
            memcpy(buffer, copy + index, n);
            return n;
        });
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

void RestApiRouter::setLiveSocket(LiveSocket* socket) {
    _live = socket;
}

void RestApiRouter::setCaptivePortal(CaptivePortal* portal) {
    _portal = portal;
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
    loadNamesFromNvs();
    preloadWebUi();
    publishNames(_live);
    // Entity routes
    _server->on("^/api/v1/showbox/entities/([a-zA-Z0-9_]+)$", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String entityName = request->pathArg(0);
        std::string entityNameStr = entityName.c_str();
        auto entityIt = string_to_entity_id.find(entityNameStr);
        if (entityIt == string_to_entity_id.end()) {
            request->send(404, "text/plain", "Entity not found.");
            return;
        }
        entity_id entityId = entityIt->second;

        auto entityTypeIt = entity_type_mapping.find(entityId);
        if (entityTypeIt == entity_type_mapping.end()) {
            request->send(404, "text/plain", "Entity not found.");
            return;
        }
        entity_data_type entityType = entityTypeIt->second;

        if (request->hasParam("value")) {
            const AsyncWebParameter* p = request->getParam("value");
            if (p == nullptr) {
                request->send(400, "text/plain", "Missing parameter: value");
                return;
            }
            String stringValue = p->value();

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
        }

        if (!request->hasParam("value") && !_showbox->hasEntityValue(entityId)) {
            request->send(200, "text/plain", "");
            return;
        }
        if (entityType == BOOL) {
            bool value = _showbox->getBoolEntityValue(entityId);
            request->send(200, "text/plain", value ? "true" : "false");
        } else if (entityType == UINT8) {
            uint8_t value = _showbox->getUint8EntityValue(entityId);
            request->send(200, "text/plain", String(value));
        } else if (entityType == FLOAT) {
            float value = _showbox->getFloatEntityValue(entityId);
            request->send(200, "text/plain", String(value));
        }
    });

    _server->on("/api/v1/showbox/entities", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_showbox == nullptr) {
            request->send(503, "text/plain", "Showbox is not available.");
            return;
        }
        const size_t cap = 12288;
        char* json = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (json == nullptr) {
            json = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (json == nullptr) {
            request->send(503, "text/plain", "The module is busy.");
            return;
        }
        size_t n = 0;
        auto append = [&](const char* text) {
            size_t len = strlen(text);
            if (n + len + 1 >= cap) {
                return;
            }
            memcpy(json + n, text, len);
            n += len;
            json[n] = '\0';
        };
        append("{\"entities\":[");
        bool first = true;
        for (uint8_t i = 0; i <= FX_BYPASS; i++) {
            entity_id entityId = static_cast<entity_id>(i);
            auto nameIt = entity_id_to_string.find(entityId);
            auto typeIt = entity_type_mapping.find(entityId);
            if (nameIt == entity_id_to_string.end() || typeIt == entity_type_mapping.end()) {
                continue;
            }
            if (!_showbox->hasEntityValue(entityId)) {
                continue;
            }
            if (!first) {
                append(",");
            }
            first = false;
            append("{\"id\":\"");
            append(nameIt->second.c_str());
            append("\",\"type\":\"");
            auto typeName = entity_data_type_to_string.find(typeIt->second);
            append(typeName == entity_data_type_to_string.end() ? "FLOAT" : typeName->second.c_str());
            append("\",\"value\":");
            if (typeIt->second == BOOL) {
                append(_showbox->getBoolEntityValue(entityId) ? "true" : "false");
            } else if (typeIt->second == UINT8) {
                char num[12];
                snprintf(num, sizeof(num), "%u", (unsigned)_showbox->getUint8EntityValue(entityId));
                append(num);
            } else {
                char num[24];
                snprintf(num, sizeof(num), "%.6g", _showbox->getFloatEntityValue(entityId));
                append(num);
            }
            append("}");
        }
        append("]}");
        sendJsonChunked(request, json, n);
        heap_caps_free(json);
    });

    _server->on("^/api/v1/showbox/entities/([a-zA-Z0-9_]+)$", HTTP_POST, [this](AsyncWebServerRequest* request) {
        String entityName = request->pathArg(0);
        auto entityIt = string_to_entity_id.find(entityName.c_str());
        if (entityIt == string_to_entity_id.end() || _showbox == nullptr) {
            request->send(404, "application/json", "{\"ok\":false}");
            return;
        }
        entity_id entityId = entityIt->second;
        entity_data_type entityType = entity_type_mapping[entityId];
        JsonDocument doc;
        String raw;
        if (jsonBody(request, doc)) {
            if (doc["value"].isNull()) {
                request->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing value.\"}");
                return;
            }
            if (entityType == BOOL) {
                bool value = false;
                if (doc["value"].is<bool>()) {
                    value = doc["value"].as<bool>();
                } else {
                    String s = doc["value"].as<String>();
                    s.toLowerCase();
                    value = s == "1" || s == "true" || s == "on";
                }
                _showbox->setEntityValue(entityId, value);
            } else if (entityType == UINT8) {
                _showbox->setEntityValue(entityId, (uint8_t)doc["value"].as<int>());
            } else {
                _showbox->setEntityValue(entityId, doc["value"].as<float>());
            }
        } else if (requireParam(request, "value", raw)) {
            if (entityType == BOOL) {
                raw.toLowerCase();
                _showbox->setEntityValue(entityId, raw == "1" || raw == "true" || raw == "on");
            } else if (entityType == UINT8) {
                _showbox->setEntityValue(entityId, (uint8_t)raw.toInt());
            } else {
                _showbox->setEntityValue(entityId, raw.toFloat());
            }
        } else {
            return;
        }
        if (_live != nullptr) {
            broadcastEntity(_live, _showbox, entityId, entityName.c_str());
        }
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    // Looper route
    _server->on("/api/v1/showbox/looper_overdub", HTTP_GET, [this](AsyncWebServerRequest* request) {
        String val;
        if (!requireParam(request, "value", val)) {
            return;
        }
        if (_actions != nullptr) {
            _actions->setVariable("looperOverdub", val == "1" || val == "true" ? 1.0f : 0.0f);
        }
        request->send(200, "application/json", "{\"ok\":true}");
    });
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
        } else if (actionName == "TAP") {
            _showbox->sendLooperButtonAction(looper_button_action::DOWN);
            _showbox->sendLooperButtonAction(looper_button_action::UP);
        } else if (actionName == "DOUBLE_PRESS") {
            _showbox->sendLooperButtonAction(looper_button_action::DOUBLE_PRESS);
        } else if (actionName == "LONG_PRESS") {
            _showbox->sendLooperButtonAction(looper_button_action::LONG_PRESS);
        } else {
            request->send(400, "text/plain", "Invalid action.");
            return;
        }
        request->send(200, "application/json", "{\"ok\":true}");
    });
    _server->on("/api/v1/showbox/action/looper_button", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        String actionName;
        if (jsonBody(request, doc) && !doc["action"].isNull()) {
            actionName = doc["action"].as<String>();
        } else if (!requireParam(request, "action", actionName)) {
            return;
        }
        actionName.toUpperCase();
        uint8_t looperNow = _showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
        uint8_t looperNext = looperNow;
        if (actionName == "DOWN") {
            _showbox->sendLooperButtonAction(looper_button_action::DOWN);
        } else if (actionName == "UP") {
            _showbox->sendLooperButtonAction(looper_button_action::UP);
        } else if (actionName == "TAP") {
            _showbox->sendLooperButtonAction(looper_button_action::DOWN);
            _showbox->sendLooperButtonAction(looper_button_action::UP);
            if (looperNow == 0) looperNext = 1;
            else if (looperNow == 1 || looperNow == 2 || looperNow == 4) looperNext = 3;
            else if (looperNow == 3) looperNext = 4;
        } else if (actionName == "DOUBLE_PRESS") {
            _showbox->sendLooperButtonAction(looper_button_action::DOUBLE_PRESS);
            if (looperNow == 1 || looperNow == 3 || looperNow == 4) looperNext = 2;
        } else if (actionName == "LONG_PRESS") {
            _showbox->sendLooperButtonAction(looper_button_action::LONG_PRESS);
            looperNext = 0;
        } else {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid action.\"}");
            return;
        }
        if (looperNext != looperNow) {
            _showbox->setLooperState(static_cast<looper_state>(looperNext));
            if (_live != nullptr) {
                _live->pushEntity("LOOPER_STATE", looperNext);
            }
        }
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    // SD Card route
    _server->on("/api/v1/showbox/action/sdcard", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/sdcard");
        _showbox->toggleSdCardRecord();
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "text/plain", "SD Card action sent.");
    });
    _server->on("/api/v1/showbox/action/sdcard", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _showbox->toggleSdCardRecord();
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    _server->on("/api/v1/showbox/action/uart_unplug", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/uart_unplug");
        uint32_t ms = 3000;
        const AsyncWebParameter* param = findParam(request, "ms");
        if (param != nullptr) {
            ms = static_cast<uint32_t>(param->value().toInt());
        }
        if (ms < 500) {
            ms = 500;
        }
        if (ms > 8000) {
            ms = 8000;
        }
        _showbox->wireUnplug(ms);
        request->send(200, "application/json", "{\"ok\":true,\"ms\":" + String((unsigned)ms) + "}");
    });
    _server->on("/api/v1/showbox/action/uart_unplug", HTTP_POST, [this](AsyncWebServerRequest* request) {
        uint32_t ms = 3000;
        JsonDocument doc;
        if (jsonBody(request, doc) && !doc["ms"].isNull()) {
            ms = static_cast<uint32_t>(doc["ms"].as<int>());
        }
        if (ms < 500) {
            ms = 500;
        }
        if (ms > 8000) {
            ms = 8000;
        }
        _showbox->wireUnplug(ms);
        request->send(200, "application/json", "{\"ok\":true,\"ms\":" + String((unsigned)ms) + "}");
    }, nullptr, collectBody);

    _server->on("/api/v1/showbox/action/autogain", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        uint8_t chan = _showbox->getSelectedChan();
        if (jsonBody(request, doc) && !doc["chan"].isNull()) {
            chan = static_cast<uint8_t>(doc["chan"].as<int>());
        } else {
            const AsyncWebParameter* param = findParam(request, "chan");
            if (param != nullptr) {
                chan = static_cast<uint8_t>(param->value().toInt());
            }
        }
        if (chan > 3) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid chan.\"}");
            return;
        }
        _showbox->sendAutogain(chan);
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    // Snapshot route
    _server->on("/api/v1/showbox/action/snapshot", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/action/snapshot");
        String actionName;
        if (!requireParam(request, "action", actionName)) {
            return;
        }
        actionName.toUpperCase();
        if (actionName == "BK") {
            _showbox->snapshotMenuBack();
        } else {
            String slotName;
            if (!requireParam(request, "slot", slotName)) {
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
        }
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "text/plain", "Snapshot action sent.");
    });
    _server->on("/api/v1/showbox/action/snapshot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        String actionName;
        int slotNum = 0;
        if (jsonBody(request, doc) && !doc["action"].isNull()) {
            actionName = doc["action"].as<String>();
            slotNum = doc["slot"].as<int>();
        } else {
            String slotName;
            if (!requireParam(request, "action", actionName) || !requireParam(request, "slot", slotName)) {
                return;
            }
            slotNum = slotName.toInt();
        }
        actionName.toUpperCase();
        snapshot_slot slot = static_cast<snapshot_slot>(slotNum);
        if (actionName == "BK") {
            _showbox->snapshotMenuBack();
        } else if (actionName == "RECALL") {
            _showbox->snapshotAction(snapshot_action::RECALL, slot);
        } else if (actionName == "SAVE") {
            _showbox->snapshotAction(snapshot_action::SAVE, slot);
        } else {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid action.\"}");
            return;
        }
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

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
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "text/plain", "Tuner action sent.");
    });
    _server->on("/api/v1/showbox/action/tuner", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        String actionName;
        int chanNum = 0;
        if (jsonBody(request, doc) && !doc["action"].isNull()) {
            actionName = doc["action"].as<String>();
            chanNum = doc["chan"].as<int>();
        } else {
            String chanName;
            if (!requireParam(request, "action", actionName) || !requireParam(request, "chan", chanName)) {
                return;
            }
            chanNum = chanName.toInt();
        }
        actionName.toUpperCase();
        if (actionName == "ON") {
            actionName = "TURN_ON";
        } else if (actionName == "OFF") {
            actionName = "TURN_OFF";
        }
        tuner_chan chan = static_cast<tuner_chan>(chanNum);
        if (actionName == "TURN_ON") {
            _showbox->tunerAction(tuner_action::TURN_ON, chan);
        } else if (actionName == "TURN_OFF") {
            _showbox->tunerAction(tuner_action::TURN_OFF, chan);
        } else {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid action.\"}");
            return;
        }
        if (_live != nullptr) {
            _live->flushLink();
        }
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    _server->on("/api/v1/showbox/analog-hold", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (_showbox == nullptr) {
            request->send(503, "application/json", "{\"ok\":false}");
            return;
        }
        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":true,\"phones\":%s,\"loop\":%s}",
            _showbox->analogHoldPhones() ? "true" : "false",
            _showbox->analogHoldLoop() ? "true" : "false");
        request->send(200, "application/json", body);
    });

    _server->on("/api/v1/showbox/analog-hold", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (_showbox == nullptr) {
            request->send(503, "application/json", "{\"ok\":false}");
            return;
        }
        bool phones = _showbox->analogHoldPhones();
        bool loop = _showbox->analogHoldLoop();
        JsonDocument doc;
        if (jsonBody(request, doc)) {
            if (doc["on"].is<bool>()) {
                phones = doc["on"].as<bool>();
                loop = phones;
            } else if (doc["on"].is<int>()) {
                phones = doc["on"].as<int>() != 0;
                loop = phones;
            }
            if (doc["phones"].is<bool>()) {
                phones = doc["phones"].as<bool>();
            } else if (doc["phones"].is<int>()) {
                phones = doc["phones"].as<int>() != 0;
            }
            if (doc["loop"].is<bool>()) {
                loop = doc["loop"].as<bool>();
            } else if (doc["loop"].is<int>()) {
                loop = doc["loop"].as<int>() != 0;
            }
        } else {
            const AsyncWebParameter* onParam = findParam(request, "on");
            const AsyncWebParameter* phonesParam = findParam(request, "phones");
            const AsyncWebParameter* loopParam = findParam(request, "loop");
            if (onParam != nullptr) {
                String raw = onParam->value();
                raw.toLowerCase();
                phones = raw != "0" && raw != "false" && raw != "off";
                loop = phones;
            }
            if (phonesParam != nullptr) {
                String raw = phonesParam->value();
                raw.toLowerCase();
                phones = raw != "0" && raw != "false" && raw != "off";
            }
            if (loopParam != nullptr) {
                String raw = loopParam->value();
                raw.toLowerCase();
                loop = raw != "0" && raw != "false" && raw != "off";
            }
        }
        _showbox->setAnalogHold(phones, loop);
        if (_live != nullptr) {
            _live->flushLink();
        }
        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":true,\"phones\":%s,\"loop\":%s}",
            phones ? "true" : "false", loop ? "true" : "false");
        request->send(200, "application/json", body);
    }, nullptr, collectBody);

    _server->on("/api/v1/showbox/names", HTTP_GET, [this](AsyncWebServerRequest* request) {
        char json[640];
        fillNamesJson(json, sizeof(json));
        request->send(200, "application/json", json);
    });
    _server->on("/api/v1/showbox/names", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        if (!jsonBody(request, doc)) {
            request->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        char chan[8][20];
        char snap[5][20];
        bool chanTouch[8] = {};
        bool snapTouch[5] = {};
        bool dirty = false;
        if (doc["channels"].is<JsonObject>()) {
            JsonObject channels = doc["channels"].as<JsonObject>();
            for (size_t i = 0; i < kChanCount; i++) {
                if (channels[kChanIds[i]].isNull()) {
                    continue;
                }
                sanitizeName(chan[i], sizeof(chan[i]), channels[kChanIds[i]].as<String>().c_str());
                chanTouch[i] = true;
                dirty = true;
            }
        }
        if (doc["snapshots"].is<JsonArray>()) {
            JsonArray snaps = doc["snapshots"].as<JsonArray>();
            for (int i = 0; i < 5; i++) {
                if (i >= static_cast<int>(snaps.size()) || snaps[i].isNull()) {
                    continue;
                }
                sanitizeName(snap[i], sizeof(snap[i]), snaps[i].as<String>().c_str());
                snapTouch[i] = true;
                dirty = true;
            }
        }
        if (!doc["snapshot"].isNull()) {
            int slot = doc["snapshot"].as<int>();
            if (slot >= 0 && slot <= 4) {
                sanitizeName(snap[slot], sizeof(snap[slot]), doc["name"].as<String>().c_str());
                snapTouch[slot] = true;
                dirty = true;
            }
        }
        if (!doc["id"].isNull()) {
            String id = doc["id"].as<String>();
            for (size_t i = 0; i < kChanCount; i++) {
                if (id != kChanIds[i]) {
                    continue;
                }
                sanitizeName(chan[i], sizeof(chan[i]), doc["name"].as<String>().c_str());
                chanTouch[i] = true;
                dirty = true;
                break;
            }
        }
        if (dirty) {
            portENTER_CRITICAL(&gNamesMux);
            for (size_t i = 0; i < kChanCount; i++) {
                if (chanTouch[i]) {
                    memcpy(gChanNames[i], chan[i], sizeof(gChanNames[i]));
                }
            }
            for (int i = 0; i < 5; i++) {
                if (snapTouch[i]) {
                    memcpy(gSnapNames[i], snap[i], sizeof(gSnapNames[i]));
                }
            }
            gNamesDirty = true;
            portEXIT_CRITICAL(&gNamesMux);
        }
        publishNames(_live);
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    _server->on("/api/v1/showbox/link", HTTP_GET, [this](AsyncWebServerRequest* request) {
        lanNoteHttp();
        if (_showbox == nullptr) {
            request->send(503, "application/json", "{\"ok\":false}");
            return;
        }
        char body[640];
        int n = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"baseAgeMs\":%u,\"mixerAgeMs\":%u,\"heartbeatAgeMs\":%u,"
            "\"mdns\":%s,\"iso\":%d,\"lock\":%u,\"pause\":%u,"
            "\"initM\":%u,\"initB\":%u,\"hbM\":%u,\"hbB\":%u,"
            "\"frmBase\":%u,\"frmMix\":%u,\"goodM\":%u,\"goodB\":%u,"
            "\"badM\":%u,\"badB\":%u,\"rxBase\":%u,\"rxMix\":%u,"
            "\"dropBase\":%u,\"dropMix\":%u,\"ovfBase\":%u,\"ovfMix\":%u,"
            "\"entM\":%u,\"parseDrop\":%u,\"analogSkip\":%u,"
            "\"fifoBase\":%u,\"fifoMix\":%u,\"dumpSeq\":%u,\"dumpTries\":%u,\"battHeard\":%s,"
            "\"unplug\":%u,\"unplugActive\":%u}",
            _showbox->linkOk(1500) ? "true" : "false",
            (unsigned)_showbox->baseAgeMs(),
            (unsigned)_showbox->mixerAgeMs(),
            (unsigned)_showbox->heartbeatAgeMs(),
            gMdnsUp ? "true" : "false",
            gIso,
            _showbox->linkOk(1500) ? 1u : 0u,
            _showbox->txToBasePaused() ? 1u : 0u,
            (unsigned)_showbox->initFromMixer(),
            (unsigned)_showbox->initFromBase(),
            (unsigned)_showbox->hbFromMixer(),
            (unsigned)_showbox->hbFromBase(),
            (unsigned)_showbox->frameErrFromBase(),
            (unsigned)_showbox->frameErrFromMixer(),
            (unsigned)_showbox->goodFromMixer(),
            (unsigned)_showbox->goodFromBase(),
            (unsigned)_showbox->badFromMixer(),
            (unsigned)_showbox->badFromBase(),
            (unsigned)_showbox->rxFromBase(),
            (unsigned)_showbox->rxFromMixer(),
            (unsigned)_showbox->droppedFromBase(),
            (unsigned)_showbox->droppedFromMixer(),
            (unsigned)_showbox->overflowFromBase(),
            (unsigned)_showbox->overflowFromMixer(),
            (unsigned)_showbox->entityFromMixer(),
            (unsigned)_showbox->parseDropped(),
            (unsigned)_showbox->analogSkipped(),
            (unsigned)_showbox->hwRxFifoBase(),
            (unsigned)_showbox->hwRxFifoMixer(),
            (unsigned)_showbox->dumpSeq(),
            (unsigned)_showbox->dumpTryCount(),
            _showbox->hasBatteryLevel() ? "true" : "false",
            (unsigned)_showbox->wireUnplugCount(),
            _showbox->wireUnplugActive() ? 1u : 0u);
        if (n < 0 || n >= (int)sizeof(body)) {
            request->send(500, "application/json", "{\"ok\":false}");
            return;
        }
        sendJsonChunked(request, body, static_cast<size_t>(n));
    });

    _server->on("/api/v1/debug/iso", HTTP_GET, [](AsyncWebServerRequest* request) {
        char body[96];
        snprintf(body, sizeof(body), "{\"iso\":%d,\"n\":%d,\"itvl\":%u,\"adv\":%s,\"advItvl\":%u}",
            gIso, bleRadio.gapLinkCount(), (unsigned)bleRadio.minConnInterval(),
            bleRadio.advertising() ? "true" : "false",
            (unsigned)bleRadio.advInterval());
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
        lanNoteHttp();
        static char tmp[6144];
        debugLogCopy(tmp, sizeof(tmp));
        size_t n = strlen(tmp);
        if (n + 48 < sizeof(tmp)) {
            int extra = snprintf(tmp + n, sizeof(tmp) - n, "heap=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (extra > 0) {
                n += static_cast<size_t>(extra);
            }
        }
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain",
            reinterpret_cast<uint8_t*>(tmp), n);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    _server->on("/api/v1/diag", HTTP_GET, [this](AsyncWebServerRequest* request) {
        lanNoteHttp();
        // AsyncWebServer sends the response after this handler returns, so the
        // buffer must outlive it. One static PSRAM buffer, like /api/v1/debug.
        static char* buf = nullptr;
        if (buf == nullptr) {
            buf = static_cast<char*>(heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (buf == nullptr) {
                buf = static_cast<char*>(heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            }
        }
        if (buf == nullptr) {
            request->send(503, "text/plain", "diag buffer");
            return;
        }
        size_t n = diagStatsJson(buf, 16384);
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
            reinterpret_cast<uint8_t*>(buf), n);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    _server->on("/api/v1/console", HTTP_GET, [](AsyncWebServerRequest* request) {
        lanNoteHttp();
        uint32_t since = 0;
        if (const AsyncWebParameter* p = findParam(request, "since")) {
            since = strtoul(p->value().c_str(), nullptr, 10);
        }
        static uint8_t chunk[2048];
        uint32_t next = since;
        size_t n = consoleCopySince(since, chunk, sizeof(chunk), &next);
        if (n == 0 && since == 0) {
            static char tmp[6144];
            n = debugLogCopy(tmp, sizeof(tmp));
            next = consoleSeq();
            if (next == 0) {
                next = 1;
            }
            AsyncWebServerResponse* response = request->beginResponse(200, "text/plain",
                reinterpret_cast<uint8_t*>(tmp), n);
            char nextBuf[16];
            snprintf(nextBuf, sizeof(nextBuf), "%u", (unsigned)next);
            response->addHeader("X-Console-Next", nextBuf);
            response->addHeader("X-Console-Dropped", "0");
            response->addHeader("Cache-Control", "no-store");
            request->send(response);
            return;
        }
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain", chunk, n);
        char nextBuf[16];
        char dropBuf[16];
        snprintf(nextBuf, sizeof(nextBuf), "%u", (unsigned)next);
        snprintf(dropBuf, sizeof(dropBuf), "%u", (unsigned)consoleDropped());
        response->addHeader("X-Console-Next", nextBuf);
        response->addHeader("X-Console-Dropped", dropBuf);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });
    _server->on("/api/v1/console", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        char cmd[96];
        cmd[0] = '\0';
        if (jsonBody(request, doc) && !doc["cmd"].isNull()) {
            strncpy(cmd, doc["cmd"].as<String>().c_str(), sizeof(cmd) - 1);
        } else if (const AsyncWebParameter* p = findParam(request, "cmd")) {
            strncpy(cmd, p->value().c_str(), sizeof(cmd) - 1);
        }
        cmd[sizeof(cmd) - 1] = '\0';
        if (cmd[0] == '\0') {
            request->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        strncpy(gConsoleCmd, cmd, sizeof(gConsoleCmd) - 1);
        gConsoleCmd[sizeof(gConsoleCmd) - 1] = '\0';
        gConsoleCmdReady = true;
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);

    // Battery Level route
    _server->on("/api/v1/showbox/status/battery", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/status/battery");
        if (_showbox == nullptr || !_showbox->hasBatteryLevel()) {
            request->send(200, "text/plain", "UNKNOWN");
            return;
        }
        request->send(200, "text/plain", String(_showbox->getBatteryLevel()));
    });

    // SD Card State route
    _server->on("/api/v1/showbox/status/sdcard", HTTP_GET, [this](AsyncWebServerRequest* request) {
        Debug->println("GET /api/v1/showbox/status/sdcard");
        sd_card_state state = _showbox->getSdCardState();
        if (!_showbox->hasSdCardState()) {
            request->send(200, "text/plain", "UNKNOWN");
        } else if (state == sd_card_state::NOT_DETECTED) {
            request->send(200, "text/plain", "NOT_DETECTED");
        } else if (state == sd_card_state::READY) {
            request->send(200, "text/plain", "READY");
        } else if (state == sd_card_state::RECORDING) {
            request->send(200, "text/plain", "RECORDING");
        } else {
            request->send(200, "text/plain", "READY");
        }
    });

    registerWebUiRoutes(_server);

    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (sendOsProbe(request)) {
            return;
        }
        if (redirectHotspotBrowser(_portal, request)) {
            return;
        }
        sendWebUi(request);
    });

    _server->on("/midi", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/services", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendHealthHtml(request);
    });

    _server->on("/api/v1/midi", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendMidiStatus(request, _connections);
    });
    // Test hook: feed a MIDI message through the same inbound dispatch the
    // transports use (BLE parser, AppleMIDI, DIN). Test-only; no port gating.
    _server->on("/api/v1/midi/inject", HTTP_POST, [this](AsyncWebServerRequest* request) {
        JsonDocument doc;
        if (!jsonBody(request, doc)) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"body required\"}");
            return;
        }
        if (!doc["packet"].isNull()) {
            // Raw UART packet test hook (hex, e.g. the looper button packet).
            if (_showbox == nullptr) {
                request->send(503, "text/plain", "Showbox is not available.");
                return;
            }
            const char* hex = doc["packet"].as<const char*>();
            uint8_t bytes[280];
            size_t n = 0;
            const char* p = hex;
            int acc = -1;
            while (*p != '\0' && n < sizeof(bytes)) {
                int v = -1;
                if (*p >= '0' && *p <= '9') v = *p - '0';
                else if (*p >= 'a' && *p <= 'f') v = 10 + (*p - 'a');
                else if (*p >= 'A' && *p <= 'F') v = 10 + (*p - 'A');
                else if (*p == ' ' || *p == ':') { ++p; continue; }
                else { request->send(400, "text/plain", "Bad hex."); return; }
                if (acc < 0) {
                    acc = v;
                } else {
                    bytes[n++] = static_cast<uint8_t>((acc << 4) | v);
                    acc = -1;
                }
                ++p;
            }
            if (n == 0 || acc >= 0) {
                request->send(400, "text/plain", "Bad hex.");
                return;
            }
            _showbox->injectPacket(bytes, n, "base");
            request->send(200, "application/json", "{\"ok\":true}");
            return;
        }
        if (doc["status"].isNull()) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"status required\"}");
            return;
        }
        uint8_t status = (uint8_t)(doc["status"].as<int>() & 0xFF);
        uint8_t d1 = (uint8_t)((doc["d1"] | 0) & 0x7F);
        uint8_t d2 = (uint8_t)((doc["d2"] | 0) & 0x7F);
        MultiMidi::testDispatch(status, d1, d2);
        request->send(200, "application/json", "{\"ok\":true}");
    }, nullptr, collectBody);
    httpsAddRoutes(_server);

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
            "wifiMidi2", "usbHid", "wifiLogHistory", "uartPackets", "acceptNewHosts", "interceptor",
            "bluetooth", "bluetoothRadio", "wifiRadio", "https"
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
        int note = 60;
        int vel = 100;
        int ch = 1;
        String kind = "note";
        if (const AsyncWebParameter* p = findParam(request, "kind")) {
            kind = p->value();
            kind.toLowerCase();
        }
        if (const AsyncWebParameter* p = findParam(request, "note")) {
            note = p->value().toInt();
        }
        if (const AsyncWebParameter* p = findParam(request, "cc")) {
            note = p->value().toInt();
            if (kind == "note") {
                kind = "cc";
            }
        }
        if (const AsyncWebParameter* p = findParam(request, "pc")) {
            note = p->value().toInt();
            if (kind == "note") {
                kind = "pc";
            }
        }
        if (const AsyncWebParameter* p = findParam(request, "val")) {
            vel = p->value().toInt();
        } else if (const AsyncWebParameter* p = findParam(request, "vel")) {
            vel = p->value().toInt();
        }
        if (const AsyncWebParameter* p = findParam(request, "ch")) {
            ch = p->value().toInt();
        }
        String error;
        if (!_connections->queueMidiTest(error, note, vel, ch, kind.c_str())) {
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
    _server->on("/wifi", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/system", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/usb", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/network", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/bluetooth", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/console", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/diag", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });
    _server->on("/debug", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendWebUi(request);
    });

    if (_portal != nullptr) {
        auto captive = [this](AsyncWebServerRequest* request) {
            if (sendOsProbe(request)) {
                return;
            }
            if (redirectHotspotBrowser(_portal, request)) {
                return;
            }
            sendOsProbeReply(request);
        };
        _server->on("/generate_204", HTTP_GET, captive);
        _server->on("/gen_204", HTTP_GET, captive);
        _server->on("/hotspot-detect.html", HTTP_GET, captive);
        _server->on("/library/test/success.html", HTTP_GET, captive);
        _server->on("/connecttest.txt", HTTP_GET, captive);
        _server->on("/ncsi.txt", HTTP_GET, captive);
        _server->on("/canonical.html", HTTP_GET, captive);
        _server->on("/success.txt", HTTP_GET, captive);
        _server->on("/kindle-wifi/wifistub.html", HTTP_GET, captive);
        _server->on("/redirect", HTTP_GET, captive);
        _server->on("/fwlink", HTTP_GET, captive);
        _server->on("/api/v1/wifi", HTTP_GET, [this](AsyncWebServerRequest* request) {
            sendJsonOk(request, _portal->statusJson());
        });
        _server->on("/api/v1/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            bool parsed = jsonBody(request, doc);
            auto flag = [&](const char* key) -> int {
                if (!parsed || doc[key].isNull()) {
                    return -1;
                }
                if (doc[key].is<bool>()) {
                    return doc[key].as<bool>() ? 1 : 0;
                }
                String raw = doc[key].as<String>();
                raw.toLowerCase();
                if (raw == "1" || raw == "true" || raw == "on") {
                    return 1;
                }
                if (raw == "0" || raw == "false" || raw == "off") {
                    return 0;
                }
                return -1;
            };
            int ap = flag("ap");
            if (ap >= 0) {
                _portal->setWantedAp(ap == 1);
            }
            int always = flag("apAlways");
            if (always >= 0) {
                _portal->setAlwaysAp(always == 1);
            }
            int apAuto = flag("apAuto");
            if (apAuto >= 0) {
                _portal->setAutoAp(apAuto == 1);
            }
            int autoConnect = flag("autoConnect");
            if (autoConnect >= 0) {
                _portal->setAutoConnect(autoConnect == 1);
            }
            if (parsed && !doc["forget"].isNull()) {
                _portal->forgetSta(doc["forget"].as<String>());
            }
            String ssid;
            String password;
            if (parsed && !doc["ssid"].isNull()) {
                ssid = doc["ssid"].as<String>();
                if (!doc["password"].isNull()) {
                    password = doc["password"].as<String>();
                }
            } else if (findParam(request, "ssid")) {
                ssid = findParam(request, "ssid")->value();
                if (findParam(request, "password")) {
                    password = findParam(request, "password")->value();
                }
            }
            if (ssid.length() > 0) {
                String error;
                if (!_portal->connectSta(ssid, password, error)) {
                    request->send(400, "application/json", String("{\"ok\":false,\"error\":\"") + error + "\"}");
                    return;
                }
            }
            sendJsonOk(request, _portal->statusJson());
        }, nullptr, collectBody);
        _server->on("/api/v1/wifi/networks", HTTP_GET, [this](AsyncWebServerRequest* request) {
            String json;
            String error;
            _portal->scanJson(json, error);
            sendJsonOk(request, json);
        });
        _server->on("/api/v1/wifi/scan", HTTP_POST, [this](AsyncWebServerRequest* request) {
            _portal->requestScan();
            request->send(200, "application/json", "{\"ok\":true,\"scanning\":true}");
        });
    }

    _server->onNotFound([this](AsyncWebServerRequest* request) {
        if (request->url().startsWith("/assets/")) {
            if (request->method() == HTTP_OPTIONS) {
                // CORS/PNA preflight from crossorigin-tag loads in stale
                // cached shells.
                AsyncWebServerResponse* pre = request->beginResponse(204);
                pre->addHeader("Access-Control-Allow-Origin", "*");
                pre->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
                pre->addHeader("Access-Control-Allow-Headers", "Range");
                pre->addHeader("Access-Control-Allow-Private-Network", "true");
                pre->addHeader("Access-Control-Max-Age", "600");
                request->send(pre);
                return;
            }
            if (request->method() != HTTP_GET) {
                request->send(404);
                return;
            }
            if (serveWebAsset(request, request->url().c_str())) {
                return;
            }
            request->send(404);
            return;
        }
        if (request->method() != HTTP_GET || request->url().startsWith("/api/") || request->url().startsWith("/ws")) {
            request->send(404);
            return;
        }
        if (sendOsProbe(request)) {
            return;
        }
        if (redirectHotspotBrowser(_portal, request)) {
            return;
        }
        sendWebUi(request);
    });
}

void RestApiRouter::tick() {
    if (gNamesDirty) {
        saveNamesToNvs();
    }
    if (gConsoleCmdReady) {
        char cmd[96];
        strncpy(cmd, gConsoleCmd, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        gConsoleCmdReady = false;
        runConsoleCmd(cmd, _showbox);
    }
    if (gConsoleReboot) {
        gConsoleReboot = false;
        ESP.restart();
    }
}

void RestApiRouter::setDebugSerial(Print* serial) {
    Debug = serial;
}