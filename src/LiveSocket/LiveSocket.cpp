#include "LiveSocket.h"
#include "../System/Diag.h"

#include "MackieShowbox/MackieShowbox.h"
#include "Connections/Connections.h"
#include "System/LanKeep.h"
#include "string_mappings.h"
#include "constants.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

bool LiveSocket::sendIfReady(AsyncWebSocketClient* client, const char* body, size_t len) {
    if (client == nullptr || body == nullptr || len == 0) {
        return false;
    }
    if (client->status() != WS_CONNECTED || client->queueIsFull() || !client->canSend()) {
        return false;
    }
    client->text(body, len);
    diagNoteWsTx(len);
    lanNoteHttp();
    return true;
}

bool LiveSocket::sendAllReady(const char* body, size_t len) {
    bool any = false;
    for (AsyncWebSocketClient& client : socket.getClients()) {
        if (sendIfReady(&client, body, len)) {
            any = true;
        }
    }
    return any;
}

void LiveSocket::rememberRetry(const char* id, const char* jsonValue) {
    if (id == nullptr || jsonValue == nullptr) {
        return;
    }
    diagNoteWsRetry();
    for (uint8_t i = 0; i < retryN; i++) {
        if (strcmp(retryId[i], id) == 0) {
            strncpy(retryVal[i], jsonValue, sizeof(retryVal[i]) - 1);
            retryVal[i][sizeof(retryVal[i]) - 1] = '\0';
            return;
        }
    }
    if (retryN >= kRetryCap) {
        memmove(retryId[0], retryId[1], (kRetryCap - 1) * sizeof(retryId[0]));
        memmove(retryVal[0], retryVal[1], (kRetryCap - 1) * sizeof(retryVal[0]));
        retryN = kRetryCap - 1;
    }
    strncpy(retryId[retryN], id, sizeof(retryId[0]) - 1);
    strncpy(retryVal[retryN], jsonValue, sizeof(retryVal[0]) - 1);
    retryId[retryN][sizeof(retryId[0]) - 1] = '\0';
    retryVal[retryN][sizeof(retryVal[0]) - 1] = '\0';
    retryN = static_cast<uint8_t>(retryN + 1);
}

void LiveSocket::flushRetries() {
    uint8_t i = 0;
    while (i < retryN) {
        const bool raw = retryVal[i][0] == '-' || retryVal[i][0] == '+'
            || (retryVal[i][0] >= '0' && retryVal[i][0] <= '9')
            || retryVal[i][0] == 't' || retryVal[i][0] == 'f';
        char body[160];
        int n = snprintf(body, sizeof(body),
            "{\"type\":\"entity\",\"id\":\"%s\",\"value\":%s%s%s}",
            retryId[i], raw ? "" : "\"", retryVal[i], raw ? "" : "\"");
        if (n <= 0 || !sendAllReady(body, static_cast<size_t>(n))) {
            break;
        }
        if (i + 1 < retryN) {
            memmove(retryId[i], retryId[i + 1], (retryN - i - 1) * sizeof(retryId[0]));
            memmove(retryVal[i], retryVal[i + 1], (retryN - i - 1) * sizeof(retryVal[0]));
        }
        retryN = static_cast<uint8_t>(retryN - 1);
    }
}

void LiveSocket::begin(AsyncWebServer* server, MackieShowbox* showboxIn) {
    showbox = showboxIn;
    if (server == nullptr) {
        return;
    }
    socket.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type,
        void* arg, uint8_t* data, size_t len) {
        (void)server;
        (void)arg;
        if (type == WS_EVT_CONNECT && client != nullptr) {
            client->setCloseClientOnQueueFull(false);
            // JTAG: a 5–12 KB mixer snapshot on connect sat on unsent
            // (unacked nil) and HOL-blocked link/pong. The UI 12s
            // watchdog toasted "Web UI disconnected" even with AIRSTEP
            // still down. Mixer state is loaded over HTTP; WS keeps
            // link + entity deltas.
            lanNoteWsClients(socket.count());
            lanNoteWsRx();
            sendLink(client);
            sendNames(client);
            return;
        }
        if (type == WS_EVT_DISCONNECT) {
            lanNoteWsClients(socket.count());
            return;
        }
        if (type != WS_EVT_DATA || client == nullptr || data == nullptr || len == 0) {
            return;
        }
        if (data[0] == 'p') {
            lanNoteWsRx();
            sendIfReady(client, "{\"type\":\"pong\"}", 16);
            return;
        }
        if (data[0] == 'm') {
            return;
        }
        if (data[0] == 'l') {
            sendLink(client);
        }
        if (data[0] == 's' && showbox != nullptr) {
            // Snapshot actions over WS: reliable when HTTP POSTs fail during
            // radio windows. data = "s" + JSON {"action":"SAVE","slot":0}
            JsonDocument doc;
            if (deserializeJson(doc, data + 1, len - 1) == DeserializationError::Ok) {
                const char* action = doc["action"] | "";
                uint8_t slot = doc["slot"] | 0;
                if (strcmp(action, "SAVE") == 0) {
                    showbox->snapshotAction(SAVE, static_cast<snapshot_slot>(slot));
                } else if (strcmp(action, "RECALL") == 0) {
                    showbox->snapshotAction(RECALL, static_cast<snapshot_slot>(slot));
                } else if (strcmp(action, "BK") == 0) {
                    showbox->snapshotMenuBack();
                }
            }
            return;
        }
    });
    server->addHandler(&socket);
}

