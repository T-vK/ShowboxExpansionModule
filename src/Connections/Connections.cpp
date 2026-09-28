#include "Connections.h"
#include "MultiMidi/MultiMidi.h"
#include "BleRadio/BleRadio.h"
#include "UsbHid/UsbHid.h"
#include "MultiMidi/UsbMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include <BLEAddress.h>
#include <Preferences.h>
#include <host/ble_gap.h>
#include <cstring>

namespace {
String jsonEscape(const char* text) {
    String out;
    if (text == nullptr) {
        return out;
    }
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
        }
        if (*p >= 32) {
            out += *p;
        }
    }
    return out;
}

const char* jsonBool(bool value) {
    return value ? "true" : "false";
}

const char* blockedReason(const String& name) {
    if (name == "usb") {
        return "Choose USB MIDI host, USB MIDI device, or USB HID peripheral.";
    }
    if (name == "usbMidi2Host" || name == "usbMidi2Peripheral") {
        return "USB MIDI 2.0 is not implemented. USB MIDI 1.0 is the peripheral switch.";
    }
    if (name == "ethernetMidi2") {
        return "Network MIDI 2.0 is not implemented. Ethernet MIDI 1.0 is the Ethernet switch.";
    }
    if (name == "wifiMidi2") {
        return "Network MIDI 2.0 is not implemented yet.";
    }
    if (name == "bleClassic") {
        return "The ESP32-S3 has no Bluetooth Classic radio, so Serial MIDI is off.";
    }
    return nullptr;
}

struct TransportRow {
    const char* id;
    bool available;
    const char* reason;
};

const TransportRow kTransports[] = {
    {"din", true, nullptr},
    {"bleMidiHost", true, nullptr},
    {"bleMidiPeripheral", true, nullptr},
    {"bleClassic", false, "The ESP32-S3 has no Bluetooth Classic radio, so Serial MIDI is off."},
    {"usbMidiHost", true, nullptr},
    {"usbMidiPeripheral", true, nullptr},
    {"usbMidi2Host", false, "USB MIDI 2.0 is not implemented."},
    {"usbMidi2Peripheral", false, "USB MIDI 2.0 is not implemented."},
    {"wifi", true, nullptr},
    {"ethernet", true, nullptr},
    {"wifiMidi2", false, "Network MIDI 2.0 is not implemented yet."},
    {"ethernetMidi2", false, "Network MIDI 2.0 is not implemented. Ethernet MIDI 1.0 is the Ethernet switch."},
    {"bleHid", true, nullptr},
    {"usbHid", true, nullptr},
};

bool transportEnabled(const Connections* self, MultiMidi* midi, const char* id) {
    if (strcmp(id, "din") == 0) return midi->dinIsEnabled();
    if (strcmp(id, "wifi") == 0) return midi->wifiIsEnabled();
    if (strcmp(id, "bleMidiHost") == 0) return midi->bleHostEnabled();
    if (strcmp(id, "bleMidiPeripheral") == 0) return midi->blePeripheralEnabled();
    if (strcmp(id, "ethernet") == 0) return midi->ethernetIsEnabled();
    if (strcmp(id, "usbMidiPeripheral") == 0) return self->usbDeviceOn();
    if (strcmp(id, "usbMidiHost") == 0) return self->usbHostOn();
    if (strcmp(id, "usbHid") == 0) return self->usbHidOn();
    if (strcmp(id, "bleHid") == 0) return false;
    return false;
}

const char* transportNote(MultiMidi* midi, const char* id, bool enabled) {
    if (strcmp(id, "ethernet") == 0 && midi != nullptr) {
        return midi->ethernetNote();
    }
    if ((strcmp(id, "usbMidiPeripheral") == 0 || strcmp(id, "usbMidiHost") == 0 || strcmp(id, "usbHid") == 0) && enabled) {
        if (strcmp(id, "usbMidiHost") == 0 && UsbMidiHost::running()) {
            return UsbMidiHost::status();
        }
        if (UsbMidi::running() || UsbMidiHost::running() || UsbHid::running()) {
            return "USB serial/JTAG is off. OTA and the UART connector can still flash.";
        }
        return "Rebooting. USB serial/JTAG turns off. OTA and the UART connector can still flash.";
    }
    return "";
}
}

