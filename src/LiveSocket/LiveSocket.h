#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class MackieShowbox;
class Connections;

class LiveSocket {
public:
    void begin(AsyncWebServer* server, MackieShowbox* showbox);
    void setConnections(Connections* conn) { connections = conn; }
    void tick();
    void noteEntity(const char* id, const char* value);
    void pushEntity(const char* id, uint8_t value);
    void pushEntityJson(const char* id, const char* jsonValue);
    void noteLink();
    void flushLink();
    void flushMixer();
    void setNamesJson(const char* json);
    void noteSnapshotDone(const char* action, uint8_t slot);
    void broadcastJson(const String& json);
    bool hasClients() const;

private:
    void sendMixer(AsyncWebSocketClient* client = nullptr);
    void sendLink(AsyncWebSocketClient* client = nullptr);
    void sendNames(AsyncWebSocketClient* client = nullptr);
    bool sendIfReady(AsyncWebSocketClient* client, const char* body, size_t len);
    bool sendAllReady(const char* body, size_t len);
    void rememberRetry(const char* id, const char* jsonValue);
    void flushRetries();
    void flushAnalog();
    int analogSlot(const char* id) const;

    AsyncWebSocket socket{"/ws"};
    MackieShowbox* showbox = nullptr;
    Connections* connections = nullptr;
    volatile bool linkDirty = false;
    volatile bool mixerDirty = false;
    uint32_t lastLinkMs = 0;
    uint32_t lastMixerMs = 0;
    char namesJson[640] = "{\"type\":\"names\",\"channels\":{},\"snapshots\":[\"1\",\"2\",\"3\",\"4\",\"5\"]}";
    static constexpr uint8_t kRetryCap = 8;
    char retryId[kRetryCap][28] {};
    char retryVal[kRetryCap][28] {};
    uint8_t retryN = 0;
    static constexpr uint32_t kAnalogMs = 40;
    char analogId[2][28] {};
    char analogVal[2][28] {};
    bool analogDirty[2] = {};
    uint32_t lastAnalogMs = 0;
    uint32_t lastDumpSeq = 0;
};