bool LiveSocket::hasClients() const {
    return socket.count() > 0;
}

int LiveSocket::analogSlot(const char* id) const {
    if (id == nullptr) {
        return -1;
    }
    if (strcmp(id, "MAIN_HEADPHONE_GAIN") == 0) {
        return 0;
    }
    if (strcmp(id, "LOOPER_LEVEL") == 0) {
        return 1;
    }
    return -1;
}

void LiveSocket::flushAnalog() {
    lastAnalogMs = millis();
    for (uint8_t i = 0; i < 2; i++) {
        if (!analogDirty[i] || analogId[i][0] == '\0') {
            continue;
        }
        analogDirty[i] = false;
        pushEntityJson(analogId[i], analogVal[i]);
    }
}

void LiveSocket::noteEntity(const char* id, const char* value) {
    if (id == nullptr || socket.count() == 0) {
        return;
    }
    if (value == nullptr || value[0] == '\0') {
        mixerDirty = true;
        return;
    }
    int slot = analogSlot(id);
    if (slot >= 0) {
        strncpy(analogId[slot], id, sizeof(analogId[slot]) - 1);
        analogId[slot][sizeof(analogId[slot]) - 1] = '\0';
        strncpy(analogVal[slot], value, sizeof(analogVal[slot]) - 1);
        analogVal[slot][sizeof(analogVal[slot]) - 1] = '\0';
        analogDirty[slot] = true;
        uint32_t now = millis();
        if (lastAnalogMs == 0 || (int32_t)(now - lastAnalogMs) >= (int32_t)kAnalogMs) {
            flushAnalog();
        }
        return;
    }
    pushEntityJson(id, value);
}

void LiveSocket::pushEntity(const char* id, uint8_t value) {
    char num[12];
    snprintf(num, sizeof(num), "%u", (unsigned)value);
    pushEntityJson(id, num);
}

void LiveSocket::pushEntityJson(const char* id, const char* jsonValue) {
    if (id == nullptr || jsonValue == nullptr || socket.count() == 0) {
        return;
    }
    const bool raw = jsonValue[0] == '-' || jsonValue[0] == '+'
        || (jsonValue[0] >= '0' && jsonValue[0] <= '9')
        || jsonValue[0] == 't' || jsonValue[0] == 'f';
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"type\":\"entity\",\"id\":\"%s\",\"value\":%s%s%s}",
        id, raw ? "" : "\"", jsonValue, raw ? "" : "\"");
    if (n > 0 && !sendAllReady(body, static_cast<size_t>(n))) {
        rememberRetry(id, jsonValue);
    }
}

void LiveSocket::noteLink() {
    linkDirty = true;
}

void LiveSocket::flushLink() {
    linkDirty = false;
    lastLinkMs = millis();
    sendLink(nullptr);
}

void LiveSocket::flushMixer() {
    mixerDirty = false;
    lastMixerMs = millis();
}

void LiveSocket::setNamesJson(const char* json) {
    if (json == nullptr || json[0] == '\0') {
        return;
    }
    strncpy(namesJson, json, sizeof(namesJson) - 1);
    namesJson[sizeof(namesJson) - 1] = '\0';
    if (socket.count() > 0) {
        sendNames(nullptr);
    }
}

void LiveSocket::noteSnapshotDone(const char* action, uint8_t slot) {
    char body[96];
    int n = snprintf(body, sizeof(body),
        "{\"type\":\"snapshot\",\"ok\":true,\"action\":\"%s\",\"slot\":%u}",
        action == nullptr ? "RECALL" : action, (unsigned)slot);
    if (n > 0) {
        sendAllReady(body, static_cast<size_t>(n));
    }
    flushMixer();
    flushLink();
}

void LiveSocket::broadcastJson(const String& json) {
    if (json.length() == 0 || socket.count() == 0) {
        return;
    }
    sendAllReady(json.c_str(), json.length());
}