void Connections::attach(MultiMidi* midi) {
    multiMidi = midi;
}

void Connections::load() {
    peers.load();
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return;
    }
    hidEnabled = prefs.getBool("hid", false);
    usbDeviceEnabled = prefs.getBool("usbdev", false);
    usbHostEnabled = prefs.getBool("usbhost", false);
    usbHidEnabled = prefs.getBool("usbhid", false);
    bool autoOn = prefs.getBool("auto", true);
    String legacy = prefs.getString("bleaddr", "");
    prefs.end();
    if (multiMidi != nullptr) {
        multiMidi->setAutoConnect(autoOn, false);
    }
    if (peers.midiPeerCount() == 0 && legacy.length() >= 11) {
        peers.upsert(legacy.c_str(), "", 1, -1, 1);
    }
}

void Connections::startAfterBle() {
    BLEServer* server = multiMidi == nullptr ? nullptr : multiMidi->bleGattServer();
    BleRadio::instance().attach(server);
    hid.begin(server, &Serial);
    if (hidEnabled) {
        hid.setEnabled(true);
    }
}

void Connections::tick() {
    if (rebootAt != 0 && (int32_t)(millis() - rebootAt) >= 0) {
        ESP.restart();
    }
    BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
    int incomingCount = BleRadio::instance().copyIncoming(incoming, BleRadio::kMaxIncoming);
    for (int i = 0; i < incomingCount; i++) {
        BlePeers::Peer existing;
        if (!peers.find(incoming[i].address, &existing)) {
            peers.upsert(incoming[i].address, "", 1, hidEnabled ? 1 : 0, 0, 0, 1);
        }
    }
    if (multiMidi != nullptr) {
        BleMidi::SeenDevice seen[BleMidi::kMaxDevices];
        int seenCount = multiMidi->copyBleScan(seen, BleMidi::kMaxDevices);
        BlePeers::Peer saved[BlePeers::kMax];
        int savedCount = peers.copy(saved, BlePeers::kMax);
        for (int i = 0; i < seenCount; i++) {
            if (seen[i].name[0] == '\0') {
                continue;
            }
            for (int j = 0; j < savedCount; j++) {
                if (sameAddress(saved[j].address, seen[i].address) && strlen(seen[i].name) > strlen(saved[j].name)) {
                    peers.upsert(seen[i].address, seen[i].name, -1, -1, -1);
                }
            }
        }
    }
    hid.tick();
}

bool Connections::shouldConnect(const char* name, const char* address) {
    (void)name;
    return address != nullptr && peers.matchesMidiAuto(address);
}

bool Connections::sameAddress(const char* left, const char* right) const {
    if (left == nullptr || right == nullptr || left[0] == '\0' || right[0] == '\0') {
        return false;
    }
    return strcasecmp(left, right) == 0;
}

void Connections::saveUsbMode(bool device, bool host, bool hid) {
    usbDeviceEnabled = device;
    usbHostEnabled = host;
    usbHidEnabled = hid;
    Preferences prefs;
    if (!prefs.begin("midi", false)) {
        return;
    }
    prefs.putBool("usbdev", device);
    prefs.putBool("usbhost", host);
    prefs.putBool("usbhid", hid);
    prefs.end();
    if (multiMidi != nullptr) {
        multiMidi->rememberUsbDevice(device);
    }
}

void Connections::saveHid() {
    Preferences prefs;
    if (!prefs.begin("midi", false)) {
        return;
    }
    prefs.putBool("hid", hidEnabled);
    prefs.end();
}

