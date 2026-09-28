#include "BleMidi.h"
#include "BleRadio/BleRadio.h"
#include <BLEDevice.h>
#include <host/ble_gap.h>
#include "MidiCommon.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <cstring>

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
    parser = new midi::MidiBleParser(action);
    if (!BLEDevice::getInitialized()) {
        BLEDevice::init("ShowboxMod");
    }
    BLEDevice::setCustomGapHandler(BleMidi::onGap);
    Debug->println("Bluetooth MIDI: connect out to peripherals, and advertise as ShowboxMod.");
    // Internal RAM is nearly gone once the controller is up. This stack can live
    // in PSRAM; the scan loop does not touch the flash cache.
    if (xTaskCreatePinnedToCoreWithCaps(BleMidi::taskEntry, "ble-midi", 12288, this, 1, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreate(BleMidi::taskEntry, "ble-midi", 12288, this, 1, nullptr);
    }
}

namespace {
// Returns true when a scan actually ran. A scan that the radio rejects returns immediately.
bool runScan(BleScanCollector& collector, bool wide) {
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&collector, true);
    scan->setActiveScan(true);
    scan->setInterval(wide ? 100 : 160);
    // A window of 99 leaves Wi-Fi almost no airtime, so the access point drops
    // the module and showbox.local stops answering. The wide window is only for
    // a scan the user asked for.
    scan->setWindow(wide ? 99 : 20);
    unsigned long started = millis();
    if (!scan->start(kScanSeconds, false)) {
        return false;
    }
    // NimBLE start() returns as soon as the controller accepts the scan.
    // Advertising on top of that scan takes the radio from Wi-Fi.
    while (scan->isScanning() && millis() - started < (kScanSeconds * 1000 + 500)) {
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
        BleRadio::instance().service();
        if (WiFi.status() != WL_CONNECTED) {
            BLEAdvertising* advertising = BLEDevice::getAdvertising();
            if (advertising != nullptr) {
                advertising->stop();
            }
            BleRadio::instance().restartWhenIdle();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool shouldScan = scanRequested || (centralEnabled && !connected);
        if (!shouldScan) {
            advertiseIfEnabled();
            vTaskDelay(pdMS_TO_TICKS(connected ? 5000 : 1000));
            continue;
        }

        scanFull = scanRequested;
        scanRequested = false;
        clearSeen();
        scanning = true;
        Debug->println("Scanning for Bluetooth MIDI devices");
        BleScanCollector collector;
        bool scanned = runScan(collector, scanFull);
        if (!collector.found && !scanned) {
            Debug->println("Pausing ShowboxMod advertising for one scan");
            BLEDevice::getAdvertising()->stop();
            vTaskDelay(pdMS_TO_TICKS(300));
            scanned = runScan(collector, scanFull);
        }
        pruneUnseen();
        scanning = false;
        scanFull = false;
        advertiseIfEnabled();

        if (collector.found && centralEnabled && !connected) {
            Debug->printf("Connecting to '%s'\n", collector.device.getName().c_str());
            connectTo(&collector.device);
            advertiseIfEnabled();
        }
        if (!connected) {
            // A continuous scan keeps the radio from Wi-Fi. Wait between
            // automatic scans; a scan the user started runs again immediately.
            int waitSeconds = scanRequested ? 1 : 20;
            for (int i = 0; i < waitSeconds && !scanRequested; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void BleMidi::setPolicy(BleMidiHostPolicy* connectPolicy) {
    policy = connectPolicy;
}

void BleMidi::setAdvertising(bool enabled) {
    advertisingEnabled = enabled;
    BleRadio::instance().setMidiPeripheral(enabled);
    advertiseIfEnabled();
}

void BleMidi::setCentral(bool enabled) {
    centralEnabled = enabled;
    if (!enabled) {
        dropLink();
        return;
    }
    scanRequested = true;
}

void BleMidi::setAutoConnect(bool enabled) {
    autoConnectOn = enabled;
}

void BleMidi::holdAddress(const char* address) {
    if (address == nullptr || address[0] == '\0') {
        heldAddress[0] = '\0';
        return;
    }
    strncpy(heldAddress, address, sizeof(heldAddress) - 1);
    heldAddress[sizeof(heldAddress) - 1] = '\0';
}

void BleMidi::connectAddress(const char* address) {
    heldAddress[0] = '\0';
    connectOnce = true;
    bool different = connected && address != nullptr && strcasecmp(peerAddress, address) != 0;
    if (different && client != nullptr) {
        client->disconnect();
        connected = false;
        peerName[0] = '\0';
        peerAddress[0] = '\0';
    }
    setTargetAddress(address);
    centralEnabled = true;
    scanRequested = true;
}

void BleMidi::dropLink() {
    if (client != nullptr && connected) {
        client->disconnect();
    }
    connected = false;
    peerName[0] = '\0';
    peerAddress[0] = '\0';
    target[0] = '\0';
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
const char* BleMidi::connectedName() const { return peerName; }
const char* BleMidi::connectedAddress() const { return peerAddress; }
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
    bool completeName = complete[0] != '\0';
    bool midi = device.haveServiceUUID() && device.isAdvertisingService(BLEUUID(MIDI_SERVICE_UUID));
    int rssi = device.getRSSI();

    portENTER_CRITICAL(&seenMux);
    int slot = -1;
    for (int i = 0; i < seenCount; i++) {
        if (strcasecmp(seen[i].address, address.c_str()) == 0) {
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
        strncpy(seen[slot].address, address.c_str(), sizeof(seen[slot].address) - 1);
    }
    if (name[0] != '\0' && (completeName || seen[slot].name[0] == '\0')) {
        strncpy(seen[slot].name, name, sizeof(seen[slot].name) - 1);
        seen[slot].name[sizeof(seen[slot].name) - 1] = '\0';
    }
    seen[slot].rssi = rssi;
    seen[slot].present = true;
    if (midi) {
        seen[slot].midi = true;
    }
    portEXIT_CRITICAL(&seenMux);
}

void BleMidi::noteName(const char* address, const char* name, bool completeName, int rssi) {
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
    portEXIT_CRITICAL(&seenMux);
}

int BleMidi::onGap(struct ble_gap_event* event, void* /*arg*/) {
    if (instance == nullptr || event == nullptr) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_CONN_UPDATE_REQ && event->conn_update_req.self_params != nullptr) {
        // A pedal asks for 7.5 ms. That interval is what kept unicast Wi-Fi
        // off the air. Offer 50–100 ms instead of accepting the request.
        event->conn_update_req.self_params->itvl_min = 0x28;
        event->conn_update_req.self_params->itvl_max = 0x50;
        event->conn_update_req.self_params->latency = 0;
        event->conn_update_req.self_params->supervision_timeout = 400;
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC || event->disc.data == nullptr || event->disc.length_data == 0) {
        return 0;
    }
    char complete[32] = {};
    char shortName[32] = {};
    parseAdNames(event->disc.data, event->disc.length_data, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* name = complete[0] != '\0' ? complete : shortName;
    if (name[0] == '\0') {
        return 0;
    }
    BLEAddress address(event->disc.addr);
    String text = address.toString();
    instance->noteName(text.c_str(), name, complete[0] != '\0', event->disc.rssi);
    return 0;
}

bool BleMidi::wantsDevice(BLEAdvertisedDevice& device) const {
    if (!centralEnabled) {
        return false;
    }
    std::string address = device.getAddress().toString().c_str();
    if (heldAddress[0] != '\0' && strcasecmp(heldAddress, address.c_str()) == 0) {
        return false;
    }
    bool targetMatch = target[0] != '\0' && strcasecmp(target, address.c_str()) == 0;
    if (target[0] != '\0' && !targetMatch) {
        return false;
    }
    if (connectOnce && targetMatch) {
        return true;
    }
    char complete[32];
    char shortName[32];
    readAdvertisedNames(device, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* name = complete[0] != '\0' ? complete : shortName;
    if (policy != nullptr) {
        return policy->shouldConnect(name, address.c_str());
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
    BleRadio::instance().setMidiPeripheral(advertisingEnabled);
    BleRadio::instance().apply();
}

bool BleMidi::isConnected() const {
    return connected;
}

void BleMidi::onConnect(BLEClient* /*client*/) {
    connected = true;
    connectOnce = false;
    Debug->println("Bluetooth MIDI connected");
}

void BleMidi::onDisconnect(BLEClient* /*client*/) {
    connected = false;
    Debug->println("Bluetooth MIDI disconnected");
}

void BleMidi::onNotify(BLERemoteCharacteristic* /*characteristic*/, uint8_t* data, size_t length, bool /*isNotify*/) {
    if (instance == nullptr || instance->parser == nullptr || data == nullptr || length == 0) {
        return;
    }
    uint8_t clipped = length > 255 ? 255 : static_cast<uint8_t>(length);
    instance->parser->parse(data, clipped);
}

void BleMidi::connectTo(BLEAdvertisedDevice* device) {
    if (client == nullptr) {
        client = BLEDevice::createClient();
        client->setClientCallbacks(this);
    }
    char complete[32];
    char shortName[32];
    readAdvertisedNames(*device, complete, sizeof(complete), shortName, sizeof(shortName));
    const char* chosen = complete[0] != '\0' ? complete : shortName;
    strncpy(peerName, chosen, sizeof(peerName) - 1);
    peerName[sizeof(peerName) - 1] = '\0';
    String address = device->getAddress().toString();
    strncpy(peerAddress, address.c_str(), sizeof(peerAddress) - 1);
    peerAddress[sizeof(peerAddress) - 1] = '\0';
    if (!client->connect(device)) {
        connected = false;
        peerName[0] = '\0';
        peerAddress[0] = '\0';
        Debug->println("Bluetooth MIDI connection failed");
        return;
    }
    BLERemoteService* service = client->getService(MIDI_SERVICE_UUID);
    if (service == nullptr) {
        Debug->println("Peripheral has no BLE MIDI service");
        client->disconnect();
        connected = false;
        return;
    }
    BLERemoteCharacteristic* characteristic = service->getCharacteristic(MIDI_CHARACTERISTIC_UUID);
    if (characteristic == nullptr) {
        Debug->println("Peripheral has no BLE MIDI characteristic");
        client->disconnect();
        connected = false;
        return;
    }
    characteristic->registerForNotify(BleMidi::onNotify, true);
    client->updateConnParams(0x28, 0x50, 0, 400);
    connected = true;
    connectOnce = false;
    Debug->printf("Bluetooth MIDI ready: %s %s\n", peerName, peerAddress);
}
