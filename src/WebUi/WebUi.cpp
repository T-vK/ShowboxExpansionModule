#include "WebUi.h"
#include "Page.h"
#include "WebAssets.h"
#include "PwaAssets.h"
#include "System/LanKeep.h"
#include <esp_heap_caps.h>
#include <cstring>

static const char HEALTH_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>ShowboxMod</title></head>
<body>
<h1>ShowboxMod</h1>
<p>Mixer remote, actions, system, console, and debug live on showbox.local.</p>
<p><a href="/">Open the mixer</a></p>
<!-- health 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
-->
</body></html>
)HTML";

static size_t fillFlash(const uint8_t* data, size_t len, uint8_t* buffer, size_t maxLen, size_t index) {
    if (data == nullptr || buffer == nullptr || index >= len || maxLen == 0) {
        return 0;
    }
    size_t n = len - index;
    if (n > maxLen) {
        n = maxLen;
    }
    // JTAG: 1024 B SPA chunks + AIRSTEP MIDI left :80 unacked (nrtx=7).
    // 256 B still ACKs while BLE is live; 1 KB is fine after MIDI CCC drops.
    const size_t cap = lanBleLive() ? 256 : 1024;
    if (n > cap) {
        n = cap;
    }
    memcpy_P(buffer, data + index, n);
    return n;
}

static size_t tcpBodyChunk(size_t remain, size_t maxLen, size_t cap) {
    size_t n = remain;
    if (n > maxLen) {
        n = maxLen;
    }
    if (n > cap) {
        n = cap;
    }
    return n;
}

static size_t spaChunkCap() {
    return lanBleLive() ? 256 : 1024;
}

void sendJsonChunked(AsyncWebServerRequest* request, const char* json, size_t len) {
    lanNoteHttp();
    lanNoteHttpBody(true);
    if (json == nullptr || len == 0) {
        lanNoteHttpBody(false);
        request->send(503, "application/json", "{\"ok\":false}");
        return;
    }
    char* copy = static_cast<char*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        lanNoteHttpBody(false);
        request->send(503, "text/plain", "The module is busy.");
        return;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';
    request->_tempObject = copy;
    // /services answers with 256 B callbacks; String beginResponse does
    // not (JTAG: async_tcp idle, client sees SYN-ACK then no body).
    AsyncWebServerResponse* response = request->beginResponse(
        "application/json", len,
        [copy, len](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= len) {
                lanNoteHttpBody(false);
                return 0;
            }
            size_t n = tcpBodyChunk(len - index, maxLen, 256);
            memcpy(buffer, copy + index, n);
            return n;
        });
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
}

void sendHealthHtml(AsyncWebServerRequest* request) {
    lanNoteHttp();
    lanNoteHttpBody(true);
    const size_t len = sizeof(HEALTH_HTML) - 1;
    AsyncWebServerResponse* response = request->beginResponse(
        "text/html", len,
        [](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= len) {
                lanNoteHttpBody(false);
                return 0;
            }
            size_t n = tcpBodyChunk(len - index, maxLen, 256);
            memcpy(buffer, HEALTH_HTML + index, n);
            return n;
        });
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
}

static uint8_t* gWebUiRam = nullptr;