String Connections::statusJson() {
    String json = "{\"reboot\":";
    json += jsonBool(rebootPending);
    json += ",\"discoverable\":";
    json += jsonBool(multiMidi != nullptr && multiMidi->blePeripheralEnabled());
    json += ",\"scanning\":";
    json += jsonBool(multiMidi != nullptr && multiMidi->bleScanning());
    json += ",\"hidReady\":";
    json += jsonBool(hid.ready());
    json += ",\"transports\":[";
    bool first = true;
    for (const TransportRow& row : kTransports) {
        if (!first) json += ",";
        first = false;
        bool enabled = false;
        if (strcmp(row.id, "bleHid") == 0) enabled = hidEnabled;
        else if (row.available && multiMidi != nullptr) enabled = transportEnabled(this, multiMidi, row.id);
        json += "{\"id\":\"" + String(row.id) + "\",\"enabled\":" + jsonBool(enabled);
        json += ",\"available\":" + String(jsonBool(row.available));
        json += ",\"reason\":\"" + jsonEscape(row.reason == nullptr ? "" : row.reason) + "\"";
        json += ",\"note\":\"" + jsonEscape(transportNote(multiMidi, row.id, enabled)) + "\"}";
    }

    BlePeers::Peer saved[BlePeers::kMax];
    int savedCount = peers.copy(saved, BlePeers::kMax);
    const char* hostAddress = multiMidi == nullptr ? "" : multiMidi->hostAddress();
    BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
    int incomingCount = BleRadio::instance().copyIncoming(incoming, BleRadio::kMaxIncoming);
    auto knownAddress = [&](const char* address) {
        for (int i = 0; i < savedCount; i++) {
            if (sameAddress(saved[i].address, address)) return true;
        }
        return false;
    };
    auto incomingMatch = [&](const char* address) {
        for (int i = 0; i < incomingCount; i++) {
            if (sameAddress(address, incoming[i].address)) return true;
        }
        return false;
    };
    auto appendPeer = [&](const char* name, const char* address, bool midi, bool hidRole, bool autoOn, bool connected) {
        json += "{\"name\":\"" + jsonEscape(name) + "\"";
        json += ",\"address\":\"" + jsonEscape(address) + "\"";
        json += ",\"midi\":" + String(jsonBool(midi));
        json += ",\"hid\":" + String(jsonBool(hidRole));
        json += ",\"autoConnect\":" + String(jsonBool(autoOn));
        json += ",\"connected\":" + String(jsonBool(connected)) + "}";
    };

    json += "],\"nearby\":[";
    bool anyNearby = false;
    if (multiMidi != nullptr) {
        BleMidi::SeenDevice seen[BleMidi::kMaxDevices];
        int n = multiMidi->copyBleScan(seen, BleMidi::kMaxDevices);
        for (int i = 0; i < n; i++) {
            if (knownAddress(seen[i].address)) {
                continue;
            }
            if (anyNearby) json += ",";
            anyNearby = true;
            json += "{\"name\":\"" + jsonEscape(seen[i].name) + "\"";
            json += ",\"address\":\"" + jsonEscape(seen[i].address) + "\"";
            json += ",\"rssi\":" + String(seen[i].rssi);
            json += ",\"midi\":" + String(jsonBool(seen[i].midi)) + "}";
        }
    }
    json += "],\"peripherals\":[";
    bool anyPeripheral = false;
    for (int i = 0; i < savedCount; i++) {
        if (saved[i].host) {
            continue;
        }
        if (anyPeripheral) json += ",";
        anyPeripheral = true;
        bool out = multiMidi != nullptr && multiMidi->hostConnected() && sameAddress(saved[i].address, hostAddress);
        appendPeer(saved[i].name, saved[i].address, saved[i].midi, saved[i].hid, saved[i].autoConnect, out);
    }
    json += "],\"hosts\":[";
    bool anyHost = false;
    for (int i = 0; i < savedCount; i++) {
        if (!saved[i].host) {
            continue;
        }
        if (anyHost) json += ",";
        anyHost = true;
        appendPeer(saved[i].name, saved[i].address, saved[i].midi, saved[i].hid, false, incomingMatch(saved[i].address));
    }
    for (int i = 0; i < incomingCount; i++) {
        if (knownAddress(incoming[i].address)) {
            continue;
        }
        if (anyHost) json += ",";
        anyHost = true;
        appendPeer("", incoming[i].address, true, hidEnabled, false, true);
    }
    json += "]}";
    return json;
}

