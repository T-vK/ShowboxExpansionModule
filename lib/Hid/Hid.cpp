#include "Hid.h"

#include "USB.h"
#include "USBHID.h"
#include <BLEDevice.h>
#include <BLECharacteristic.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <esp_heap_caps.h>
#include <HIDTypes.h>
#include <Preferences.h>
#include <cstring>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>

namespace {
constexpr uint8_t kReportIdKb = 1;
constexpr uint8_t kReportIdMouse = 2;
constexpr uint8_t kReportIdAbs = 3;
constexpr uint8_t kReportIdDig = 4;
constexpr uint8_t kReportIdConsumer = 5;
constexpr uint8_t kPartKb = 0x01;
constexpr uint8_t kPartMouse = 0x02;
constexpr uint8_t kPartAbs = 0x04;
constexpr uint8_t kPartDig = 0x08;
constexpr uint8_t kPartConsumer = 0x10;

void append(uint8_t*& dst, const uint8_t* src, size_t len) {
    memcpy(dst, src, len);
    dst += len;
}

const uint8_t kKeyboardMap[] = {
    USAGE_PAGE(1), 0x01,
    USAGE(1), 0x06,
    COLLECTION(1), 0x01,
        REPORT_ID(1), kReportIdKb,
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
        LOGICAL_MAXIMUM(2), 0xFF, 0x00,
        USAGE_MINIMUM(1), 0x00,
        USAGE_MAXIMUM(1), 0xFF,
        HIDINPUT(1), 0x00,
    END_COLLECTION(0)
};

const uint8_t kConsumerMap[] = {
    USAGE_PAGE(1), 0x0C,
    USAGE(1), 0x01,
    COLLECTION(1), 0x01,
        REPORT_ID(1), kReportIdConsumer,
        LOGICAL_MINIMUM(1), 0x00,
        LOGICAL_MAXIMUM(2), 0xFF, 0x03,
        USAGE_MINIMUM(1), 0x00,
        USAGE_MAXIMUM(2), 0xFF, 0x03,
        REPORT_SIZE(1), 0x10,
        REPORT_COUNT(1), 0x01,
        HIDINPUT(1), 0x00,
    END_COLLECTION(0)
};

const uint8_t kMouseMap[] = {
    USAGE_PAGE(1), 0x01,
    USAGE(1), 0x02,
    COLLECTION(1), 0x01,
        REPORT_ID(1), kReportIdMouse,
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
    END_COLLECTION(0)
};

// Single Generic Desktop pointer, 5 bytes: buttons + abs X/Y.
// The old dual-contact layout only produced EV_ABS when it was the only
// application collection. Beside a keyboard, Linux hid-generic left a
// consumer/padding node and dropped the axes (key yes, no-abs).
const uint8_t kAbsMap[] = {
    USAGE_PAGE(1), 0x01,
    USAGE(1), 0x01,
    COLLECTION(1), 0x01,
        REPORT_ID(1), kReportIdAbs,
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
            REPORT_COUNT(1), 0x01,
            REPORT_SIZE(1), 0x05,
            HIDINPUT(1), 0x01,
            USAGE_PAGE(1), 0x01,
            USAGE(1), 0x30,
            USAGE(1), 0x31,
            LOGICAL_MINIMUM(1), 0x00,
            LOGICAL_MAXIMUM(2), 0x10, 0x27,
            REPORT_SIZE(1), 0x10,
            REPORT_COUNT(1), 0x02,
            HIDINPUT(1), 0x02,
        END_COLLECTION(0),
    END_COLLECTION(0)
};

// BLE registers only Report IDs that are in the map. Extra unused
// characteristics make Android HOGP connect with no /dev/input. Pads in
// the map (vendor / System Control / extra Consumer / dummy mouse) either
// yield SYN-only (EV=1) or make Linux hid-generic create no evdev nodes.

const uint8_t kDigitizerMap[] = {
    USAGE_PAGE(1), 0x0D,
    USAGE(1), 0x04,
    COLLECTION(1), 0x01,
        REPORT_ID(1), kReportIdDig,
        USAGE(1), 0x22,
        COLLECTION(1), 0x02,
            USAGE(1), 0x42,
            USAGE(1), 0x32,
            LOGICAL_MINIMUM(1), 0x00,
            LOGICAL_MAXIMUM(1), 0x01,
            REPORT_SIZE(1), 0x01,
            REPORT_COUNT(1), 0x02,
            HIDINPUT(1), 0x02,
            REPORT_COUNT(1), 0x06,
            HIDINPUT(1), 0x01,
            USAGE(1), 0x51,
            LOGICAL_MAXIMUM(1), 0x7F,
            REPORT_SIZE(1), 0x08,
            REPORT_COUNT(1), 0x01,
            HIDINPUT(1), 0x02,
            USAGE_PAGE(1), 0x01,
            USAGE(1), 0x30,
            USAGE(1), 0x31,
            LOGICAL_MINIMUM(1), 0x00,
            LOGICAL_MAXIMUM(2), 0x10, 0x27,
            REPORT_SIZE(1), 0x10,
            REPORT_COUNT(1), 0x02,
            HIDINPUT(1), 0x02,
        END_COLLECTION(0),
        USAGE_PAGE(1), 0x0D,
        USAGE(1), 0x22,
        COLLECTION(1), 0x02,
            USAGE(1), 0x42,
            USAGE(1), 0x32,
            LOGICAL_MINIMUM(1), 0x00,
            LOGICAL_MAXIMUM(1), 0x01,
            REPORT_SIZE(1), 0x01,
            REPORT_COUNT(1), 0x02,
            HIDINPUT(1), 0x02,
            REPORT_COUNT(1), 0x06,
            HIDINPUT(1), 0x01,
            USAGE(1), 0x51,
            LOGICAL_MAXIMUM(1), 0x7F,
            REPORT_SIZE(1), 0x08,
            REPORT_COUNT(1), 0x01,
            HIDINPUT(1), 0x02,
            USAGE_PAGE(1), 0x01,
            USAGE(1), 0x30,
            USAGE(1), 0x31,
            LOGICAL_MINIMUM(1), 0x00,
            LOGICAL_MAXIMUM(2), 0x10, 0x27,
            REPORT_SIZE(1), 0x10,
            REPORT_COUNT(1), 0x02,
            HIDINPUT(1), 0x02,
        END_COLLECTION(0),
        USAGE_PAGE(1), 0x0D,
        USAGE(1), 0x54,
        LOGICAL_MINIMUM(1), 0x00,
        LOGICAL_MAXIMUM(1), 0x02,
        REPORT_SIZE(1), 0x08,
        REPORT_COUNT(1), 0x01,
        HIDINPUT(1), 0x02,
        USAGE(1), 0x55,
        LOGICAL_MAXIMUM(1), 0x02,
        FEATURE(1), 0x02,
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

class UsbCombinedDevice : public USBHIDDevice {
public:
    UsbCombinedDevice(const uint8_t* map, uint16_t mapLen) : hid(), descriptor(map), descriptorLen(mapLen) {
        hid.addDevice(this, mapLen);
    }

    void begin() {
        hid.begin();
    }

    bool ready() {
        return hid.ready();
    }

    bool send(uint8_t id, const void* data, size_t len) {
        return hid.SendReport(id, data, len);
    }

    uint16_t _onGetDescriptor(uint8_t* buffer) override {
        memcpy(buffer, descriptor, descriptorLen);
        return descriptorLen;
    }

private:
    USBHID hid;
    const uint8_t* descriptor;
    uint16_t descriptorLen;
};

uint16_t Hid::buildMap(uint8_t* out, Mix mix) {
    if (!mix.any()) {
        mix = Mix::desktop();
    }
    uint8_t* dst = out;
    if (mix.keyboard) {
        append(dst, kKeyboardMap, sizeof(kKeyboardMap));
        append(dst, kConsumerMap, sizeof(kConsumerMap));
    }
    if (mix.mouse) {
        append(dst, kMouseMap, sizeof(kMouseMap));
    }
    if (mix.absPointer) {
        append(dst, kAbsMap, sizeof(kAbsMap));
    }
    if (mix.digitizer) {
        append(dst, kDigitizerMap, sizeof(kDigitizerMap));
    }
    return static_cast<uint16_t>(dst - out);
}

size_t Hid::packReport(uint8_t* out, Mix mix, const CombinedReport& report) {
    size_t n = 0;
    auto put8 = [&](uint8_t value) {
        out[n++] = value;
    };
    auto put16 = [&](uint16_t value) {
        out[n++] = static_cast<uint8_t>(value);
        out[n++] = static_cast<uint8_t>(value >> 8);
    };
    auto putContacts = [&](uint8_t upFlags) {
        put8(report.contact0Flags & 0x01 ? 0x03 : upFlags);
        put8(report.contact0Id);
        put16(report.contact0X);
        put16(report.contact0Y);
        put8(report.contact1Flags & 0x01 ? 0x03 : upFlags);
        put8(report.contact1Id);
        put16(report.contact1X);
        put16(report.contact1Y);
        put8(report.contactCount);
    };
    if (mix.keyboard) {
        put8(report.kbMod);
        put8(report.kbReserved);
        for (uint8_t key : report.kbKeys) {
            put8(key);
        }
    }
    if (mix.mouse) {
        put8(report.mouseButtons);
        put8(static_cast<uint8_t>(report.mouseX));
        put8(static_cast<uint8_t>(report.mouseY));
        put8(static_cast<uint8_t>(report.mouseWheel));
    }
    if (mix.absPointer) {
        put8(report.contact0Flags & 0x01 ? 0x01 : 0x00);
        put16(report.contact0X);
        put16(report.contact0Y);
    }
    if (mix.digitizer) {
        // Keep In Range on lift so Android does not drop the contact.
        putContacts(0x02);
    }
    return n;
}

Hid::Mix Hid::loadUsbMix() {
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return Mix::desktop();
    }
    int bits = prefs.getInt("usbhidmix", 0);
    prefs.end();
    return Mix::fromStored(bits);
}

void Hid::setDebug(Print* serial) {
    debug = serial == nullptr ? &Serial : serial;
}

void Hid::setIdentity(const char* manufacturer, const char* product) {
    if (manufacturer != nullptr && manufacturer[0] != '\0') {
        manufacturerName = manufacturer;
    }
    if (product != nullptr && product[0] != '\0') {
        productName = product;
    }
}

void Hid::attachBle(BLEServer* server) {
    bleServer = server;
}

Hid::Mix Hid::currentMix() const {
    if (usbOn) {
        return usbActive;
    }
    Mix live = unionConnMix();
    return live.any() ? live : bleActive;
}

Hid::Mix Hid::unionConnMix() const {
    Mix live{};
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < connMixCount; i++) {
        live.keyboard = live.keyboard || connMix[i].mix.keyboard;
        live.mouse = live.mouse || connMix[i].mix.mouse;
        live.absPointer = live.absPointer || connMix[i].mix.absPointer;
        live.digitizer = live.digitizer || connMix[i].mix.digitizer;
    }
    portEXIT_CRITICAL(&mux);
    return live;
}