static const uint8_t* webUiBytes() {
    if (gWebUiRam == nullptr) {
        gWebUiRam = static_cast<uint8_t*>(heap_caps_malloc(WEBUI_INDEX_GZ_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (gWebUiRam != nullptr) {
            memcpy_P(gWebUiRam, WEBUI_INDEX_GZ, WEBUI_INDEX_GZ_LEN);
        }
    }
    return gWebUiRam != nullptr ? gWebUiRam : WEBUI_INDEX_GZ;
}

void preloadWebUi() {
    webUiBytes();
}

size_t webUiGzSize() {
    return WEBUI_INDEX_GZ_LEN;
}

const uint8_t* webUiGzData() {
    return webUiBytes();
}

void sendWebUi(AsyncWebServerRequest* request) {
    lanNoteHttp();
    lanNoteBigHttp(true);
    const uint8_t* src = webUiBytes();
    const size_t cap = spaChunkCap();
    AsyncWebServerResponse* response = request->beginResponse(
        "text/html", WEBUI_INDEX_GZ_LEN,
        [src, cap](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= WEBUI_INDEX_GZ_LEN) {
                lanNoteBigHttp(false);
                return 0;
            }
            lanTouchBigHttp();
            size_t n = tcpBodyChunk(WEBUI_INDEX_GZ_LEN - index, maxLen, cap);
            memcpy(buffer, src + index, n);
            return n;
        });
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=600");
    response->addHeader("Connection", "close");
    request->send(response);
}

static void sendFlashBytes(
    AsyncWebServerRequest* request,
    const char* type,
    const uint8_t* data,
    size_t len,
    const char* cache,
    bool gz = false
) {
    lanNoteHttp();
    if (len >= 4096) {
        lanNoteBigHttp(true);
    } else {
        lanNoteHttpBody(true);
    }
    const size_t cap = spaChunkCap();
    AsyncWebServerResponse* response = request->beginResponse(
        type, len,
        [data, len, cap](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= len) {
                if (len >= 4096) {
                    lanNoteBigHttp(false);
                } else {
                    lanNoteHttpBody(false);
                }
                return 0;
            }
            return fillFlash(data, len, buffer, maxLen > cap ? cap : maxLen, index);
        });
    if (gz) {
        response->addHeader("Content-Encoding", "gzip");
    }
    response->addHeader("Cache-Control", cache);
    request->send(response);
}

static void sendTextAsset(AsyncWebServerRequest* request, const char* type, const char* text, const char* cache) {
    lanNoteHttp();
    lanNoteHttpBody(true);
    if (text == nullptr) {
        lanNoteHttpBody(false);
        request->send(503, "text/plain", "The module is busy.");
        return;
    }
    const size_t len = strlen(text);
    AsyncWebServerResponse* response = request->beginResponse(
        type, len,
        [text, len](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= len) {
                lanNoteHttpBody(false);
                return 0;
            }
            size_t n = tcpBodyChunk(len - index, maxLen, 256);
            memcpy(buffer, text + index, n);
            return n;
        });
    response->addHeader("Cache-Control", cache);
    response->addHeader("Connection", "close");
    request->send(response);
}

void registerWebUiRoutes(AsyncWebServer* server) {
    if (server == nullptr) {
        return;
    }
    server->on("/manifest.webmanifest", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendTextAsset(request, "application/manifest+json", PWA_MANIFEST, "no-cache");
    });
    server->on("/sw.js", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendTextAsset(request, "application/javascript", PWA_SW, "no-cache");
    });
    server->on("/icon-192.png", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendFlashBytes(request, "image/png", PWA_ICON_192, PWA_ICON_192_LEN, "public, max-age=86400");
    });
    server->on("/icon-512.png", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendFlashBytes(request, "image/png", PWA_ICON_512, PWA_ICON_512_LEN, "public, max-age=86400");
    });
    server->on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendFlashBytes(request, "image/png", PWA_ICON_180, PWA_ICON_180_LEN, "public, max-age=86400");
    });
    server->on("/icon-180.png", HTTP_GET, [](AsyncWebServerRequest* request) {
        sendFlashBytes(request, "image/png", PWA_ICON_180, PWA_ICON_180_LEN, "public, max-age=86400");
    });
    // Split-build assets (hashed names, immutable): many small gz bodies
    // survive the BLE-coex radio window far better than one 71 KB file, and
    // the service worker caches each after the first good fetch.
    for (size_t i = 0; i < WEBUI_ASSETS_N; i++) {
        const WebUiAsset* asset = WEBUI_ASSETS[i];
        server->on(asset->path, HTTP_GET, [asset](AsyncWebServerRequest* request) {
            sendFlashBytes(request, asset->type, asset->gz, asset->len,
                "public, max-age=31536000, immutable", true);
        });
    }
}
