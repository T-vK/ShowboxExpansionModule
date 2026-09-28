#include "BleHid.h"
#include "BleRadio/BleRadio.h"
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <HIDTypes.h>
#include <cstring>

namespace {
const uint8_t kReportMap[] = {
    USAGE_PAGE(1), 0x01,
    USAGE(1), 0x06,
    COLLECTION(1), 0x01,
        REPORT_ID(1), 0x01,
        USAGE_PAGE(1), 0x07,
        USAGE_MINIMUM(1), 0xE0,
        USAGE_MAXIMUM(1), 0xE7,
        LOGICAL_MINIMUM(1), 0x00,
        LOGICAL_MAXIMUM(1), 0x01,
        REPORT_SIZE(1), 0x01,
        REPORT_COUNT(1), 0x08,
        HIDINPUT(1), 0x02,
        REPORT_COUNT(1), 0x01,
        REPORT_SIZE(1), 0x08,
        HIDINPUT(1), 0x01,
        REPORT_COUNT(1), 0x06,
        REPORT_SIZE(1), 0x08,
        LOGICAL_MINIMUM(1), 0x00,
        LOGICAL_MAXIMUM(1), 0xFF,
        USAGE_MINIMUM(1), 0x00,
        USAGE_MAXIMUM(1), 0xFF,
        HIDINPUT(1), 0x00,
    END_COLLECTION(0),
    USAGE_PAGE(1), 0x01,
    USAGE(1), 0x02,
    COLLECTION(1), 0x01,
        REPORT_ID(1), 0x02,
        USAGE(1), 0x01,
        COLLECTION(1), 0x00,
            USAGE_PAGE(1), 0x09,
            USAGE_MINIMUM(1), 0x01,
            USAGE_MAXIMUM(1), 0x03,
            LOGICAL_MINIMUM(1), 0x00,
            LOGICAL_MAXIMUM(1), 0x01,
            REPORT_SIZE(1), 0x01,
            REPORT_COUNT(1), 0x03,
            HIDINPUT(1), 0x02,
            REPORT_SIZE(1), 0x05,
            REPORT_COUNT(1), 0x01,
            HIDINPUT(1), 0x01,
            USAGE_PAGE(1), 0x01,
            USAGE(1), 0x30,
            USAGE(1), 0x31,
            USAGE(1), 0x38,
            LOGICAL_MINIMUM(1), 0x81,
            LOGICAL_MAXIMUM(1), 0x7F,
            REPORT_SIZE(1), 0x08,
            REPORT_COUNT(1), 0x03,
            HIDINPUT(1), 0x06,
        END_COLLECTION(0),
    END_COLLECTION(0)
};

bool usageFor(char c, uint8_t& modifier, uint8_t& key) {
    modifier = 0;
    if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { modifier = 0x02; key = 0x04 + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); return true; }
    if (c == '0') { key = 0x27; return true; }
    if (c == ' ' || c == '\n' || c == '\r') { key = c == ' ' ? 0x2C : 0x28; return true; }
    if (c == '\t') { key = 0x2B; return true; }
    const struct { char symbol; uint8_t mod; uint8_t usage; } table[] = {
        {'-', 0, 0x2D}, {'=', 0, 0x2E}, {'[', 0, 0x2F}, {']', 0, 0x30},
        {'\\', 0, 0x31}, {';', 0, 0x33}, {'\'', 0, 0x34}, {'`', 0, 0x35},
        {',', 0, 0x36}, {'.', 0, 0x37}, {'/', 0, 0x38},
        {'!', 2, 0x1E}, {'@', 2, 0x1F}, {'#', 2, 0x20}, {'$', 2, 0x21},
        {'%', 2, 0x22}, {'^', 2, 0x23}, {'&', 2, 0x24}, {'*', 2, 0x25},
        {'(', 2, 0x26}, {')', 2, 0x27}, {'_', 2, 0x2D}, {'+', 2, 0x2E},
        {'{', 2, 0x2F}, {'}', 2, 0x30}, {'|', 2, 0x31}, {':', 2, 0x33},
        {'"', 2, 0x34}, {'~', 2, 0x35}, {'<', 2, 0x36}, {'>', 2, 0x37},
        {'?', 2, 0x38},
    };
    for (const auto& entry : table) {
        if (entry.symbol == c) {
            modifier = entry.mod;
            key = entry.usage;
            return true;
        }
    }
    return false;
}
}

