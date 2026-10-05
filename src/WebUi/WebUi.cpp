#include "WebUi.h"
#include "Page.h"

void sendWebUi(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "text/html", WEBUI_INDEX_GZ, WEBUI_INDEX_GZ_LEN);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
}
