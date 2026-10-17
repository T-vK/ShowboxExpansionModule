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
    // JTAG-era note said 256 B still ACKs while BLE is live; that was the
    // broken-coex era (reboot storms, 200 ms AIRSTEP). The resumable webui
    // shell now recovers stalled transfers, so a full TCP segment per push
    // is strictly better: 1460 B needs ~6x fewer event-loop round trips for
    // the SPA bundle than 256 B.
    const size_t cap = 1460;
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
    // Full TCP segment; see fillFlash. The resumable shell makes stalls
    // recoverable, so big pushes no longer risk permanent mid-body death.
    return 1460;
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
    // no-cache, never max-age: a cached shell can reference asset hashes
    // that no longer exist after an update — that deadlock kept browsers on
    // 'Loading mixer…' forever. The shell is ~2 KB; always revalidate it.
    response->addHeader("Cache-Control", "no-cache");
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

// Resume support for the BLE-coex radio: big TCP bodies stall mid-flight for
// tens of seconds (data+ACK interleaving starves; plain ping of any size
// still flows). The webui shell therefore streams each asset and resumes
// with HTTP Range after an inactivity abort. 206 slices carry raw gzip
// bytes (no Content-Encoding — each slice must not be decoded alone).
static void sendFlashRange(
    AsyncWebServerRequest* request,
    const char* type,
    const uint8_t* data,
    size_t len,
    const char* cache
) {
    size_t start = 0;
    bool ranged = false;
    const AsyncWebHeader* range = request->getHeader("Range");
    if (range != nullptr && len > 0) {
        const char* v = range->value().c_str();
        if (strncmp(v, "bytes=", 6) == 0) {
            long s = strtol(v + 6, nullptr, 10);
            if (s >= 0 && (size_t)s < len) {
                start = (size_t)s;
                ranged = true;
            }
        }
    }
    const size_t remain = len - start;
    lanNoteHttp();
    if (remain >= 4096) {
        lanNoteBigHttp(true);
    } else {
        lanNoteHttpBody(true);
    }
    const size_t cap = spaChunkCap();
    AsyncWebServerResponse* response = request->beginResponse(
        type, remain,
        [data, len, start, remain, cap](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (index >= remain) {
                if (remain >= 4096) {
                    lanNoteBigHttp(false);
                } else {
                    lanNoteHttpBody(false);
                }
                return 0;
            }
            return fillFlash(data, len, buffer, maxLen > cap ? cap : maxLen, start + index);
        });
    if (ranged) {
        response->setCode(206);
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %u-%u/%u",
            (unsigned)start, (unsigned)(len - 1), (unsigned)len);
        response->addHeader("Content-Range", cr);
        response->addHeader("Accept-Ranges", "bytes");
        response->addHeader("Cache-Control", "no-store");
    } else {
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Accept-Ranges", "bytes");
        response->addHeader("Cache-Control", cache);
    }
    // Stale cached shells load us with crossorigin tags (cors mode) and
    // Chromium's private-network policy blocks them without this.
    response->addHeader("Access-Control-Allow-Origin", "*");
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
    // Split-build assets (hashed names): served with Range/resume support
    // so the shell can continue stalled transfers from the exact byte.
    for (size_t i = 0; i < WEBUI_ASSETS_N; i++) {
        const WebUiAsset* asset = WEBUI_ASSETS[i];
        server->on(asset->path, HTTP_GET, [asset](AsyncWebServerRequest* request) {
            sendFlashRange(request, asset->type, asset->gz, asset->len,
                "public, max-age=31536000, immutable");
        });
    }
}

bool serveWebAsset(AsyncWebServerRequest* request, const char* path) {
    // Update-deadlock breaker: a stale cached shell references asset hashes
    // that no longer exist (the old shell pointed at index-BDP4IidH.js; the
    // new build ships index-CXzm662G.js). Serving the shell HTML for those
    // paths made the browser parse HTML as a JS module — the stale browser
    // could never reach the new UI. Match by extension instead: any
    // /assets/*.js gets the current bundle, any /assets/*.css the current
    // stylesheet, so every old shell can always reach the current app.
    const char* dotReq = strrchr(path, '.');
    if (dotReq == nullptr) {
        return false;
    }
    for (size_t i = 0; i < WEBUI_ASSETS_N; i++) {
        const WebUiAsset* asset = WEBUI_ASSETS[i];
        const char* dot = strrchr(asset->path, '.');
        if (dot == nullptr || strcmp(dot, dotReq) != 0) {
            continue;
        }
        sendFlashRange(request, asset->type, asset->gz, asset->len, "no-cache");
        return true;
    }
    return false;
}
