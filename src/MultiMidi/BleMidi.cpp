#include "BleMidi.h"
#include "MultiMidi/MidiInbound.h"
#include <BleRadio.h>
#include <BLEDevice.h>
#include <BLEUUID.h>
#include <BLEAddress.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include "MidiCommon.h"
#include "DebugLog.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_coexist.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdlib>

BleMidi* BleMidi::instance = nullptr;

static void copyField(char* dest, size_t destSize, const uint8_t* text, size_t textLen) {
    if (destSize == 0) {
        return;
    }
    size_t n = textLen < destSize - 1 ? textLen : destSize - 1;
    memcpy(dest, text, n);
    dest[n] = '\0';
}

static void parseAdNames(const uint8_t* payload, size_t len, char* complete, size_t completeSize, char* shortName, size_t shortSize) {
    if (payload == nullptr) {
        return;
    }
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t fieldLen = payload[i];
        if (fieldLen == 0 || i + 1 + fieldLen > len) {
            break;
        }
        uint8_t type = payload[i + 1];
        if ((type == 0x09 || type == 0x08) && fieldLen > 1) {
            char* dest = type == 0x09 ? complete : shortName;
            size_t destSize = type == 0x09 ? completeSize : shortSize;
            if (type == 0x09 || dest[0] == '\0') {
                copyField(dest, destSize, payload + i + 2, fieldLen - 1);
            }
        }
        i += static_cast<size_t>(fieldLen) + 1;
    }
}

void parseAdServices(const uint8_t* payload, size_t len, bool& midi, bool& hid, bool& battery) {
    if (payload == nullptr) {
        return;
    }
    static const uint8_t kMidiLe[16] = {
        0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
        0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
    };
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t fieldLen = payload[i];
        if (fieldLen == 0 || i + 1 + fieldLen > len) {
            break;
        }
        uint8_t type = payload[i + 1];
        const uint8_t* data = payload + i + 2;
        size_t dataLen = fieldLen > 0 ? static_cast<size_t>(fieldLen - 1) : 0;
        if (type == 0x02 || type == 0x03) {
            for (size_t o = 0; o + 1 < dataLen; o += 2) {
                uint16_t uuid = static_cast<uint16_t>(data[o] | (data[o + 1] << 8));
                if (uuid == 0x1812) {
                    hid = true;
                } else if (uuid == 0x180F) {
                    battery = true;
                }
            }
        } else if (type == 0x06 || type == 0x07) {
            for (size_t o = 0; o + 15 < dataLen; o += 16) {
                if (memcmp(data + o, kMidiLe, 16) == 0) {
                    midi = true;
                }
            }
        }
        i += static_cast<size_t>(fieldLen) + 1;
    }
}

namespace {
constexpr uint32_t kScanSeconds = 4;
constexpr unsigned long kRescanDelayMs = 1000;

void readAdvertisedNames(BLEAdvertisedDevice& device, char* complete, size_t completeSize, char* shortName, size_t shortSize) {
    complete[0] = '\0';
    shortName[0] = '\0';
    parseAdNames(device.getPayload(), device.getPayloadLength(), complete, completeSize, shortName, shortSize);
    if (complete[0] == '\0' && device.haveName()) {
        String libraryName = device.getName();
        copyField(complete, completeSize, reinterpret_cast<const uint8_t*>(libraryName.c_str()), libraryName.length());
    }
}

// The controller often sends the advertising packet first and the scan response
// (which carries the name) second. Duplicate filtering drops that second packet,
// so the name never gets recorded. This keeps every packet until the name arrives.
class BleScanCollector : public BLEAdvertisedDeviceCallbacks {
public:
    bool found = false;
    BLEAdvertisedDevice device;

    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        BleMidi* self = BleMidi::active();
        if (self != nullptr) {
            self->noteDevice(advertisedDevice);
        }
        if (found || self == nullptr) {
            return;
        }
        if (!self->wantsDevice(advertisedDevice)) {
            return;
        }
        device = advertisedDevice;
        found = true;
        if (!self->isScanningFull()) {
            advertisedDevice.getScan()->stop();
        }
    }
};
}

BleMidi* BleMidi::active() {
    return instance;
}

