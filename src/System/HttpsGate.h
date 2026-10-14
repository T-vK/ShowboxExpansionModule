#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

void httpsLoad();
void httpsTick();
void httpsWaitReady(uint32_t timeoutMs);
void httpsAllowBind();
void httpsOnMdnsStart();
bool httpsWanted();
bool httpsListening();
bool httpsHoldOutgoing();
bool httpsSetEnabled(bool on, String& error);
bool httpsEnsureCa(String& error);
const char* httpsCaPem();
String httpsStatusJson();
String httpsLinkJson();
void httpsAddRoutes(AsyncWebServer* server);
