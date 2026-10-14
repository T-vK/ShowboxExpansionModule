#pragma once

#include <ESPAsyncWebServer.h>

void sendWebUi(AsyncWebServerRequest* request);
void sendHealthHtml(AsyncWebServerRequest* request);
void sendJsonChunked(AsyncWebServerRequest* request, const char* json, size_t len);
void preloadWebUi();
void registerWebUiRoutes(AsyncWebServer* server);
size_t webUiGzSize();
const uint8_t* webUiGzData();