void BleMidi::begin(midi::MidiAction* action, Print* debug) {
    Debug = debug;
    instance = this;
    decoder.setAction(action);
    if (!BLEDevice::getInitialized()) {
        BLEDevice::init("ShowboxMod");
    }
    BLEDevice::setCustomGapHandler(BleMidi::onGap);
    Debug->println("Bluetooth MIDI: connect out to peripherals, and advertise on the shared radio.");
    // NimBLE host and the BT controller already run on core 0. Pin this
    // task there too: connect()/GAP calls from core 1 take the same
    // interrupt lock as the UART ISR and the mixer goes silent.
    if (xTaskCreatePinnedToCoreWithCaps(BleMidi::taskEntry, "ble-midi", 12288, this, 1, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreatePinnedToCore(BleMidi::taskEntry, "ble-midi", 12288, this, 1, nullptr, 0);
    }
}

namespace {
// Returns true when a scan actually ran. A scan that the radio rejects returns immediately.
bool runScan(BleScanCollector& collector, bool wide) {
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&collector, true);
    BleMidi* self = BleMidi::active();
    bool radioBusy = self != nullptr && self->connectedCount() > 0;
    // Passive + a 10 ms window when a pedal is already linked. Active
    // 15 ms windows with a live link took TCP and the mixer UART down.
    scan->setActiveScan(!radioBusy);
    (void)wide;
    scan->setInterval(160);
    scan->setWindow(16);
    uint32_t seconds = radioBusy ? 2 : kScanSeconds;
    unsigned long started = millis();
    if (!scan->start(seconds, false)) {
        return false;
    }
    while (scan->isScanning() && millis() - started < (seconds * 1000 + 500)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return collector.found || millis() - started > 1000;
}
}

void BleMidi::tick() {}

void BleMidi::taskEntry(void* arg) {
    static_cast<BleMidi*>(arg)->scanLoop();
}

void BleMidi::scanLoop() {
    for (;;) {
        if (radio != nullptr) {
            radio->service();
        }
        // The looper pedal is Bluetooth. Advertising stays quiet while Wi-Fi
        // is down so the access point can rejoin. Auto-scan and remembered
        // pedal connects also wait 20s: they share 2.4 GHz with STA associate.
        bool wifiUp = WiFi.status() == WL_CONNECTED;
        static uint32_t wifiDownSince = 0;
        if (wifiUp) {
            wifiDownSince = 0;
        } else if (wifiDownSince == 0) {
            wifiDownSince = millis();
        }
        bool holdForWifi = !wifiUp && (int32_t)(millis() - wifiDownSince) < 20000;
        bool holdAdv = connectedCount() > 0 || (radio != nullptr && radio->hasIncoming());
        if (!wifiUp && !holdAdv) {
            BLEAdvertising* advertising = BLEDevice::getAdvertising();
            if (advertising != nullptr) {
                advertising->stop();
            }
            if (radio != nullptr) {
                radio->restartWhenIdle();
            }
        }
        bool hostLive = radio != nullptr && radio->hasIncoming();
        bool room = hasRoomForOutgoing();
        int outCount = connectedCount();
        bool recentFail = lastOutgoingFailMs != 0 && (int32_t)(millis() - lastOutgoingFailMs) < 4000;
        bool bootQuiet = !outgoingAllowed || (int32_t)(millis() - outgoingReadyMs) < 500;
        if (bootQuiet) {
            keepAdvertising();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        size_t heapBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        static uint32_t lastHeapLog = 0;
        if (lastHeapLog == 0 || (int32_t)(millis() - lastHeapLog) >= 5000) {
            lastHeapLog = millis();
            debugLog("ble largest=%u out=%d scan=%d auto=%d",
                (unsigned)heapBlock, connectedCount(), scanning ? 1 : 0, autoConnectOn ? 1 : 0);
        }
        bool heapTight = heapBlock < 2048;
        if (directConnect && connectOnce && target[0] != '\0') {
            directConnect = false;
            connectOnce = false;
            keepAdvertising();
            if (room) {
                lastQuietConnectMs = millis();
                Debug->printf("Connecting to '%s'\n", target);
                if (!connectAddressNow(target, nullptr, 3000, typeForAddress(target))) {
                    lastOutgoingFailMs = millis();
                } else {
                    lastOutgoingFailMs = 0;
                }
                if (radio != nullptr) {
                    radio->refreshLinkCount();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        // Remembered pedals must connect by address. A discovery scan used to
        // win this path, so AIRSTEP never joined unless it was advertising a name.
        if (centralEnabled && autoConnectOn && room && !scanning && !scanRequested && !holdForWifi && heapBlock >= 2048) {
            uint32_t waitMs = recentFail ? 4000 : (hostLive ? 8000 : 2500);
            if (lastQuietConnectMs == 0 || (int32_t)(millis() - lastQuietConnectMs) >= (int32_t)waitMs) {
                char pickAddress[18] = {};
                char pickName[32] = {};
                bool have = policy != nullptr
                    && policy->nextAutoOutgoing(pickAddress, sizeof(pickAddress), pickName, sizeof(pickName));
                if (have && hasRoomForOutgoing()) {
                    lastQuietConnectMs = millis();
                    keepAdvertising();
                    int pickType = typeForAddress(pickAddress);
                    debugLog("ble auto %s heap=%u type=%d", pickName[0] != '\0' ? pickName : pickAddress, (unsigned)heapBlock, pickType);
                    Debug->printf("Auto-connecting to '%s'\n", pickName[0] != '\0' ? pickName : pickAddress);
                    if (!connectAddressNow(pickAddress, pickName, 3000, pickType)) {
                        lastOutgoingFailMs = millis();
                    } else {
                        lastOutgoingFailMs = 0;
                    }
                    if (radio != nullptr) {
                        radio->refreshLinkCount();
                    }
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
            }
        }
        bool autoScan = !holdForWifi && !heapTight && centralEnabled && room && autoConnectOn && !hostLive && !recentFail &&
            millis() > 8000 && (policy == nullptr || policy->wantsAutoScan());
        bool shouldScan = (scanRequested || autoScan) && room;
        if (hostLive) {
            keepAdvertising();
        }
        if (!shouldScan) {
            keepAdvertising();
            if (outCount > 0 && (lastLinkMs == 0 || (int32_t)(millis() - lastLinkMs) >= 5000)) {
                lastLinkMs = millis();
                refreshLink();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool oneShot = connectOnce;
        // Do not take extra radio time just because Wi-Fi is up. The old
        // "full" scan was a 99% window and dropped the mixer UART.
        scanFull = false;
        scanRequested = false;
        clearSeen();
        scanning = true;
        Debug->println("Scanning for Bluetooth MIDI devices");
        BleScanCollector collector;
        bool scanned = runScan(collector, scanFull);
        if (!collector.found && !scanned) {
            // Do not stop ShowboxMod advertising to make a scan fit. That
            // drops phone HID and the mixer UART for a pedal that can wait.
            Debug->println("Skipping a scan that needs the advertiser off");
        }
        pruneUnseen();
        scanning = false;
        scanFull = false;
        if (wifiUp) {
            advertiseIfEnabled();
        }

        if (centralEnabled) {
            char pickAddress[18] = {};
            char pickName[32] = {};
            int pickType = -1;
            int bestScore = 0;
            if (oneShot && target[0] != '\0') {
                strncpy(pickAddress, target, sizeof(pickAddress) - 1);
                int score = policy != nullptr ? policy->connectScore("", pickAddress) : 1;
                bestScore = score > 0 ? score : 1;
            } else {
                SeenDevice found[kMaxDevices];
                int foundCount = copyDevices(found, kMaxDevices);
                for (int i = 0; i < foundCount; i++) {
                    if (addressConnected(found[i].address)) {
                        continue;
                    }
                    int score = policy != nullptr
                        ? policy->connectScore(found[i].name, found[i].address)
                        : 0;
                    if (score > bestScore) {
                        bestScore = score;
                        strncpy(pickAddress, found[i].address, sizeof(pickAddress) - 1);
                        strncpy(pickName, found[i].name, sizeof(pickName) - 1);
                        pickType = found[i].addrType == 0xFF ? -1 : found[i].addrType;
                    }
                }
            }
            if (pickAddress[0] != '\0' && bestScore >= 2 && !hasRoomForOutgoing() && policy != nullptr) {
                policy->yieldOutgoing();
            }
            if (pickAddress[0] != '\0' && bestScore > 0 && hasRoomForOutgoing()) {
                if (pickName[0] == '\0' && collector.found) {
                    String n = collector.device.getName();
                    strncpy(pickName, n.c_str(), sizeof(pickName) - 1);
                }
                Debug->printf("Connecting to '%s'\n", pickName[0] != '\0' ? pickName : pickAddress);
                if (!connectAddressNow(pickAddress, pickName, oneShot ? 5000 : 4000, pickType)) {
                    lastOutgoingFailMs = millis();
                }
                if (radio != nullptr) {
                    radio->refreshLinkCount();
                }
                if (wifiUp) {
                    advertiseIfEnabled();
                }
            }
        }
        if (oneShot && !addressConnected(target)) {
            connectOnce = false;
            target[0] = '\0';
        }
        if (connectedCount() == 0) {
            // A continuous scan keeps the radio from Wi-Fi. Wait between
            // automatic scans; a scan the user started runs again immediately.
            int waitSeconds = scanRequested ? 1 : 20;
            for (int i = 0; i < waitSeconds && !scanRequested; i++) {
                keepAdvertising();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void BleMidi::setPolicy(BleMidiHostPolicy* connectPolicy) {
    policy = connectPolicy;
}

void BleMidi::setRadio(BleRadio* bleRadio) {
    radio = bleRadio;
}

void BleMidi::setAdvertising(bool enabled) {
    advertisingEnabled = enabled;
    if (radio != nullptr) {
        radio->setMidiPeripheral(enabled);
    }
    advertiseIfEnabled();
}

void BleMidi::setCentral(bool enabled) {
    centralEnabled = enabled;
    if (!enabled) {
        dropLink();
        return;
    }
}

void BleMidi::setMaxOutgoing(int n) {
    if (n < 0) {
        n = 0;
    }
    if (n > kMaxOutgoing) {
        n = kMaxOutgoing;
    }
    outgoingMax = n;
    for (int i = n; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected && outgoing[i].client != nullptr) {
            outgoing[i].client->disconnect();
        }
        clearSlot(i);
    }
}

void BleMidi::setAutoConnect(bool enabled) {
    autoConnectOn = enabled;
}

void BleMidi::allowOutgoing(bool enabled) {
    outgoingAllowed = enabled;
    if (enabled) {
        outgoingReadyMs = millis();
    }
}

void BleMidi::holdAddress(const char* address) {
    if (address == nullptr || address[0] == '\0') {
        heldAddress[0] = '\0';
        return;
    }
    strncpy(heldAddress, address, sizeof(heldAddress) - 1);
    heldAddress[sizeof(heldAddress) - 1] = '\0';
}

void BleMidi::releaseAddress(const char* address) {
    if (address == nullptr || address[0] == '\0' || heldAddress[0] == '\0') {
        return;
    }
    if (strcasecmp(heldAddress, address) == 0) {
        heldAddress[0] = '\0';
    }
}

void BleMidi::connectAddress(const char* address) {
    heldAddress[0] = '\0';
    connectOnce = true;
    directConnect = true;
    setTargetAddress(address);
    centralEnabled = true;
}

void BleMidi::dropAddress(const char* address) {
    int slot = slotOfAddress(address);
    if (slot < 0) {
        return;
    }
    if (outgoing[slot].client != nullptr && outgoing[slot].connected) {
        outgoing[slot].client->disconnect();
    }
    clearSlot(slot);
}

void BleMidi::dropLink() {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].client != nullptr && outgoing[i].connected) {
            outgoing[i].client->disconnect();
        }
        clearSlot(i);
    }
    target[0] = '\0';
}

void BleMidi::clearSlot(int slot) {
    if (slot < 0 || slot >= kMaxOutgoing) {
        return;
    }
    outgoing[slot].connected = false;
    outgoing[slot].characteristic = nullptr;
    outgoing[slot].name[0] = '\0';
    outgoing[slot].address[0] = '\0';
    outgoing[slot].haveRssi = false;
    outgoing[slot].rssi = 0;
    outgoing[slot].battery = -1;
    outgoing[slot].sinceMs = 0;
}

void BleMidi::write(midi::MidiMessage* msg, int len) {
    if (msg == nullptr || len <= 0) {
        return;
    }
    timestampLow = static_cast<uint8_t>((timestampLow + 1) & 0x7F);
    uint8_t packet[5];
    packet[0] = 0x80;
    packet[1] = static_cast<uint8_t>(0x80 | timestampLow);
    packet[2] = msg->status;
    packet[3] = msg->arg1;
    packet[4] = msg->arg2;
    size_t n = (len >= 2) ? 5 : 4;
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (!outgoing[i].connected || outgoing[i].characteristic == nullptr) {
            continue;
        }
        outgoing[i].characteristic->writeValue(packet, n, false);
    }
}

void BleMidi::dropClient(int slot) {
    if (slot < 0 || slot >= kMaxOutgoing) {
        return;
    }
    BLEClient* client = outgoing[slot].client;
    clearSlot(slot);
    if (client != nullptr && client->isConnected()) {
        client->disconnect();
        for (int i = 0; i < 20 && client->isConnected(); i++) {
            delay(50);
        }
    }
}

int BleMidi::freeOutgoingSlot() const {
    for (int i = 0; i < outgoingMax && i < kMaxOutgoing; i++) {
        if (!outgoing[i].connected) {
            return i;
        }
    }
    return -1;
}

int BleMidi::slotOfClient(BLEClient* client) const {
    if (client == nullptr) {
        return -1;
    }
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].client == client) {
            return i;
        }
    }
    return -1;
}

int BleMidi::slotOfAddress(const char* address) const {
    if (address == nullptr || address[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].address[0] != '\0' && strcasecmp(outgoing[i].address, address) == 0) {
            return i;
        }
    }
    return -1;
}

int BleMidi::connectedCount() const {
    int n = 0;
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected) {
            n++;
        }
    }
    return n;
}

bool BleMidi::addressConnected(const char* address) const {
    return slotOfAddress(address) >= 0 && outgoing[slotOfAddress(address)].connected;
}

int BleMidi::copyOutgoing(Outgoing* out, int max) const {
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < kMaxOutgoing && n < max; i++) {
        if (!outgoing[i].connected && outgoing[i].address[0] == '\0') {
            continue;
        }
        strncpy(out[n].name, outgoing[i].name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        strncpy(out[n].address, outgoing[i].address, sizeof(out[n].address) - 1);
        out[n].address[sizeof(out[n].address) - 1] = '\0';
        out[n].rssi = outgoing[i].rssi;
        out[n].battery = outgoing[i].battery;
        out[n].haveRssi = outgoing[i].haveRssi;
        out[n].connected = outgoing[i].connected;
        out[n].sinceMs = outgoing[i].sinceMs;
        n++;
    }
    return n;
}

bool BleMidi::hasRoomForOutgoing() const {
    if (!centralEnabled || outgoingMax <= 0) {
        return false;
    }
    if (connectedCount() >= outgoingMax) {
        return false;
    }
    return true;
}

void BleMidi::setTargetAddress(const char* address) {
    if (address == nullptr) {
        target[0] = '\0';
        return;
    }
    strncpy(target, address, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
}

bool BleMidi::advertising() const { return advertisingEnabled; }
bool BleMidi::central() const { return centralEnabled; }
bool BleMidi::isScanning() const { return scanning; }
bool BleMidi::isScanningFull() const { return scanFull; }
bool BleMidi::isConnected() const { return connectedCount() > 0; }

const char* BleMidi::connectedName() const {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected) {
            return outgoing[i].name;
        }
    }
    return "";
}

