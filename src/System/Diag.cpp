#include "Diag.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "LanKeep.h"
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include <UltimateBaseFirmware.h>
#include <esp_timer.h>

extern MackieShowbox showbox;
extern MultiMidi multiMidi;
extern UltimateBaseFirmware ubfw;
extern bool wifiRadioOn;

namespace {

constexpr uint32_t kRtcMagic = 0x44494147; // "DIAG"
constexpr int kRing = 64;
constexpr int kRtcEntries = 18;
constexpr uint32_t kSampleMs = 5000;
constexpr uint32_t kKindGapMs = 2000;
constexpr size_t kStatsCap = 12288;
constexpr size_t kCrashText = 560;

struct DiagEntry {
    uint32_t ms;
    char kind[10];
    char msg[104];
};

struct DiagRtc {
    uint32_t magic;
    uint32_t bootCount;
    uint16_t head;
    uint16_t count;
    uint32_t pad;
    DiagEntry entries[kRtcEntries];
};

struct CrashLog {
    char text[kCrashText];
};

DiagEntry* ring = nullptr;
uint16_t ringHead = 0;
uint16_t ringCount = 0;
DiagRtc rtcBuf __attribute__((section(".rtc_noinit")));

CrashLog* crashSlots = nullptr;
uint8_t crashCount = 0;

volatile uint32_t wsTxTotal = 0;
volatile uint32_t wsRetries = 0;
volatile uint32_t midiOutMsgs = 0;
volatile uint32_t midiOutBytes = 0;

struct FlashStats {
    size_t sketch = 0;
    size_t free = 0;
    size_t part = 0;
    size_t chip = 0;
    bool nvsOk = false;
    uint32_t nvsUsed = 0;
    uint32_t nvsFree = 0;
};
FlashStats flashStats;

uint32_t lastSampleMs = 0;
uint32_t kindLastMs[8] = {};
char kindNames[8][10] = {};
uint8_t kindCount = 0;
bool heapLowLogged = false;
bool linkDeadLogged = false;
int linkDeadSamples = 0;
int lastWsClients = -1;
int lastWifiStatus = -1;
int lastRssi = 0;
uint32_t bootUptime0 = 0;

struct TaskPrev {
    UBaseType_t number;
    configRUN_TIME_COUNTER_TYPE runtime;
};
TaskPrev taskPrev[32];
UBaseType_t taskPrevCount = 0;
configRUN_TIME_COUNTER_TYPE totalPrev = 0;
uint32_t statsWindowMs = 0;

const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        default: return "UNKNOWN";
    }
}

bool isCrashReason(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT
        || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

void ringPush(uint32_t ms, const char* kind, const char* msg) {
    DiagEntry& e = ring[ringHead];
    e.ms = ms;
    strncpy(e.kind, kind, sizeof(e.kind) - 1);
    e.kind[sizeof(e.kind) - 1] = '\0';
    strncpy(e.msg, msg, sizeof(e.msg) - 1);
    e.msg[sizeof(e.msg) - 1] = '\0';
    ringHead = static_cast<uint16_t>((ringHead + 1) % kRing);
    if (ringCount < kRing) {
        ringCount++;
    }
}

void appendStr(char*& out, size_t& left, const char* text) {
    size_t n = strlen(text);
    if (n > left - 1) {
        n = left - 1;
    }
    memcpy(out, text, n);
    out += n;
    left -= n;
    *out = '\0';
}

void appendFmt(char*& out, size_t& left, const char* fmt, ...) {
    char tmp[160];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n > 0) {
        appendStr(out, left, tmp);
    }
}

// Indexed by task number so deltas survive task churn.
static TaskStatus_t* taskBuf = nullptr;