void BleHid::begin(BLEServer* bleServer, Print* debugPrint) {
    server = bleServer;
    debug = debugPrint == nullptr ? &Serial : debugPrint;
}

bool BleHid::ensureStarted() {
    if (started) {
        return true;
    }
    if (server == nullptr) {
        return false;
    }
    if (!securityReady) {
        BLESecurity* security = new BLESecurity();
        security->setAuthenticationMode(ESP_LE_AUTH_BOND);
        security->setCapability(ESP_IO_CAP_NONE);
        security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        securityReady = true;
    }
    device = new BLEHIDDevice(server);
    device->manufacturer()->setValue("Showbox");
    device->pnp(0x02, 0xE502, 0xA111, 0x0100);
    device->hidInfo(0x00, 0x02);
    device->reportMap((uint8_t*)kReportMap, sizeof(kReportMap));
    keyboard = device->inputReport(1);
    mouse = device->inputReport(2);
    device->startServices();
    device->setBatteryLevel(100);
    started = true;
    debug->println("Bluetooth HID keyboard and mouse are ready.");
    return true;
}

void BleHid::setEnabled(bool enabled) {
    on = enabled;
    if (enabled) {
        ensureStarted();
    }
    BleRadio::instance().setHidPeripheral(enabled && started);
    BleRadio::instance().apply();
}

bool BleHid::enabled() const { return on; }

bool BleHid::ready() const {
    if (!on || !started) {
        return false;
    }
    return server != nullptr && server->getConnectedCount() > 0;
}

bool BleHid::push(uint8_t id, const uint8_t* data, uint8_t length) {
    if (length > 8) {
        return false;
    }
    portENTER_CRITICAL(&mux);
    if (queueCount >= kQueueSize) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    Report& report = queue[(queueHead + queueCount) % kQueueSize];
    report.id = id;
    report.length = length;
    memset(report.data, 0, sizeof(report.data));
    memcpy(report.data, data, length);
    queueCount++;
    portEXIT_CRITICAL(&mux);
    return true;
}

bool BleHid::queueText(const char* text) {
    if (!on || text == nullptr) {
        return false;
    }
    int queued = 0;
    for (const char* p = text; *p != '\0' && queued < 24; p++) {
        uint8_t modifier = 0;
        uint8_t key = 0;
        if (!usageFor(*p, modifier, key)) {
            continue;
        }
        uint8_t down[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
        uint8_t up[8] = {};
        if (!push(1, down, 8) || !push(1, up, 8)) {
            return queued > 0;
        }
        queued++;
    }
    return queued > 0;
}

bool BleHid::queueMouse(int8_t x, int8_t y, uint8_t buttons) {
    if (!on) {
        return false;
    }
    uint8_t report[4] = {buttons, (uint8_t)x, (uint8_t)y, 0};
    if (!push(2, report, 4)) {
        return false;
    }
    if (buttons != 0) {
        uint8_t release[4] = {0, 0, 0, 0};
        push(2, release, 4);
    }
    return true;
}

void BleHid::send(const Report& report) {
    BLECharacteristic* characteristic = report.id == 2 ? mouse : keyboard;
    if (characteristic == nullptr) {
        return;
    }
    characteristic->setValue((uint8_t*)report.data, report.length);
    characteristic->notify();
}

void BleHid::tick() {
    if (!on || !started || queueCount == 0) {
        return;
    }
    if (millis() - lastSentMs < 16) {
        return;
    }
    Report report;
    portENTER_CRITICAL(&mux);
    if (queueCount == 0) {
        portEXIT_CRITICAL(&mux);
        return;
    }
    report = queue[queueHead];
    queueHead = (queueHead + 1) % kQueueSize;
    queueCount--;
    portEXIT_CRITICAL(&mux);
    send(report);
    lastSentMs = millis();
}
