#ifndef REST_API_ROUTER_H
#define REST_API_ROUTER_H

#include <ESPAsyncWebServer.h>
#include "MackieShowbox/MackieShowbox.h"
#include "string_mappings.h"
#include "constants.h"

class RestApiRouter {
public:
    RestApiRouter();
    void setup();
    void setWebServer(AsyncWebServer* server);
    void setShowbox(MackieShowbox* showbox);
    void setDebugSerial(Print* serial);

private:
    String getBodyParam(AsyncWebServerRequest* request, const String& paramName);
    String parseFormData(const String& body, const String& paramName);
    AsyncWebServer* _server;
    MackieShowbox* _showbox;
    Print *Debug = &Serial;
};

#endif // REST_API_ROUTER_H