#include "DebugLog.h"

#include <Arduino.h>

int gIso = 0;
volatile int gIsoPending = -1;
volatile uint32_t gIsoRebootAt = 0;
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static constexpr int kDebugLogLines = 48;
static constexpr int kDebugLogWidth = 120;
static char sLines[kDebugLogLines][kDebugLogWidth];
static volatile uint8_t sHead = 0;
static StaticSemaphore_t sMuxBuf;
static SemaphoreHandle_t sMux = xSemaphoreCreateMutexStatic(&sMuxBuf);

void debugLog(const char* fmt, ...) {
    char line[kDebugLogWidth];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (sMux != nullptr) {
        xSemaphoreTake(sMux, portMAX_DELAY);
    }
    uint8_t i = sHead;
    strncpy(sLines[i], line, kDebugLogWidth - 1);
    sLines[i][kDebugLogWidth - 1] = 0;
    sHead = static_cast<uint8_t>((i + 1) % kDebugLogLines);
    if (sMux != nullptr) {
        xSemaphoreGive(sMux);
    }
}

size_t debugLogCopy(char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    char tmp[kDebugLogLines][kDebugLogWidth];
    uint8_t head;
    if (sMux != nullptr) {
        xSemaphoreTake(sMux, portMAX_DELAY);
    }
    memcpy(tmp, sLines, sizeof(tmp));
    head = sHead;
    if (sMux != nullptr) {
        xSemaphoreGive(sMux);
    }
    size_t used = 0;
    out[0] = 0;
    for (int n = 0; n < kDebugLogLines; n++) {
        const char* line = tmp[(head + n) % kDebugLogLines];
        if (line[0] == 0) {
            continue;
        }
        int wrote = snprintf(out + used, cap - used, "%s\n", line);
        if (wrote < 0 || static_cast<size_t>(wrote) >= cap - used) {
            out[cap - 1] = 0;
            return cap - 1;
        }
        used += static_cast<size_t>(wrote);
    }
    return used;
}
