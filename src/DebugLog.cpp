#include "DebugLog.h"

#include <Arduino.h>
#include <cstring>

int gIso = 0;
volatile int gIsoPending = -1;
volatile uint32_t gIsoRebootAt = 0;
volatile bool gDiagWatch = false;
volatile int gSoakSeconds = 0;
volatile bool gSoakRunning = false;
static char gSoakJson[512] = "{\"done\":false}";

bool uartSoakResult(char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    strncpy(out, gSoakJson, cap - 1);
    out[cap - 1] = 0;
    return gSoakJson[0] != 0;
}

void uartSoakStore(const char* json) {
    if (json == nullptr) {
        return;
    }
    strncpy(gSoakJson, json, sizeof(gSoakJson) - 1);
    gSoakJson[sizeof(gSoakJson) - 1] = 0;
}
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstring>

static constexpr int kDebugLogLines = 48;
static constexpr int kDebugLogWidth = 120;
static char sLines[kDebugLogLines][kDebugLogWidth];
static volatile uint8_t sHead = 0;
static StaticSemaphore_t sMuxBuf;
static SemaphoreHandle_t sMux = xSemaphoreCreateMutexStatic(&sMuxBuf);

static void usbWriteLine(const char* line);

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
    usbWriteLine(line);
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

#include "driver/usb_serial_jtag.h"
#include <UltimateBaseFirmware.h>
extern UltimateBaseFirmware ubfw;

static bool sUsbListener = false;
static uint32_t sUsbQuietAt = 0;
static uint8_t sUsbPhase = 0;
static char sUsbDump[2048];
static size_t sUsbDumpN = 0;
static size_t sUsbDumpOff = 0;

static bool usbMonitorUp() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    return usb_serial_jtag_is_connected();
#else
    return Serial;
#endif
}

static int usbRoom() {
    return Serial.availableForWrite();
}

static void usbWriteLine(const char* line) {
    if (line == nullptr || !usbMonitorUp()) {
        return;
    }
    size_t n = strlen(line);
    int room = usbRoom();
    if (room < 8) {
        return;
    }
    if ((size_t)room < n + 2) {
        n = (size_t)room > 2 ? (size_t)room - 2 : 0;
    }
    if (n > 0) {
        Serial.write(reinterpret_cast<const uint8_t*>(line), n);
    }
    Serial.write('\n');
}

static bool usbFillDump() {
    sUsbDumpOff = 0;
    sUsbDumpN = 0;
    if (sUsbPhase == 1) {
        const char hdr[] = "\n--- ShowboxMod USB ---\n";
        memcpy(sUsbDump, hdr, sizeof(hdr) - 1);
        sUsbDumpN = sizeof(hdr) - 1;
        return true;
    }
    if (sUsbPhase == 2) {
        sUsbDumpN = debugLogCopy(sUsbDump, sizeof(sUsbDump));
        return sUsbDumpN > 0;
    }
    if (sUsbPhase == 3) {
        // Skip the wifi-log blast. Dumping ~2 KB over USB-Serial/JTAG
        // while AIRSTEP is up kills STA ARP.
        return false;
    }
    return false;
}

static void usbPumpDump() {
    if (sUsbPhase == 0 || usbRoom() < 24) {
        return;
    }
    if (sUsbDumpOff >= sUsbDumpN) {
        while (sUsbPhase >= 1 && sUsbPhase <= 3 && !usbFillDump()) {
            sUsbPhase++;
        }
        if (sUsbDumpN == 0 || sUsbPhase > 3) {
            sUsbPhase = 0;
            sUsbDumpN = 0;
            sUsbDumpOff = 0;
            return;
        }
    }
    int room = usbRoom();
    if (room < 16 || sUsbDumpOff >= sUsbDumpN) {
        return;
    }
    size_t chunk = sUsbDumpN - sUsbDumpOff;
    if ((size_t)room < chunk) {
        chunk = (size_t)room;
    }
    if (chunk > 256) {
        chunk = 256;
    }
    Serial.write(reinterpret_cast<const uint8_t*>(sUsbDump + sUsbDumpOff), chunk);
    sUsbDumpOff += chunk;
    if (sUsbDumpOff >= sUsbDumpN) {
        sUsbPhase++;
        sUsbDumpN = 0;
        sUsbDumpOff = 0;
        if (sUsbPhase > 3) {
            sUsbPhase = 0;
        }
    }
}

void debugLogUsbTick() {
    bool up = usbMonitorUp();
    int room = up ? usbRoom() : 0;
    bool listening = up && room >= 48;
    if (!listening) {
        if (sUsbListener) {
            if (sUsbQuietAt == 0) {
                sUsbQuietAt = millis();
            }
            if ((int32_t)(millis() - sUsbQuietAt) > 600) {
                sUsbListener = false;
                sUsbPhase = 0;
                sUsbDumpN = 0;
                sUsbDumpOff = 0;
            }
        }
        return;
    }
    sUsbQuietAt = 0;
    if (!sUsbListener) {
        sUsbListener = true;
        sUsbPhase = 1;
        sUsbDumpN = 0;
        sUsbDumpOff = 0;
    }
    usbPumpDump();
}

size_t consoleCopySince(uint32_t since, uint8_t* out, size_t cap, uint32_t* next) {
    return ubfw.Debug.copyWifiSince(since, out, cap, next);
}

uint32_t consoleDropped() {
    return ubfw.Debug.wifiDropped();
}

uint32_t consoleSeq() {
    return ubfw.Debug.wifiSeq();
}

void consoleClear() {
    ubfw.Debug.clearWifi();
}