bool Connections::setOption(const String& name, bool enabled, String& error) {
    const char* blocked = blockedReason(name);
    if (blocked != nullptr) {
        error = blocked;
        return false;
    }
    if (multiMidi == nullptr) {
        error = "MIDI is not available.";
        return false;
    }
    if (name == "autoConnect") {
        multiMidi->setAutoConnect(enabled, true);
        if (enabled) multiMidi->requestBleScan();
        return true;
    }
    if (name == "bleHid") {
        hidEnabled = enabled;
        saveHid();
        hid.setEnabled(enabled);
        return true;
    }
    if (name == "ethernet" || name == "ethernetMidi") {
        return multiMidi->setMidiOption("ethernet", enabled, error);
    }
    if (name == "usbMidiPeripheral" || name == "usbDevice" || name == "usbMidiHost" || name == "usbHost" || name == "usbHid") {
        bool device = false;
        bool host = false;
        bool hidPort = false;
        if (enabled && (name == "usbMidiPeripheral" || name == "usbDevice")) {
            device = true;
        } else if (enabled && (name == "usbMidiHost" || name == "usbHost")) {
            host = true;
        } else if (enabled && name == "usbHid") {
            hidPort = true;
        }
        saveUsbMode(device, host, hidPort);
        scheduleReboot(enabled
            ? "The USB port switches after reboot. USB serial/JTAG turns off. OTA and the UART connector can still flash."
            : "Rebooting so the USB serial/JTAG port comes back.");
        return true;
    }
    String midiName = name;
    if (name == "bleMidiHost") midiName = "bleCentral";
    if (name == "bleMidiPeripheral") midiName = "bleAdvertise";
    if (midiName == "din" || midiName == "wifi" || midiName == "bleCentral" || midiName == "bleAdvertise") {
        return multiMidi->setMidiOption(midiName, enabled, error);
    }
    error = "Unknown setting.";
    return false;
}

void Connections::requestScan() {
    if (multiMidi != nullptr) multiMidi->requestBleScan();
}

bool Connections::connect(const String& address, const String& name, int midi, int hidRole, String& error) {
    if (midi < 0 && hidRole < 0) {
        error = "Choose MIDI, HID, or both.";
        return false;
    }
    if (!peers.upsert(address.c_str(), name.c_str(), midi, hidRole, -1, -1, 0)) {
        error = "Could not save that device.";
        return false;
    }
    if (midi == 1 && multiMidi != nullptr) {
        multiMidi->connectHost(address.c_str());
    }
    if (hidRole == 1) {
        hidEnabled = true;
        saveHid();
        hid.setEnabled(true);
    }
    return true;
}

bool Connections::disconnectAddress(const String& address, String& error) {
    if (multiMidi != nullptr) {
        multiMidi->holdHost(address.c_str());
    }
    bool dropped = false;
    if (multiMidi != nullptr && multiMidi->hostConnected() && sameAddress(multiMidi->hostAddress(), address.c_str())) {
        multiMidi->dropHostLink();
        dropped = true;
    }
    if (BleRadio::instance().disconnectIncoming(address.c_str())) {
        dropped = true;
    }
    if (!dropped && !peers.find(address.c_str(), nullptr)) {
        error = "That device is not connected.";
        return false;
    }
    return true;
}

bool Connections::forget(const String& address, String& error) {
    disconnectAddress(address, error);
    peers.forget(address.c_str());
    if (address.length() >= 11) {
        BLEAddress publicAddress(String(address.c_str()), BLE_ADDR_PUBLIC);
        BLEAddress randomAddress(String(address.c_str()), BLE_ADDR_RANDOM);
        ble_addr_t peer = {};
        peer.type = BLE_ADDR_PUBLIC;
        memcpy(peer.val, publicAddress.getNative(), sizeof(peer.val));
        ble_gap_unpair(&peer);
        peer.type = BLE_ADDR_RANDOM;
        memcpy(peer.val, randomAddress.getNative(), sizeof(peer.val));
        ble_gap_unpair(&peer);
    }
    return true;
}

bool Connections::setPeerAuto(const String& address, bool enabled, String& error) {
    BlePeers::Peer peer;
    if (!peers.find(address.c_str(), &peer) || peer.host || !peers.upsert(address.c_str(), "", -1, -1, enabled ? 1 : 0)) {
        error = "That peripheral is not saved.";
        return false;
    }
    if (multiMidi == nullptr) {
        return true;
    }
    if (enabled) {
        multiMidi->connectHost(address.c_str());
    } else {
        multiMidi->holdHost(address.c_str());
    }
    return true;
}