void Hid::setConnMix(uint16_t connId, Mix mix) {
    portENTER_CRITICAL(&mux);
    int found = -1;
    for (int i = 0; i < connMixCount; i++) {
        if (connMix[i].connId == connId) {
            found = i;
            break;
        }
    }
    if (found < 0 && connMixCount < kMaxBleConn) {
        found = connMixCount;
        connMix[found].connId = connId;
        connMixCount++;
    }
    if (found >= 0) {
        connMix[found].mix = mix;
    }
    portEXIT_CRITICAL(&mux);
}

void Hid::clearConnMix(uint16_t connId) {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < connMixCount; i++) {
        if (connMix[i].connId == connId) {
            for (int j = i; j < connMixCount - 1; j++) {
                connMix[j] = connMix[j + 1];
            }
            connMixCount--;
            break;
        }
    }
    portEXIT_CRITICAL(&mux);
}

void Hid::rebuildBleMap() {
    Mix mapMix = bleActive.asBleMap();
    if (!mapMix.any()) {
        mapMix = Mix{true, true, false, false};
    }
    uint8_t* dst = bleMap;
    if (mapMix.keyboard) {
        append(dst, kKeyboardMap, sizeof(kKeyboardMap));
        append(dst, kConsumerMap, sizeof(kConsumerMap));
    }
    if (mapMix.mouse) {
        append(dst, kMouseMap, sizeof(kMouseMap));
    }
    if (mapMix.absPointer) {
        append(dst, kAbsMap, sizeof(kAbsMap));
    }
    bleMapLen = static_cast<uint16_t>(dst - bleMap);
    if (bleMapLen < sizeof(bleMap)) {
        memset(bleMap + bleMapLen, 0, sizeof(bleMap) - bleMapLen);
    }
    if (bleDevice != nullptr) {
        bleDevice->reportMap(bleMap, bleMapLen);
    }
}

