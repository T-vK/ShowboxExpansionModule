#pragma once
#include <Arduino.h>

struct WebUiAsset {
    const char* path;
    const char* type;
    const uint8_t* gz;
    const size_t len;
};

static const WebUiAsset* const WEBUI_ASSETS[] = {
};
static const size_t WEBUI_ASSETS_N = 0;