bool Connections::pair(const String& address, const String& name, String& error) {
    if (!peers.upsert(address.c_str(), name.c_str(), 1, -1, 1, 0, 0)) {
        error = "Could not save that device.";
        return false;
    }
    if (multiMidi != nullptr) {
        multiMidi->connectHost(address.c_str());
    }
    return true;
}

bool Connections::setPeerRoles(const String& address, int midi, int serialMidi, int hidRole, String& error) {
    if (serialMidi > 0) {
        error = "The ESP32-S3 has no Bluetooth Classic radio, so Serial MIDI stays off.";
        return false;
    }
    BlePeers::Peer peer;
    if (!peers.find(address.c_str(), &peer)) {
        error = "That device is not saved.";
        return false;
    }
    if (!peer.host && hidRole > 0) {
        error = "HID is for a computer in Paired hosts.";
        return false;
    }
    int serialFlag = serialMidi == 0 ? 0 : -1;
    if (!peers.upsert(address.c_str(), "", midi, hidRole, -1, serialFlag, -1)) {
        error = "Could not save that device.";
        return false;
    }
    if (!peer.host && multiMidi != nullptr) {
        if (midi == 1) {
            multiMidi->connectHost(address.c_str());
        } else if (midi == 0 && multiMidi->hostConnected() && sameAddress(multiMidi->hostAddress(), address.c_str())) {
            multiMidi->holdHost(address.c_str());
            multiMidi->dropHostLink();
        }
    }
    if (peer.host && hidRole == 0) {
        BleRadio::instance().disconnectIncoming(address.c_str());
    }
    if (peer.host && midi == 0) {
        BleRadio::instance().disconnectIncoming(address.c_str());
    }
    if (hidRole == 1 || (hidRole < 0 && peer.hid)) {
        if (peer.host && hidRole == 1) {
            hidEnabled = true;
            saveHid();
            hid.setEnabled(true);
        }
    }
    if (hidRole == 0 && peer.host && !anyHostHid()) {
        hidEnabled = false;
        saveHid();
        hid.setEnabled(false);
    }
    return true;
}

void Connections::scheduleReboot(const char* reason) {
    (void)reason;
    rebootPending = true;
    rebootAt = millis() + 1200;
}

bool Connections::anyHostHid() const {
    BlePeers::Peer saved[BlePeers::kMax];
    int count = peers.copy(saved, BlePeers::kMax);
    for (int i = 0; i < count; i++) {
        if (saved[i].host && saved[i].hid) {
            return true;
        }
    }
    return false;
}

bool Connections::queueKeys(const String& text, String& error) {
    if (UsbHid::running()) {
        if (!UsbHid::sendText(text.c_str())) {
            error = "The USB keyboard could not send that.";
            return false;
        }
        return true;
    }
    if (!hidEnabled) {
        error = "Turn on Bluetooth HID Peripheral first. A computer then connects to ShowboxMod.";
        return false;
    }
    if (!hid.ready()) {
        error = "No computer is connected yet.";
        return false;
    }
    if (!hid.queueText(text.c_str())) {
        error = "Those keys could not be queued.";
        return false;
    }
    return true;
}

bool Connections::queueMouse(int x, int y, int buttons, String& error) {
    int8_t dx = x > 127 ? 127 : (x < -127 ? -127 : (int8_t)x);
    int8_t dy = y > 127 ? 127 : (y < -127 ? -127 : (int8_t)y);
    uint8_t buttonBits = buttons < 0 ? 0 : (buttons > 7 ? 7 : (uint8_t)buttons);
    if (UsbHid::running()) {
        if (!UsbHid::sendMouse(dx, dy, buttonBits)) {
            error = "The USB mouse could not move.";
            return false;
        }
        return true;
    }
    if (!hidEnabled) {
        error = "Turn on Bluetooth HID Peripheral first. A computer then connects to ShowboxMod.";
        return false;
    }
    if (!hid.ready()) {
        error = "No computer is connected yet.";
        return false;
    }
    if (!hid.queueMouse(dx, dy, buttonBits)) {
        error = "The mouse action could not be queued.";
        return false;
    }
    return true;
}