void Hid::setBleMix(Mix mix) {
    Mix before = bleActive.asBleMap();
    bleActive = mix;
    if (!mix.any()) {
        return;
    }
    if (!before.same(mix.asBleMap())) {
        rebuildBleMap();
        Mix mapMix = mix.asBleMap();
        if (bleDevice != nullptr) {
            if (mapMix.keyboard && bleInputKb == nullptr) {
                bleInputKb = bleDevice->inputReport(kReportIdKb);
            }
            if (mapMix.mouse && bleInputMouse == nullptr) {
                bleInputMouse = bleDevice->inputReport(kReportIdMouse);
            }
            if (mapMix.absPointer && bleInputAbs == nullptr) {
                bleInputAbs = bleDevice->inputReport(kReportIdAbs);
            }
            if (mapMix.keyboard && bleInputConsumer == nullptr) {
                bleInputConsumer = bleDevice->inputReport(kReportIdConsumer);
            }
        }
    }
}

bool Hid::ensureBleStarted() {
    if (bleStarted) {
        return true;
    }
    if (bleServer == nullptr) {
        return false;
    }
    if (!bleSecurityReady) {
        BLESecurity* security = new BLESecurity();
        // Match BleRadio: DisplayYesNo so BlueZ KeyboardDisplay can finish SC
        // numeric comparison. IO_CAP_NONE + KeyboardDisplay fails SMP reason 3.
        security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
        security->setForceAuthentication(false);
        security->setCapability(ESP_IO_CAP_IO);
        security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        bleSecurityReady = true;
    }
    if (debug != nullptr) {
        debug->printf("[boot] HID: internal heap free=%u largest=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    bleDevice = new BLEHIDDevice(bleServer);
    bleDevice->manufacturer()->setValue(manufacturerName);
    bleDevice->pnp(0x02, 0xE502, 0xA113, 0x0103);
    bleDevice->hidInfo(0x00, 0x02);
    rebuildBleMap();
    // Always register Report IDs 1/2/3/5. BlueZ CCC writes fail if a
    // characteristic is missing; unused IDs stay out of the map so Linux
    // hid-generic still creates Keyboard/Mouse/Abs nodes.
    bleInputKb = bleDevice->inputReport(kReportIdKb);
    bleInputMouse = bleDevice->inputReport(kReportIdMouse);
    bleInputAbs = bleDevice->inputReport(kReportIdAbs);
    bleInputDig = nullptr;
    bleInputConsumer = bleDevice->inputReport(kReportIdConsumer);
    bleDevice->startServices();
    bleBattery = 255;
    bleDevice->setBatteryLevel(100);
    bleStarted = true;
    debug->println("Bluetooth HID keyboard, mouse, and absolute pointer are ready.");
    return true;
}

void Hid::setBatteryPercent(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    if (!bleStarted || bleDevice == nullptr || percent == bleBattery) {
        return;
    }
    bleBattery = percent;
    bleDevice->setBatteryLevel(percent);
}

void Hid::setBleEnabled(bool enabled) {
    bleOn = enabled;
    if (enabled) {
        ensureBleStarted();
        if (!bleStarted) {
            bleOn = false;
        }
    }
}

bool Hid::bleReady() const {
    if (!bleOn || !bleStarted) {
        return false;
    }
    return bleServer != nullptr && bleServer->getConnectedCount() > 0;
}

bool Hid::startUsb() {
    if (usbOn) {
        return true;
    }
    usbActive = loadUsbMix();
    usbMapLen = buildMap(usbMap, usbActive);
    usbDevice = new UsbCombinedDevice(usbMap, usbMapLen);
    USB.manufacturerName("");
    USB.productName(productName);
    USB.VID(0x303A);
    USB.PID(0x0002);
    if (!USB.begin()) {
        return false;
    }
    usbDevice->begin();
    usbOn = true;
    return true;
}

bool Hid::usbReady() const {
    return usbOn && usbDevice != nullptr && usbDevice->ready();
}

Hid::CombinedReport Hid::blank() const {
    CombinedReport report = {};
    return report;
}

uint16_t Hid::clampAxis(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > kAxisMax) {
        return kAxisMax;
    }
    return static_cast<uint16_t>(value);
}

bool Hid::pushBle(const CombinedReport& report) {
    portENTER_CRITICAL(&mux);
    if (queueCount >= kQueueSize) {
        portEXIT_CRITICAL(&mux);
        return false;
    }
    int slot = (queueHead + queueCount) % kQueueSize;
    queue[slot] = report;
    queueMix[slot] = bleActive;
    queueCount++;
    portEXIT_CRITICAL(&mux);
    return true;
}

bool Hid::notifyInput(BLECharacteristic* ch, uint16_t connId, const uint8_t* data, size_t len) {
    (void)connId;
    if (ch == nullptr || data == nullptr || len == 0) {
        return false;
    }
    if (bleServer != nullptr && bleServer->getConnectedCount() == 0) {
        return false;
    }
    ch->setValue(const_cast<uint8_t*>(data), len);
    // Notify from the NimBLE host task. ch->notify() runs ble_gatts_notify_custom
    // on the Arduino loop core and has rebooted this chip.
    uint16_t handle = ch->getHandle();
    if (handle == 0 || handle == 0xFFFF) {
        return false;
    }
    ble_gatts_chr_updated(handle);
    return true;
}

bool Hid::notifyParts(const CombinedReport& report, Mix mix, uint16_t connId) {
    uint8_t parts = report.parts;
    if (parts == 0 || !mix.any()) {
        return false;
    }
    uint8_t packed[32];
    bool any = false;
    auto sendBle = [&](uint8_t part, bool enabled, BLECharacteristic* ch, Mix one) {
        if ((parts & part) == 0 || !enabled) {
            return;
        }
        size_t n = packReport(packed, one, report);
        if (n == 0) {
            return;
        }
        any = notifyInput(ch, connId, packed, n) || any;
    };
    auto sendUsb = [&](uint8_t part, bool enabled, uint8_t reportId, Mix one) {
        if ((parts & part) == 0 || !enabled || usbDevice == nullptr) {
            return;
        }
        size_t n = packReport(packed, one, report);
        if (n == 0) {
            return;
        }
        any = usbDevice->send(reportId, packed, n) || any;
    };
    Mix kb{};
    kb.keyboard = true;
    Mix mouse{};
    mouse.mouse = true;
    Mix abs{};
    abs.absPointer = true;
    Mix dig{};
    dig.digitizer = true;
    auto sendConsumer = [&](uint16_t conn) {
        if ((parts & kPartConsumer) == 0 || !mix.keyboard) {
            return;
        }
        uint8_t payload[2] = {
            static_cast<uint8_t>(report.consumer),
            static_cast<uint8_t>(report.consumer >> 8)
        };
        if (conn == 0xFFFE) {
            if (usbDevice != nullptr) {
                any = usbDevice->send(kReportIdConsumer, payload, 2) || any;
            }
            return;
        }
        any = notifyInput(bleInputConsumer, conn, payload, 2) || any;
    };
    if (connId == 0xFFFE) {
        sendUsb(kPartKb, mix.keyboard, kReportIdKb, kb);
        sendUsb(kPartMouse, mix.mouse, kReportIdMouse, mouse);
        sendUsb(kPartAbs, mix.absPointer, kReportIdAbs, abs);
        sendUsb(kPartDig, mix.digitizer, kReportIdDig, dig);
        sendConsumer(connId);
        return any;
    }
    sendBle(kPartKb, mix.keyboard, bleInputKb, kb);
    sendBle(kPartMouse, mix.mouse, bleInputMouse, mouse);
    sendConsumer(connId);
    bool wantAbs = mix.absPointer || mix.digitizer;
    uint8_t absParts = report.parts;
    if (wantAbs && (absParts & (kPartAbs | kPartDig)) != 0) {
        CombinedReport absReport = report;
        absReport.parts = kPartAbs;
        size_t n = packReport(packed, abs, absReport);
        if (n > 0) {
            any = notifyInput(bleInputAbs, connId, packed, n) || any;
        }
    }
    return any;
}

bool Hid::dispatch(const CombinedReport& report, Mix mix) {
    if (report.parts == 0) {
        return false;
    }
    bool any = false;
    if (usbOn && usbDevice != nullptr) {
        any = notifyParts(report, mix, 0xFFFE) || any;
    }
    if (!bleOn) {
        return any;
    }
    ConnMix local[kMaxBleConn];
    int n = 0;
    portENTER_CRITICAL(&mux);
    n = connMixCount;
    for (int i = 0; i < n; i++) {
        local[i] = connMix[i];
    }
    portEXIT_CRITICAL(&mux);
    if (n == 0) {
        return notifyParts(report, mix, 0xFFFF) || any;
    }
    for (int i = 0; i < n; i++) {
        any = notifyParts(report, local[i].mix, local[i].connId) || any;
    }
    return any;
}

bool Hid::emit(const CombinedReport& report) {
    Mix mix = currentMix();
    if (!mix.any()) {
        return false;
    }
    if (usbOn && usbDevice != nullptr) {
        return dispatch(report, usbActive);
    }
    if (!bleOn) {
        return false;
    }
    return pushBle(report);
}

bool Hid::pushTouchFrame(uint16_t x0, uint16_t y0, bool down0, uint16_t x1, uint16_t y1, bool down1, uint8_t count) {
    Mix mix = currentMix();
    if (!mix.absPointer && !mix.digitizer) {
        return false;
    }
    CombinedReport report = blank();
    if (count >= 1) {
        report.contact0Flags = down0 ? 0x03 : 0x00;
        report.contact0Id = 1;
        report.contact0X = x0;
        report.contact0Y = y0;
    }
    if (count >= 2) {
        report.contact1Flags = down1 ? 0x03 : 0x00;
        report.contact1Id = 2;
        report.contact1X = x1;
        report.contact1Y = y1;
    }
    report.contactCount = count;
    if (mix.absPointer) {
        report.parts |= kPartAbs;
    }
    if (mix.digitizer) {
        report.parts |= kPartDig;
    }
    return emit(report);
}

bool Hid::sendText(const char* text) {
    if (text == nullptr || !currentMix().keyboard) {
        return false;
    }
    int queued = 0;
    for (const char* p = text; *p != '\0' && queued < 24; p++) {
        uint8_t modifier = 0;
        uint8_t key = 0;
        if (!usageFor(*p, modifier, key)) {
            continue;
        }
        CombinedReport down = blank();
        down.parts = kPartKb;
        down.kbMod = modifier;
        down.kbKeys[0] = key;
        CombinedReport up = blank();
        up.parts = kPartKb;
        if (!emit(down) || !emit(up)) {
            return queued > 0;
        }
        queued++;
    }
    return queued > 0;
}

bool Hid::sendUsage(uint8_t key) {
    return sendKey(key, 0, true, true);
}

bool Hid::sendKey(uint8_t usage, uint8_t modifiers, bool down, bool autoUp) {
    if (usage == 0 || !currentMix().keyboard) {
        return false;
    }
    CombinedReport report = blank();
    report.parts = kPartKb;
    if (down) {
        report.kbMod = modifiers;
        report.kbKeys[0] = usage;
    }
    if (!emit(report)) {
        return false;
    }
    if (down && autoUp) {
        portENTER_CRITICAL(&mux);
        kbReleasePending = true;
        kbReleaseAtMs = millis() + 50;
        portEXIT_CRITICAL(&mux);
    }
    return true;
}

bool Hid::sendMouse(int8_t x, int8_t y, uint8_t buttons, int8_t wheel, int8_t pan, bool autoRelease) {
    if (!currentMix().mouse) {
        return false;
    }
    CombinedReport move = blank();
    move.parts = kPartMouse;
    move.mouseButtons = buttons;
    move.mouseX = x;
    move.mouseY = y;
    move.mouseWheel = wheel;
    move.mousePan = pan;
    if (!emit(move)) {
        return false;
    }
    if (autoRelease && buttons != 0) {
        CombinedReport up = blank();
        up.parts = kPartMouse;
        emit(up);
    }
    return true;
}

bool Hid::sendConsumer(uint16_t usage, bool down, bool autoUp) {
    if (!currentMix().keyboard) {
        return false;
    }
    CombinedReport report = blank();
    report.parts = kPartConsumer;
    report.consumer = down ? usage : 0;
    if (!emit(report)) {
        return false;
    }
    if (down && autoUp) {
        CombinedReport up = blank();
        up.parts = kPartConsumer;
        emit(up);
    }
    return true;
}

bool Hid::sendAbs(uint16_t x, uint16_t y, bool down, uint8_t buttons) {
    (void)buttons;
    x = clampAxis(x);
    y = clampAxis(y);
    if (!pushTouchFrame(x, y, down, 0, 0, false, down ? 1 : 0)) {
        return false;
    }
    if (!down) {
        return pushTouchFrame(x, y, false, 0, 0, false, 0);
    }
    return true;
}

bool Hid::sendTap(uint16_t x, uint16_t y) {
    x = clampAxis(x);
    y = clampAxis(y);
    if (!pushTouchFrame(x, y, true, 0, 0, false, 1)) {
        return false;
    }
    if (!pushTouchFrame(x, y, false, 0, 0, false, 1)) {
        return false;
    }
    return pushTouchFrame(x, y, false, 0, 0, false, 0);
}

bool Hid::sendSwipe(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    x1 = clampAxis(x1);
    y1 = clampAxis(y1);
    x2 = clampAxis(x2);
    y2 = clampAxis(y2);
    const int steps = 8;
    if (!pushTouchFrame(x1, y1, true, 0, 0, false, 1)) {
        return false;
    }
    for (int i = 1; i <= steps; i++) {
        uint16_t x = static_cast<uint16_t>(x1 + (static_cast<int>(x2) - static_cast<int>(x1)) * i / steps);
        uint16_t y = static_cast<uint16_t>(y1 + (static_cast<int>(y2) - static_cast<int>(y1)) * i / steps);
        if (!pushTouchFrame(x, y, true, 0, 0, false, 1)) {
            return false;
        }
    }
    if (!pushTouchFrame(x2, y2, false, 0, 0, false, 1)) {
        return false;
    }
    return pushTouchFrame(x2, y2, false, 0, 0, false, 0);
}

bool Hid::sendPinch(uint16_t cx, uint16_t cy, bool inward) {
    cx = clampAxis(cx);
    cy = clampAxis(cy);
    const int steps = 8;
    int start = inward ? 2400 : 700;
    int end = inward ? 700 : 2400;
    for (int i = 0; i <= steps; i++) {
        int dist = start + (end - start) * i / steps;
        int x0 = static_cast<int>(cx) - dist / 2;
        int x1 = static_cast<int>(cx) + dist / 2;
        if (!pushTouchFrame(clampAxis(x0), cy, true, clampAxis(x1), cy, true, 2)) {
            return false;
        }
    }
    if (!pushTouchFrame(cx, cy, false, cx, cy, false, 2)) {
        return false;
    }
    return pushTouchFrame(cx, cy, false, cx, cy, false, 0);
}

void Hid::tick() {
    if (!bleOn || !bleStarted) {
        return;
    }
    uint32_t nowMs = millis();
    bool sendRelease = false;
    portENTER_CRITICAL(&mux);
    if (kbReleasePending && (int32_t)(nowMs - kbReleaseAtMs) >= 0) {
        kbReleasePending = false;
        sendRelease = true;
    }
    portEXIT_CRITICAL(&mux);
    if (sendRelease) {
        CombinedReport up = blank();
        up.parts = kPartKb;
        pushBle(up);
    }
    for (int flushed = 0; flushed < 8; flushed++) {
        CombinedReport report = {};
        Mix mix = bleActive;
        portENTER_CRITICAL(&mux);
        if (queueCount == 0) {
            portEXIT_CRITICAL(&mux);
            return;
        }
        report = queue[queueHead];
        mix = queueMix[queueHead];
        queueHead = (queueHead + 1) % kQueueSize;
        queueCount--;
        portEXIT_CRITICAL(&mux);
        if (dispatch(report, mix)) {
            lastSentMs = millis();
        }
    }
}
