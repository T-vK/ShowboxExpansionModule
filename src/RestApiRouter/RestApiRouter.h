#ifndef REST_API_ROUTER_H
#define REST_API_ROUTER_H

#include <ESPAsyncWebServer.h>
#include "MackieShowbox/MackieShowbox.h"
#include "Connections/Connections.h"
#include "string_mappings.h"
#include "constants.h"

class ActionRules;

class RestApiRouter {
public:
    RestApiRouter();
    void setup();
    void setWebServer(AsyncWebServer* server);
    void setShowbox(MackieShowbox* showbox);
    void setConnections(Connections* connections);
    void setActions(ActionRules* actions);
    void setDebugSerial(Print* serial);

private:
    AsyncWebServer* _server;
    MackieShowbox* _showbox;
    Connections* _connections = nullptr;
    ActionRules* _actions = nullptr;
    Print *Debug = &Serial;
};

#endif // REST_API_ROUTER_H