void LiveSocket::sendLink(AsyncWebSocketClient* client) {
    if (showbox == nullptr) {
        return;
    }
    char batt[16];
    if (showbox->hasBatteryLevel()) {
        snprintf(batt, sizeof(batt), "%.1f", (double)showbox->getBatteryLevel());
    } else {
        memcpy(batt, "null", 5);
    }
    char namesBuf[192] = "";
    if (connections != nullptr) {
        connections->bleConnectedNames(namesBuf, sizeof(namesBuf));
    }
    char body[768];
    int n = snprintf(body, sizeof(body),
        "{\"type\":\"link\",\"ok\":%s,\"baseAgeMs\":%u,\"mixerAgeMs\":%u,\"battery\":%s,\"tunerOn\":%s,\"tunerChan\":%u,\"tunerNoteId\":%u,\"tunerMidi\":%u,\"tunerCents\":%.1f,\"tunerHz\":%.2f,\"tunerHeard\":%s,\"sdCard\":%u,\"sdHeard\":%s,\"analogHoldPhones\":%s,\"analogHoldLoop\":%s,\"dumpSeq\":%u,\"bleOn\":%s,\"bleHosts\":%u,\"bleNames\":\"%s\",\"recSec\":%u,\"recPre\":%s}",
        showbox->linkOk(1500) ? "true" : "false",
        (unsigned)showbox->baseAgeMs(),
        (unsigned)showbox->mixerAgeMs(),
        batt,
        showbox->getTunerState() ? "true" : "false",
        (unsigned)showbox->getTunerChan(),
        (unsigned)showbox->getTunerNoteId(),
        (unsigned)showbox->getTunerMidi(),
        (double)showbox->getTunerCents(),
        (double)showbox->getTunerHz(),
        showbox->getTunerHeard() ? "true" : "false",
        (unsigned)showbox->getSdCardState(),
        showbox->hasSdCardState() ? "true" : "false",
        showbox->analogHoldPhones() ? "true" : "false",
        showbox->analogHoldLoop() ? "true" : "false",
        (unsigned)showbox->dumpSeq(),
        connections != nullptr && connections->bluetoothRadioOn() ? "true" : "false",
        connections != nullptr && connections->bluetoothRadioOn() ? (unsigned)connections->bleLinkCount() : 0u,
        namesBuf,
        showbox->getRecStartMs() != 0 ? (unsigned)((millis() - showbox->getRecStartMs()) / 1000) : 0u,
        showbox->getRecPreBoot() ? "true" : "false");
    if (n <= 0) {
        return;
    }
    if (client != nullptr) {
        sendIfReady(client, body, static_cast<size_t>(n));
    } else {
        sendAllReady(body, static_cast<size_t>(n));
    }
}

void LiveSocket::sendMixer(AsyncWebSocketClient* client) {
    if (showbox == nullptr) {
        return;
    }
    const size_t cap = 12288;
    char* json = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (json == nullptr) {
        json = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (json == nullptr) {
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
    append("{\"type\":\"mixer\",\"entities\":[");
    bool first = true;
    for (uint8_t i = 0; i <= FX_BYPASS; i++) {
        entity_id entityId = static_cast<entity_id>(i);
        auto nameIt = entity_id_to_string.find(entityId);
        auto typeIt = entity_type_mapping.find(entityId);
        if (nameIt == entity_id_to_string.end() || typeIt == entity_type_mapping.end()) {
            continue;
        }
        if (!showbox->hasEntityValue(entityId)) {
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
        char num[24];
        if (typeIt->second == BOOL) {
            append(showbox->getBoolEntityValue(entityId) ? "true" : "false");
        } else if (typeIt->second == UINT8) {
            snprintf(num, sizeof(num), "%u", (unsigned)showbox->getUint8EntityValue(entityId));
            append(num);
        } else {
            snprintf(num, sizeof(num), "%.6g", showbox->getFloatEntityValue(entityId));
            append(num);
        }
        append("}");
    }
    append("]}");
    if (client != nullptr) {
        sendIfReady(client, json, n);
    } else {
        sendAllReady(json, n);
    }
    heap_caps_free(json);
}

void LiveSocket::sendNames(AsyncWebSocketClient* client) {
    size_t n = strlen(namesJson);
    if (n == 0) {
        return;
    }
    if (client != nullptr) {
        sendIfReady(client, namesJson, n);
    } else {
        sendAllReady(namesJson, n);
    }
}

void LiveSocket::tick() {
    socket.cleanupClients();
    lanNoteWsClients(socket.count());
    if (retryN > 0 && socket.count() > 0) {
        flushRetries();
    }
    if (socket.count() > 0 && (analogDirty[0] || analogDirty[1])) {
        uint32_t now = millis();
        if (lastAnalogMs == 0 || (int32_t)(now - lastAnalogMs) >= (int32_t)kAnalogMs) {
            flushAnalog();
        }
    }
    if (showbox != nullptr) {
        uint8_t action = 0;
        uint8_t slot = 0;
        if (showbox->takeSnapshotDone(action, slot)) {
            noteSnapshotDone(action == 2 ? "SAVE" : "RECALL", slot);
        }
    }
    if (socket.count() == 0) {
        mixerDirty = false;
        return;
    }
    uint32_t now = millis();
    if (mixerDirty) {
        mixerDirty = false;
        lastMixerMs = now;
    }
    if (showbox != nullptr) {
        const uint32_t dump = showbox->dumpSeq();
        if (dump != lastDumpSeq) {
            lastDumpSeq = dump;
            linkDirty = true;
        }
    }
    uint32_t linkMs = (showbox != nullptr && showbox->getTunerState()) ? 80 : (lanBleLive() ? 1000 : 400);
    uint32_t dirtyMs = 40;
    if ((linkDirty && (int32_t)(now - lastLinkMs) >= (int32_t)dirtyMs) || (int32_t)(now - lastLinkMs) >= (int32_t)linkMs) {
        linkDirty = false;
        lastLinkMs = now;
        sendLink(nullptr);
    }
}