void sampleTasks(char*& out, size_t& left, uint32_t now) {
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > 32) {
        n = 32;
    }
    if (taskBuf == nullptr) {
        taskBuf = static_cast<TaskStatus_t*>(heap_caps_calloc(32, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (taskBuf == nullptr) {
            taskBuf = static_cast<TaskStatus_t*>(heap_caps_calloc(32, sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
    if (taskBuf == nullptr) {
        appendStr(out, left, ",\"tasks\":[],\"load\":[0,0]");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    n = uxTaskGetSystemState(taskBuf, n, &total);
    configRUN_TIME_COUNTER_TYPE deltaSum = total >= totalPrev ? total - totalPrev : 0;
    double idleDelta[2] = {0, 0};
    double coreDelta[2] = {0, 0};
    appendStr(out, left, ",\"tasks\":[");
    for (UBaseType_t i = 0; i < n; i++) {
        TaskStatus_t& t = taskBuf[i];
        configRUN_TIME_COUNTER_TYPE delta = 0;
        for (UBaseType_t p = 0; p < taskPrevCount; p++) {
            if (taskPrev[p].number == t.xTaskNumber) {
                delta = t.ulRunTimeCounter >= taskPrev[p].runtime
                    ? t.ulRunTimeCounter - taskPrev[p].runtime : 0;
                taskPrev[p].runtime = t.ulRunTimeCounter;
                break;
            }
        }
        if (i >= taskPrevCount && taskPrevCount < 32) {
            taskPrev[taskPrevCount].number = t.xTaskNumber;
            taskPrev[taskPrevCount].runtime = t.ulRunTimeCounter;
            taskPrevCount++;
        }
        double cpu = deltaSum > 0 ? (100.0 * delta) / deltaSum : 0.0;
        BaseType_t core = -1;
#if configTASKLIST_INCLUDE_COREID == 1
        core = t.xCoreID;
#endif
        const char* name = t.pcTaskName ? t.pcTaskName : "?";
        if (core == 0 || core == 1) {
            coreDelta[core] += delta;
            if (strncmp(name, "IDLE", 4) == 0 && name[4] - '0' == core) {
                idleDelta[core] = delta;
            }
        }
        if (i > 0) {
            appendStr(out, left, ",");
        }
        char state = t.eCurrentState == eRunning ? 'R'
            : t.eCurrentState == eReady ? 'r'
            : t.eCurrentState == eBlocked ? 'B'
            : t.eCurrentState == eSuspended ? 'S' : 'X';
        appendFmt(out, left, "{\"n\":\"%.14s\",\"c\":%d,\"p\":%u,\"s\":\"%c\",\"cpu\":%.1f,\"hwm\":%u}",
            name, (int)core, (unsigned)t.uxCurrentPriority, state,
            cpu, (unsigned)t.usStackHighWaterMark);
    }
    appendStr(out, left, "]");
    double allDelta = coreDelta[0] + coreDelta[1];
    double load0 = allDelta > 0 && idleDelta[0] > 0 ? 100.0 * (1.0 - idleDelta[0] / coreDelta[0]) : 0.0;
    double load1 = allDelta > 0 && idleDelta[1] > 0 ? 100.0 * (1.0 - idleDelta[1] / coreDelta[1]) : 0.0;
    if (load0 < 0) load0 = 0;
    if (load1 < 0) load1 = 0;
    if (load0 > 100) load0 = 100;
    if (load1 > 100) load1 = 100;
    appendFmt(out, left, ",\"load\":[%.1f,%.1f]", load0, load1);
    totalPrev = total;
}

void loadCrashLogs() {
    crashCount = 0;
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        char key[4] = {'c', static_cast<char>('0' + i), 0};
        size_t len = sizeof(crashSlots[i].text);
        if (nvs_get_str(h, key, crashSlots[i].text, &len) == ESP_OK && crashSlots[i].text[0] != '\0') {
            crashCount++;
        } else {
            crashSlots[i].text[0] = '\0';
        }
    }
    nvs_close(h);
}

void saveCrashLog(esp_reset_reason_t r, const char* tail) {
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t slot = rtcBuf.bootCount % 3;
    char key[4] = {'c', static_cast<char>('0' + slot), 0};
    char text[sizeof(CrashLog::text)];
    snprintf(text, sizeof(text), "[%s] %s", resetReasonName(r), tail);
    nvs_set_str(h, key, text);
    nvs_commit(h);
    nvs_close(h);
}

} // namespace

static void diagWatchCb(void* arg);
static esp_timer_handle_t sWatchTimer = nullptr;
static volatile uint32_t sLoopBeatMs = 0;
static void (*sStallCb)(uint32_t) = nullptr;

void diagBegin() {
    esp_reset_reason_t r = esp_reset_reason();
    // Flash and NVS queries disable the SPI cache; on a PSRAM-stack task
    // (async_tcp) that asserts. Snapshot them here on the main task.
    flashStats.sketch = ESP.getSketchSize();
    flashStats.free = ESP.getFreeSketchSpace();
    const esp_partition_t* part = esp_ota_get_running_partition();
    flashStats.part = part ? part->size : 0;
    flashStats.chip = ESP.getFlashChipSize();
    nvs_stats_t nvsStat;
    flashStats.nvsOk = nvs_get_stats(nullptr, &nvsStat) == ESP_OK;
    if (flashStats.nvsOk) {
        flashStats.nvsUsed = nvsStat.used_entries;
        flashStats.nvsFree = nvsStat.free_entries;
    }
    // The ring and crash log live in PSRAM: static internal DRAM of this size
    // starved an already tight heap (mDNS + HTTP died at free=440 B).
    if (ring == nullptr) {
        ring = static_cast<DiagEntry*>(heap_caps_calloc(kRing, sizeof(DiagEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (ring == nullptr) {
            ring = static_cast<DiagEntry*>(heap_caps_calloc(kRing, sizeof(DiagEntry), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
    if (crashSlots == nullptr) {
        crashSlots = static_cast<CrashLog*>(heap_caps_calloc(3, sizeof(CrashLog), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (crashSlots == nullptr) {
            crashSlots = static_cast<CrashLog*>(heap_caps_calloc(3, sizeof(CrashLog), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
    if (ring == nullptr || crashSlots == nullptr) {
        return;
    }
    bool valid = rtcBuf.magic == kRtcMagic && rtcBuf.count <= kRtcEntries;
    if (!valid) {
        memset(&rtcBuf, 0, sizeof(rtcBuf));
        rtcBuf.magic = kRtcMagic;
        rtcBuf.bootCount = 0;
    }
    rtcBuf.bootCount++;
    bootUptime0 = millis();
    loadCrashLogs();
    // Build the crash tail before the RTC buffer is reset for this boot.
    char crashTail[400] = "";
    if (isCrashReason(r)) {
        size_t pos = 0;
        for (uint16_t i = 0; i < rtcBuf.count && pos + 1 < sizeof(crashTail); i++) {
            uint16_t idx = static_cast<uint16_t>((rtcBuf.head + i) % kRtcEntries);
            pos += static_cast<size_t>(snprintf(crashTail + pos, sizeof(crashTail) - pos, "%s%.72s",
                i ? " | " : "", rtcBuf.entries[idx].msg));
        }
    }
    if (valid && rtcBuf.count > 0) {
        // Carry the previous boot's tail over, marked so the UI can group it.
        for (uint16_t i = 0; i < rtcBuf.count && ringCount < 40; i++) {
            uint16_t idx = static_cast<uint16_t>((rtcBuf.head + rtcBuf.count - 1 - i) % kRtcEntries);
            DiagEntry e = rtcBuf.entries[idx];
            char marked[112];
            snprintf(marked, sizeof(marked), "[prev-boot] %.96s", e.msg);
            ringPush(0, e.kind, marked);
        }
        ringPush(0, "boot", "previous-boot tail above");
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "boot #%u reset=%s", (unsigned)rtcBuf.bootCount, resetReasonName(r));
    ringPush(bootUptime0, "boot", msg);
    rtcBuf.head = 0;
    rtcBuf.count = 0;
    if (crashTail[0] != '\0') {
        saveCrashLog(r, crashTail);
    }
    // Mirror the boot event into RTC so a fast second crash still has context.
    rtcBuf.entries[rtcBuf.head] = ring[(ringHead + kRing - 1) % kRing];
    rtcBuf.head = static_cast<uint16_t>((rtcBuf.head + 1) % kRtcEntries);
    rtcBuf.count = 1;
    // Watchdog lives in the esp_timer task: independent of the loop it guards.
    esp_timer_create_args_t args = {};
    args.callback = diagWatchCb;
    args.name = "diag-watch";
    if (esp_timer_create(&args, &sWatchTimer) == ESP_OK) {
        esp_timer_start_periodic(sWatchTimer, 1000000);
    }
}

// Loop-freeze watchdog. The loop updates its beat every pass; this timer runs
// in the esp_timer task and fires while a pass is still stuck — a pass can
// spend minutes inside one blocked telnet write, so waiting for it to end is
// far too late.
void diagLoopBeat() {
    sLoopBeatMs = millis();
}

void diagOnLoopStall(void (*cb)(uint32_t stallMs)) {
    sStallCb = cb;
}

static void diagWatchCb(void*) {
    if (sStallCb == nullptr || sLoopBeatMs == 0) {
        return;
    }
    uint32_t beat = __atomic_load_n(&sLoopBeatMs, __ATOMIC_RELAXED);
    uint32_t now = millis();
    if (beat != 0 && now - beat > 3000) {
        // Re-arm so a still-stuck loop keeps triggering (and keeps telnet
        // quarantined) until it recovers.
        __atomic_store_n(&sLoopBeatMs, now, __ATOMIC_RELAXED);
        sStallCb(now - beat);
    }
}

void diagEventImpl(const char* kind, const char* fmt, va_list args, bool force) {
    char msg[112];
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    if (n <= 0) {
        return;
    }
    uint32_t now = millis();
    if (!force) {
        int slot = -1;
        for (uint8_t i = 0; i < kindCount; i++) {
            if (strcmp(kindNames[i], kind) == 0) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            if (now - kindLastMs[slot] < kKindGapMs) {
                return;
            }
            kindLastMs[slot] = now;
        } else if (kindCount < 8) {
            slot = kindCount++;
            strncpy(kindNames[slot], kind, sizeof(kindNames[slot]) - 1);
            kindLastMs[slot] = now;
        }
    }
    ringPush(now, kind, msg);
    // Mirror into RTC (drop the oldest RTC entry when full).
    DiagEntry& e = rtcBuf.entries[rtcBuf.head];
    e.ms = now;
    strncpy(e.kind, kind, sizeof(e.kind) - 1);
    e.kind[sizeof(e.kind) - 1] = '\0';
    strncpy(e.msg, msg, sizeof(e.msg) - 1);
    e.msg[sizeof(e.msg) - 1] = '\0';
    rtcBuf.head = static_cast<uint16_t>((rtcBuf.head + 1) % kRtcEntries);
    if (rtcBuf.count < kRtcEntries) {
        rtcBuf.count++;
    }
}

void diagEvent(const char* kind, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diagEventImpl(kind, fmt, args, false);
    va_end(args);
}

void diagEventForce(const char* kind, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    diagEventImpl(kind, fmt, args, true);
    va_end(args);
}

void diagNoteWsTx(size_t bytes) {
    if (bytes) {
        __atomic_add_fetch(&wsTxTotal, bytes, __ATOMIC_RELAXED);
    }
}

void diagNoteWsRetry() {
    __atomic_add_fetch(&wsRetries, 1, __ATOMIC_RELAXED);
}

void diagNoteMidiOut(size_t bytes) {
    __atomic_add_fetch(&midiOutMsgs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&midiOutBytes, bytes, __ATOMIC_RELAXED);
}

uint32_t diagWsTxBytes() {
    return __atomic_load_n(&wsTxTotal, __ATOMIC_RELAXED);
}

uint32_t diagWsRetries() {
    return __atomic_load_n(&wsRetries, __ATOMIC_RELAXED);
}

uint32_t diagMidiOutMsgs() {
    return __atomic_load_n(&midiOutMsgs, __ATOMIC_RELAXED);
}

static size_t buildStats(char* out, size_t cap);
// The stats payload cache: built in diagTick (loop context), served by the
// route via memcpy.
static char* statsCache = nullptr;
static volatile size_t statsCacheLen = 0;

void diagTick() {
    uint32_t now = millis();
    if (lastSampleMs != 0 && now - lastSampleMs < kSampleMs) {
        return;
    }
    bool first = lastSampleMs == 0;
    lastSampleMs = now;

    size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t intLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t intMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t psFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (!heapLowLogged && intLargest < 4500) {
        heapLowLogged = true;
        diagEventForce("heap", "low: largest=%u free=%u minEver=%u",
            (unsigned)intLargest, (unsigned)intFree, (unsigned)intMin);
    } else if (heapLowLogged && intLargest >= 5500) {
        heapLowLogged = false;
        diagEvent("heap", "recovered: largest=%u free=%u minEver=%u",
            (unsigned)intLargest, (unsigned)intFree, (unsigned)intMin);
    }

    static uint32_t prevDropB = 0, prevDropM = 0, prevOvfB = 0, prevOvfM = 0;
    static uint32_t prevFrmB = 0, prevFrmM = 0, prevParseDrop = 0;
    uint32_t dropB = showbox.droppedFromBase(), dropM = showbox.droppedFromMixer();
    uint32_t ovfB = showbox.overflowFromBase(), ovfM = showbox.overflowFromMixer();
    uint32_t frmB = showbox.frameErrFromBase(), frmM = showbox.frameErrFromMixer();
    uint32_t pd = showbox.parseDropped();
    if (!first) {
        uint32_t dDropB = dropB - prevDropB, dDropM = dropM - prevDropM;
        uint32_t dOvfB = ovfB - prevOvfB, dOvfM = ovfM - prevOvfM;
        uint32_t dFrmB = frmB - prevFrmB, dFrmM = frmM - prevFrmM;
        uint32_t dPd = pd - prevParseDrop;
        if (dDropB || dDropM || dOvfB || dOvfM || dFrmB || dFrmM || dPd) {
            diagEventForce("uart", "loss: dropB=%+u dropM=%+u ovfB=%+u ovfM=%+u frmB=%+u frmM=%+u parseQ=%+u",
                dDropB, dDropM, dOvfB, dOvfM, dFrmB, dFrmM, dPd);
        }
    }
    prevDropB = dropB; prevDropM = dropM;
    prevOvfB = ovfB; prevOvfM = ovfM;
    prevFrmB = frmB; prevFrmM = frmM;
    prevParseDrop = pd;

    bool link = showbox.linkOk(1500);
    linkDeadSamples = link ? 0 : linkDeadSamples + 1;
    if (linkDeadSamples >= 3 && !linkDeadLogged) {
        linkDeadLogged = true;
        diagEventForce("link", "dead for %u s (hbM=%u hbB=%u)",
            (unsigned)(linkDeadSamples * kSampleMs / 1000),
            (unsigned)showbox.heartbeatAgeMs(), (unsigned)showbox.baseAgeMs());
    } else if (link && linkDeadLogged) {
        linkDeadLogged = false;
        linkDeadSamples = 0;
        diagEventForce("link", "restored after %u s",
            (unsigned)(kSampleMs * 3 / 1000));
    }

    int ws = lanWsClients();
    if (ws != lastWsClients) {
        if (lastWsClients >= 0) {
            diagEvent("ws", "clients %d -> %d", lastWsClients, ws);
        }
        lastWsClients = ws;
    }

    int st = (int)WiFi.status();
    if (wifiRadioOn && st != lastWifiStatus) {
        if (lastWifiStatus >= 0) {
            diagEventForce("wifi", "status %s -> %s",
                lastWifiStatus == (int)WL_CONNECTED ? "up" : "down",
                st == (int)WL_CONNECTED ? "up" : "down");
        }
        lastWifiStatus = st;
    }
    if (st == (int)WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        if (!first && rssi <= -80 && lastRssi > -80) {
            diagEvent("wifi", "rssi %d dBm", rssi);
        }
        lastRssi = rssi;
    }

    // Rebuild the task-manager payload here in the loop, once per sample.
    if (statsCache == nullptr) {
        statsCache = static_cast<char*>(heap_caps_malloc(kStatsCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (statsCache == nullptr) {
            statsCache = static_cast<char*>(heap_caps_malloc(kStatsCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
    if (statsCache != nullptr) {
        size_t len = buildStats(statsCache, kStatsCap);
        __atomic_store_n(&statsCacheLen, len, __ATOMIC_RELAXED);
    }
}

void diagNoteLoopSlow(uint32_t durationMs) {
    diagEvent("loop", "stall %u ms", (unsigned)durationMs);
}

// The full stats payload is built ONCE per sample interval, here in the main
// loop. Building it from the HTTP task (a browser polling at 1 Hz) called
// uxTaskGetSystemState, which suspends the scheduler, and that deadlocked
// against the loop's lwip locks under load. The route now serves a memcpy of
// the cache declared above diagTick.

size_t diagStatsJson(char* out, size_t cap) {
    if (out == nullptr || cap < 256) {
        return 0;
    }
    size_t len = __atomic_load_n(&statsCacheLen, __ATOMIC_RELAXED);
    if (statsCache == nullptr || len == 0 || len >= cap) {
        len = statsCache == nullptr ? 0 : (cap - 1);
    }
    memcpy(out, statsCache, len);
    out[len] = '\0';
    return len;
}

static size_t buildStats(char* out, size_t cap) {
    if (out == nullptr || cap < 256) {
        return 0;
    }
    char* p = out;
    size_t left = cap;
    uint32_t now = millis();

    appendStr(p, left, "{\"boot\":{");
    appendFmt(p, left, "\"uptimeMs\":%u,\"bootCount\":%u,\"reset\":\"%s\",\"fw\":\"%.32s\",\"build\":\"%.24s\"}",
        (unsigned)now, (unsigned)rtcBuf.bootCount, resetReasonName(esp_reset_reason()),
        ubfw.version(), __DATE__ " " __TIME__);

    appendStr(p, left, ",\"mem\":{");
    appendFmt(p, left, "\"int\":{\"free\":%u,\"largest\":%u,\"min\":%u,\"size\":%u}",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    appendFmt(p, left, ",\"psram\":{\"free\":%u,\"largest\":%u,\"size\":%u}",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    appendFmt(p, left, ",\"ble\":{\"largest\":%u}}",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    appendStr(p, left, ",\"flash\":{");
    appendFmt(p, left, "\"sketch\":%u,\"part\":%u,\"free\":%u,\"chip\":%u",
        (unsigned)flashStats.sketch, (unsigned)flashStats.part,
        (unsigned)flashStats.free, (unsigned)flashStats.chip);
    if (flashStats.nvsOk) {
        appendFmt(p, left, ",\"nvs\":{\"used\":%u,\"free\":%u}",
            (unsigned)flashStats.nvsUsed, (unsigned)flashStats.nvsFree);
    }
    appendStr(p, left, "}");

    appendStr(p, left, ",\"net\":{");
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    appendFmt(p, left, "\"wifi\":{\"up\":%s,\"rssi\":%d,\"chan\":%d}",
        wifiUp ? "true" : "false", wifiUp ? WiFi.RSSI() : 0, WiFi.channel());
    uint32_t tx = diagWsTxBytes();
    static uint32_t prevWsTx = 0;
    uint32_t txDelta = tx >= prevWsTx ? tx - prevWsTx : 0;
    prevWsTx = tx;
    uint32_t winMs = now >= statsWindowMs && statsWindowMs ? now - statsWindowMs : kSampleMs;
    if (winMs == 0) {
        winMs = 1;
    }
    appendFmt(p, left, ",\"ws\":{\"clients\":%d,\"txBps\":%u,\"txTotal\":%u,\"retries\":%u}",
        lanWsClients(), (unsigned)(txDelta * 1000 / winMs), (unsigned)tx, (unsigned)diagWsRetries());
    appendFmt(p, left, ",\"gw\":{\"rttMs\":%u,\"misses\":%d}",
        (unsigned)lanGwRttMs(), (int)lanGwMisses());
    uint32_t midiMsgs = diagMidiOutMsgs();
    uint32_t midiBytesTotal = __atomic_load_n(&midiOutBytes, __ATOMIC_RELAXED);
    static uint32_t prevMidiMsgs = 0;
    static uint32_t prevMidiBytes = 0;
    uint32_t midiDelta = midiMsgs >= prevMidiMsgs ? midiMsgs - prevMidiMsgs : 0;
    uint32_t midiByteDelta = midiBytesTotal >= prevMidiBytes ? midiBytesTotal - prevMidiBytes : 0;
    prevMidiMsgs = midiMsgs;
    prevMidiBytes = midiBytesTotal;
    appendFmt(p, left, ",\"midi\":{\"outMsgs\":%u,\"outMsgsPerS\":%u,\"outBytesPerS\":%u,\"xtouch\":%s,\"bleHosts\":%d}",
        (unsigned)midiMsgs, (unsigned)(midiDelta * 1000 / winMs),
        (unsigned)(midiByteDelta * 1000 / winMs),
        UsbMidiHost::connected() ? "true" : "false", multiMidi.outgoingCount());
    appendStr(p, left, "}");

    appendStr(p, left, ",\"uart\":{");
    static uint32_t prevStatsMs = 0;
    static uint32_t statsUartPrevRxB = 0;
    static uint32_t statsUartPrevRxM = 0;
    uint32_t rxB = showbox.rxFromBase();
    uint32_t rxM = showbox.rxFromMixer();
    uint32_t dBps = 0, dMps = 0;
    uint32_t win = prevStatsMs && now > prevStatsMs ? now - prevStatsMs : 0;
    if (win > 100) {
        dBps = (rxB - statsUartPrevRxB) * 1000 / win;
        dMps = (rxM - statsUartPrevRxM) * 1000 / win;
    }
    statsUartPrevRxB = rxB;
    statsUartPrevRxM = rxM;
    prevStatsMs = now;
    appendFmt(p, left, "\"rxBps\":%u,\"rxMps\":%u,\"baud\":115200,\"dropB\":%u,\"dropM\":%u,\"ovfB\":%u,\"ovfM\":%u",
        (unsigned)dBps, (unsigned)dMps,
        (unsigned)showbox.droppedFromBase(), (unsigned)showbox.droppedFromMixer(),
        (unsigned)showbox.overflowFromBase(), (unsigned)showbox.overflowFromMixer());
    appendFmt(p, left, ",\"frmB\":%u,\"frmM\":%u,\"parseQDrop\":%u,\"analogSkip\":%u,\"cutB\":%u,\"cutM\":%u",
        (unsigned)showbox.frameErrFromBase(), (unsigned)showbox.frameErrFromMixer(),
        (unsigned)showbox.parseDropped(), (unsigned)showbox.analogSkipped(),
        (unsigned)showbox.cutThroughFromBase(), (unsigned)showbox.cutThroughFromMixer());
    appendFmt(p, left, ",\"pendB\":%u,\"pendM\":%u,\"fifo1\":%u,\"fifo2\":%u,\"link\":%s,\"hbM\":%u,\"hbB\":%u,\"dumpSeq\":%u}",
        (unsigned)showbox.pendingToMixer(), (unsigned)showbox.pendingToBase(),
        (unsigned)showbox.hwRxFifoBase(), (unsigned)showbox.hwRxFifoMixer(),
        showbox.linkOk(1500) ? "true" : "false",
        (unsigned)showbox.heartbeatAgeMs(), (unsigned)showbox.baseAgeMs(),
        (unsigned)showbox.dumpSeq());

    sampleTasks(p, left, now);
    statsWindowMs = now;

    appendStr(p, left, ",\"log\":[");
    uint16_t start = static_cast<uint16_t>((ringHead + kRing - ringCount) % kRing);
    bool firstRow = true;
    for (uint16_t i = 0; i < ringCount; i++) {
        DiagEntry& e = ring[(start + i) % kRing];
        if (!firstRow) {
            appendStr(p, left, ",");
        }
        firstRow = false;
        appendFmt(p, left, "{\"t\":%u,\"k\":\"%.10s\",\"m\":\"", (unsigned)e.ms, e.kind);
        for (const char* c = e.msg; *c && left > 8; c++) {
            char ch = *c;
            if (ch == '"' || ch == '\\') {
                *p++ = '\\';
                left--;
            } else if (ch < 32) {
                *p++ = ' ';
                left--;
            } else {
                *p++ = ch;
                left--;
            }
        }
        *p++ = '"';
        left--;
        *p++ = '}';
        left--;
    }
    appendStr(p, left, "]");

    if (crashCount > 0) {
        appendStr(p, left, ",\"crashLogs\":[");
        bool first = true;
        for (int i = 0; i < 3; i++) {
            if (crashSlots[i].text[0] == '\0') {
                continue;
            }
            if (!first) {
                appendStr(p, left, ",");
            }
            first = false;
            appendStr(p, left, "\"");
            for (const char* c = crashSlots[i].text; *c && left > 8; c++) {
                char ch = *c;
                if (ch == '"' || ch == '\\') {
                    *p++ = '\\';
                    left--;
                } else if (ch < 32) {
                    *p++ = ' ';
                    left--;
                } else {
                    *p++ = ch;
                    left--;
                }
            }
            appendStr(p, left, "\"");
        }
        appendStr(p, left, "]");
    }

    appendStr(p, left, "}");
    return cap - left;
}