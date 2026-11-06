#ifndef REST_API_ROUTER_H
#define REST_API_ROUTER_H

#include <ESPAsyncWebServer.h>
#include "MackieShowbox/MackieShowbox.h"
#include "Connections/Connections.h"
#include "string_mappings.h"
#include "constants.h"

class ActionRules;
class LiveSocket;
class CaptivePortal;
class BleRadio;

class RestApiRouter {
public:
    RestApiRouter();
    void setup();
    void tick();
    void setWebServer(AsyncWebServer* server);
    void setShowbox(MackieShowbox* showbox);
    void setConnections(Connections* connections);
    void setActions(ActionRules* actions);
    void setLiveSocket(LiveSocket* socket);
    void setBleRadio(BleRadio* radio);
    void setCaptivePortal(CaptivePortal* portal);
    void setDebugSerial(Print* serial);

private:
    AsyncWebServer* _server;
    MackieShowbox* _showbox;
    Connections* _connections = nullptr;
    ActionRules* _actions = nullptr;
    LiveSocket* _live = nullptr;
    BleRadio* _bleRadio = nullptr;
    CaptivePortal* _portal = nullptr;
    Print *Debug = &Serial;
};

#endif // REST_API_ROUTER_H