const char* BleMidi::connectedAddress() const {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected) {
            return outgoing[i].address;
        }
    }
    return "";
}

int BleMidi::connectedRssi() const {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected && outgoing[i].haveRssi) {
            return outgoing[i].rssi;
        }
    }
    return 0;
}

bool BleMidi::connectedRssiKnown() const {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected && outgoing[i].haveRssi) {
            return true;
        }
    }
    return false;
}

int BleMidi::connectedBattery() const {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (outgoing[i].connected && outgoing[i].battery >= 0) {
            return outgoing[i].battery;
        }
    }
    return -1;
}
const char* BleMidi::targetAddress() const { return target; }

void BleMidi::requestScan() {
    scanRequested = true;
}

void BleMidi::disconnect() {
    setCentral(false);
}

int BleMidi::copyDevices(SeenDevice* out, int max) const {
    if (out == nullptr || max <= 0) {
        return 0;
    }
    portENTER_CRITICAL(&seenMux);
    int count = seenCount;
    if (count > max) {
        count = max;
    }
    for (int i = 0; i < count; i++) {
        out[i] = seen[i];
    }
    portEXIT_CRITICAL(&seenMux);
    return count;
}

void BleMidi::noteDevice(BLEAdvertisedDevice& device) {
    std::string address = device.getAddress().toString().c_str();
    char complete[32];
    char shortName[32];
    readAdvertisedNames(device, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* name = complete[0] != '\0' ? complete : shortName;
    bool midi = false;
    bool hid = false;
    bool battery = false;
    parseAdServices(device.getPayload(), device.getPayloadLength(), midi, hid, battery);
    if (device.haveServiceUUID()) {
        if (device.isAdvertisingService(BLEUUID(MIDI_SERVICE_UUID))) {
            midi = true;
        }
        if (device.isAdvertisingService(BLEUUID((uint16_t)0x1812))) {
            hid = true;
        }
        if (device.isAdvertisingService(BLEUUID((uint16_t)0x180F))) {
            battery = true;
        }
    }
    noteSeen(address.c_str(), name, complete[0] != '\0', device.getRSSI(), midi, hid, battery, device.getAddress().getType());
}

void BleMidi::noteName(const char* address, const char* name, bool completeName, int rssi) {
    noteSeen(address, name, completeName, rssi, false, false, false);
}

void BleMidi::noteSeen(const char* address, const char* name, bool completeName, int rssi, bool midi, bool hid, bool battery, uint8_t addrType) {
    if (address == nullptr || address[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&seenMux);
    int slot = -1;
    for (int i = 0; i < seenCount; i++) {
        if (strcasecmp(seen[i].address, address) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (seenCount >= kMaxDevices) {
            portEXIT_CRITICAL(&seenMux);
            return;
        }
        slot = seenCount++;
        memset(&seen[slot], 0, sizeof(seen[slot]));
        seen[slot].addrType = 0xFF;
        strncpy(seen[slot].address, address, sizeof(seen[slot].address) - 1);
    }
    if (name != nullptr && name[0] != '\0' && (completeName || seen[slot].name[0] == '\0')) {
        strncpy(seen[slot].name, name, sizeof(seen[slot].name) - 1);
        seen[slot].name[sizeof(seen[slot].name) - 1] = '\0';
    }
    if (rssi != 0) {
        seen[slot].rssi = rssi;
    }
    seen[slot].present = true;
    if (midi) {
        seen[slot].midi = true;
    }
    if (hid) {
        seen[slot].hid = true;
    }
    if (battery) {
        seen[slot].battery = true;
    }
    if (addrType != 0xFF) {
        seen[slot].addrType = addrType;
    }
    portEXIT_CRITICAL(&seenMux);
}

int BleMidi::onGap(struct ble_gap_event* event, void* /*arg*/) {
    if (instance == nullptr || event == nullptr) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_CONN_UPDATE_REQ && event->conn_update_req.self_params != nullptr) {
        // A pedal asks for 7.5 ms. Two live links at 50–100 ms still keep
        // unicast Wi-Fi off the air, so showbox.local never returns a page.
        event->conn_update_req.self_params->itvl_min = 0x50;
        event->conn_update_req.self_params->itvl_max = 0xA0;
        event->conn_update_req.self_params->latency = 2;
        event->conn_update_req.self_params->supervision_timeout = 800;
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_CONN_UPDATE) {
        struct ble_gap_conn_desc desc = {};
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            if (instance->Debug != nullptr) {
                instance->Debug->printf("BLE interval %u x1.25ms handle %u rc %d\n",
                    desc.conn_itvl, event->conn_update.conn_handle, event->conn_update.status);
            }
            static uint32_t lastForceMs = 0;
            if (desc.conn_itvl < 0x50 && (lastForceMs == 0 || (int32_t)(millis() - lastForceMs) > 4000)) {
                lastForceMs = millis();
                ble_gap_upd_params params = {};
                params.itvl_min = 0x50;
                params.itvl_max = 0xA0;
                params.latency = 2;
                params.supervision_timeout = 800;
                params.min_ce_len = 0;
                params.max_ce_len = 16;
                ble_gap_update_params(event->conn_update.conn_handle, &params);
            }
        }
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC || event->disc.data == nullptr || event->disc.length_data == 0) {
        return 0;
    }
    char complete[32] = {};
    char shortName[32] = {};
    parseAdNames(event->disc.data, event->disc.length_data, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* name = complete[0] != '\0' ? complete : shortName;
    bool midi = false;
    bool hid = false;
    bool battery = false;
    parseAdServices(event->disc.data, event->disc.length_data, midi, hid, battery);
    if (name[0] == '\0' && !midi && !hid && !battery) {
        return 0;
    }
    BLEAddress address(event->disc.addr);
    String text = address.toString();
    instance->noteSeen(text.c_str(), name, complete[0] != '\0', event->disc.rssi, midi, hid, battery, address.getType());
    return 0;
}

bool BleMidi::wantsDevice(BLEAdvertisedDevice& device) const {
    if (!centralEnabled) {
        return false;
    }
    std::string address = device.getAddress().toString().c_str();
    bool targetMatch = target[0] != '\0' && strcasecmp(target, address.c_str()) == 0;
    if (heldAddress[0] != '\0' && strcasecmp(heldAddress, address.c_str()) == 0 && !targetMatch) {
        return false;
    }
    if (connectOnce && target[0] != '\0') {
        return targetMatch;
    }
    char complete[32];
    char shortName[32];
    readAdvertisedNames(device, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* name = complete[0] != '\0' ? complete : shortName;
    if (policy != nullptr) {
        return policy->connectScore(name, address.c_str()) > 0;
    }
    return false;
}

void BleMidi::clearSeen() {
    portENTER_CRITICAL(&seenMux);
    for (int i = 0; i < seenCount; i++) {
        seen[i].present = false;
    }
    portEXIT_CRITICAL(&seenMux);
}

void BleMidi::pruneUnseen() {
    portENTER_CRITICAL(&seenMux);
    int write = 0;
    for (int i = 0; i < seenCount; i++) {
        if (!seen[i].present) {
            continue;
        }
        if (write != i) {
            seen[write] = seen[i];
        }
        write++;
    }
    seenCount = write;
    portEXIT_CRITICAL(&seenMux);
}

void BleMidi::advertiseIfEnabled() {
    if (radio != nullptr) {
        radio->setMidiPeripheral(advertisingEnabled);
        radio->apply();
        return;
    }
    // Standalone MultiMidi: advertise MIDI without a shared BleRadio.
    if (!BLEDevice::getInitialized()) {
        return;
    }
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    if (advertising == nullptr) {
        return;
    }
    if (!advertisingEnabled) {
        advertising->stop();
        return;
    }
    if (advertising->isAdvertising()) {
        return;
    }
    BLEAdvertisementData adv;
    adv.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    adv.setPartialServices(BLEUUID("03B80E5A-EDE8-4B33-A751-6CE34EC4C700"));
    advertising->setMinInterval(0xA0);
    advertising->setMaxInterval(0x140);
    advertising->setScanResponse(false);
    advertising->setAdvertisementData(adv);
    advertising->start();
}

void BleMidi::keepAdvertising() {
    if (radio != nullptr) {
        radio->service();
    }
    if (WiFi.status() == WL_CONNECTED) {
        advertiseIfEnabled();
    }
}

void BleMidi::refreshLink() {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (!outgoing[i].connected || outgoing[i].client == nullptr || !outgoing[i].client->isConnected()) {
            continue;
        }
        int rssi = outgoing[i].client->getRssi();
        if (rssi != 0) {
            outgoing[i].rssi = rssi;
            outgoing[i].haveRssi = true;
            noteSeen(outgoing[i].address, nullptr, false, rssi, false, false, false);
        }
        if (outgoing[i].lastBatteryReadMs == 0 || (int32_t)(millis() - outgoing[i].lastBatteryReadMs) >= 30000) {
            readPeerBattery();
        }
    }
}

void BleMidi::readPeerBattery() {
    for (int i = 0; i < kMaxOutgoing; i++) {
        if (!outgoing[i].connected || outgoing[i].client == nullptr || !outgoing[i].client->isConnected()) {
            continue;
        }
        outgoing[i].lastBatteryReadMs = millis();
        BLERemoteService* service = outgoing[i].client->getService(BLEUUID((uint16_t)0x180F));
        if (service == nullptr) {
            continue;
        }
        BLERemoteCharacteristic* characteristic = service->getCharacteristic(BLEUUID((uint16_t)0x2A19));
        if (characteristic == nullptr || !characteristic->canRead()) {
            continue;
        }
        outgoing[i].battery = characteristic->readUInt8();
        noteSeen(outgoing[i].address, nullptr, false, 0, false, false, true);
    }
}

void BleMidi::onConnect(BLEClient* client) {
    int slot = slotOfClient(client);
    if (slot >= 0 && outgoing[slot].sinceMs == 0) {
        outgoing[slot].sinceMs = millis();
    }
    connectOnce = false;
    Debug->println("Bluetooth MIDI GAP up");
}

void BleMidi::onDisconnect(BLEClient* client) {
    int slot = slotOfClient(client);
    if (slot >= 0) {
        clearSlot(slot);
    }
    Debug->println("Bluetooth MIDI disconnected");
}

void BleMidi::onNotify(BLERemoteCharacteristic* /*characteristic*/, uint8_t* data, size_t length, bool /*isNotify*/) {
    if (instance == nullptr || data == nullptr || length == 0) {
        return;
    }
    midiInboundSetSource("ble");
    instance->decoder.feed(data, length);
}

void BleMidi::connectTo(BLEAdvertisedDevice* device) {
    if (device == nullptr) {
        return;
    }
    char complete[32];
    char shortName[32];
    readAdvertisedNames(*device, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* chosen = complete[0] != '\0' ? complete : shortName;
    String address = device->getAddress().toString();
    connectAddressNow(address.c_str(), chosen, 5000, device->getAddress().getType());
}

uint8_t BleMidi::typeForAddress(const char* address) const {
    if (address != nullptr && address[0] != '\0') {
        portENTER_CRITICAL(&seenMux);
        for (int i = 0; i < seenCount; i++) {
            if (strcasecmp(seen[i].address, address) == 0 && seen[i].addrType != 0xFF) {
                uint8_t type = seen[i].addrType;
                portEXIT_CRITICAL(&seenMux);
                return type;
            }
        }
        portEXIT_CRITICAL(&seenMux);
    }
    if (address == nullptr || strlen(address) < 2) {
        return BLE_ADDR_PUBLIC;
    }
    char octet[3] = {address[0], address[1], 0};
    unsigned msb = strtoul(octet, nullptr, 16);
    return (msb & 0xC0) != 0 ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
}

bool BleMidi::tryConnect(BLEClient* client, const char* address, uint8_t type, uint32_t timeoutMs) {
    BLEAddress bleAddress(String(address), type);
    if (client->connect(bleAddress, type, timeoutMs)) {
        return true;
    }
    uint16_t handle = client->getConnId();
    if (handle != BLE_HS_CONN_HANDLE_NONE && handle != 0xFF) {
        ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 30 && client->getConnId() == handle; i++) {
            delay(50);
        }
    }
    return false;
}

bool BleMidi::connectAddressNow(const char* address, const char* name, uint32_t timeoutMs, int addrType) {
    if (address == nullptr || address[0] == '\0' || addressConnected(address)) {
        return address != nullptr && addressConnected(address);
    }
    int slot = freeOutgoingSlot();
    if (slot < 0) {
        Debug->println("No free peripheral slot");
        return false;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    // createClient is the internal-RAM hit. A live slot can retry cheaper.
    if (outgoing[slot].client == nullptr && largest < 2048) {
        debugLog("ble skip connect largest=%u", (unsigned)largest);
        return false;
    }
    if (outgoing[slot].client == nullptr) {
        outgoing[slot].client = BLEDevice::createClient();
        outgoing[slot].client->setClientCallbacks(this);
    }
    BLEClient* client = outgoing[slot].client;
    if (name != nullptr && name[0] != '\0') {
        strncpy(outgoing[slot].name, name, sizeof(outgoing[slot].name) - 1);
        outgoing[slot].name[sizeof(outgoing[slot].name) - 1] = '\0';
    }
    strncpy(outgoing[slot].address, address, sizeof(outgoing[slot].address) - 1);
    outgoing[slot].address[sizeof(outgoing[slot].address) - 1] = '\0';
    outgoing[slot].sinceMs = millis();
    if (timeoutMs < 1500) {
        timeoutMs = 1500;
    }
    if (timeoutMs > 3500) {
        timeoutMs = 3500;
    }
    uint8_t type = addrType >= 0 ? static_cast<uint8_t>(addrType) : typeForAddress(address);
    Debug->printf("Connecting MIDI at %s as %s\n", address, type == BLE_ADDR_RANDOM ? "random" : "public");
    auto resumeAdv = [this]() {
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        if (WiFi.status() == WL_CONNECTED) {
            advertiseIfEnabled();
        }
    };
    // Keep BALANCE. PREFER_BT during GAP connect made ICMP live and TCP
    // (HTTP, telnet, AppleMIDI) stall until the next reboot.
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    bool ok = tryConnect(client, address, type, timeoutMs);
    if (!ok && addrType < 0) {
        uint8_t other = type == BLE_ADDR_RANDOM ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
        Debug->printf("Retrying MIDI at %s as %s\n", address, other == BLE_ADDR_RANDOM ? "random" : "public");
        ok = tryConnect(client, address, other, 1500);
        if (ok) {
            type = other;
        }
    }
    if (!ok) {
        strncpy(lastFailAddress, address, sizeof(lastFailAddress) - 1);
        lastFailAddress[sizeof(lastFailAddress) - 1] = '\0';
        lastFailType = type;
        const char* label = outgoing[slot].name[0] != '\0' ? outgoing[slot].name : address;
        char line[88];
        snprintf(line, sizeof(line), "Could not connect to %s", label);
        Debug->println(line);
        if (policy != nullptr) {
            policy->note(line);
        }
        dropClient(slot);
        resumeAdv();
        return false;
    }
    lastFailAddress[0] = '\0';
    lastFailType = 0xFF;
    BLERemoteService* service = client->getService(MIDI_SERVICE_UUID);
    if (service == nullptr) {
        Debug->println("Peripheral has no BLE MIDI service");
        if (policy != nullptr) {
            policy->note("Peripheral has no BLE MIDI service");
        }
        client->disconnect();
        dropClient(slot);
        resumeAdv();
        return false;
    }
    BLERemoteCharacteristic* characteristic = service->getCharacteristic(MIDI_CHARACTERISTIC_UUID);
    if (characteristic == nullptr) {
        Debug->println("Peripheral has no BLE MIDI characteristic");
        if (policy != nullptr) {
            policy->note("Peripheral has no BLE MIDI characteristic");
        }
        client->disconnect();
        dropClient(slot);
        resumeAdv();
        return false;
    }
    characteristic->registerForNotify(BleMidi::onNotify, true);
    outgoing[slot].characteristic = characteristic;
    client->updateConnParams(0x50, 0xA0, 2, 400);
    {
        struct ble_gap_conn_desc desc = {};
        if (ble_gap_conn_find(client->getConnId(), &desc) == 0 && desc.conn_itvl < 0x50) {
            Debug->printf("MIDI link still %u, requesting 100-200 ms\n", desc.conn_itvl);
            client->updateConnParams(0x50, 0xA0, 2, 400);
        }
    }
    outgoing[slot].connected = true;
    outgoing[slot].lastBatteryReadMs = 0;
    connectOnce = false;
    target[0] = '\0';
    refreshLink();
    Debug->printf("Bluetooth MIDI ready: %s %s\n", outgoing[slot].name, outgoing[slot].address);
    if (policy != nullptr) {
        char line[88];
        const char* label = outgoing[slot].name[0] != '\0' ? outgoing[slot].name : outgoing[slot].address;
        snprintf(line, sizeof(line), "Connected to %s", label);
        policy->note(line);
    }
    resumeAdv();
    return true;
}
