#include "Connections.h"
#include "FrontPanel/FrontPanel.h"
#include "MultiMidi/MultiMidi.h"
#include "MultiMidi/BleMidi.h"
#include "MackieShowbox/MackieShowbox.h"
#include <BleRadio.h>
#include <Hid.h>
#include "MultiMidi/UsbMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include <BLEAddress.h>
#include <BLEDevice.h>
#include <Preferences.h>
#include <host/ble_gap.h>
#include <host/ble_store.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdio>
#include "DebugLog.h"
#include "System/HttpsGate.h"
#include "System/LanKeep.h"

namespace {
const char* jsonBool(bool value) {
    return value ? "true" : "false";
}

struct JsonOut {
    char* start = nullptr;
    char* p = nullptr;
    char* end = nullptr;
    void begin(char* buf, size_t cap) {
        start = buf;
        p = buf;
        end = buf + cap;
        if (cap > 0) {
            buf[0] = '\0';
        }
    }
    void add(const char* s) {
        if (s == nullptr || p == nullptr || p + 1 >= end) {
            return;
        }
        while (*s != '\0' && p + 1 < end) {
            *p++ = *s++;
        }
        *p = '\0';
    }
    void addCh(char c) {
        if (p != nullptr && p + 1 < end) {
            *p++ = c;
            *p = '\0';
        }
    }
    void addEsc(const char* s) {
        if (s == nullptr) {
            return;
        }
        for (; *s != '\0'; s++) {
            if (*s == '"' || *s == '\\') {
                addCh('\\');
            }
            if (static_cast<unsigned char>(*s) >= 32) {
                addCh(*s);
            }
        }
    }
    void addInt(long v) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%ld", v);
        add(tmp);
    }
    void addU(unsigned v) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%u", v);
        add(tmp);
    }
    size_t len() const {
        return start == nullptr || p == nullptr ? 0 : static_cast<size_t>(p - start);
    }
};

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

bool incomingIs(const BleRadio::Incoming& incoming, const char* address) {
    if (address == nullptr || address[0] == '\0') {
        return false;
    }
    if (strcasecmp(incoming.address, address) == 0 || strcasecmp(incoming.otaAddress, address) == 0) {
        return true;
    }
    return incoming.idAddress[0] != '\0' && strcasecmp(incoming.idAddress, address) == 0;
}

// Android posts the current RPA; NVS is often the IRK identity after rekey.
bool resolveSavedPeer(BlePeers& peers, BleRadio* radio, const char* address, BlePeers::Peer* out) {
    if (address == nullptr || address[0] == '\0') {
        return false;
    }
    if (peers.find(address, out)) {
        return true;
    }
    if (radio == nullptr) {
        return false;
    }
    BleRadio::Incoming live[BleRadio::kMaxIncoming];
    int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
    for (int i = 0; i < liveCount; i++) {
        if (!incomingIs(live[i], address)) {
            continue;
        }
        const char* ota = live[i].otaAddress[0] != '\0' ? live[i].otaAddress : nullptr;
        if (peers.findMatch(live[i].address, ota, out)) {
            return true;
        }
        if (live[i].idAddress[0] != '\0' && peers.find(live[i].idAddress, out)) {
            return true;
        }
    }
    return false;
}

bool resolvablePrivate(const char* address) {
    if (address == nullptr || strlen(address) < 2) {
        return false;
    }
    char octet[3] = {address[0], address[1], 0};
    return (strtoul(octet, nullptr, 16) & 0xC0) == 0x40;
}

void unpairAddress(const char* address) {
    if (address == nullptr || strlen(address) < 11) {
        return;
    }
    BLEAddress publicAddress(String(address), BLE_ADDR_PUBLIC);
    BLEAddress randomAddress(String(address), BLE_ADDR_RANDOM);
    ble_addr_t peer = {};
    peer.type = BLE_ADDR_PUBLIC;
    memcpy(peer.val, publicAddress.getNative(), sizeof(peer.val));
    ble_gap_unpair(&peer);
    peer.type = BLE_ADDR_RANDOM;
    memcpy(peer.val, randomAddress.getNative(), sizeof(peer.val));
    ble_gap_unpair(&peer);
}

const char* transportNote(MultiMidi* midi, Hid* hid, const char* id, bool enabled) {
    if (strcmp(id, "ethernet") == 0 && midi != nullptr) {
        return midi->ethernetNote();
    }
    if ((strcmp(id, "usbMidiPeripheral") == 0 || strcmp(id, "usbMidiHost") == 0 || strcmp(id, "usbHid") == 0) && enabled) {
        if (strcmp(id, "usbMidiHost") == 0 && UsbMidiHost::running()) {
            return UsbMidiHost::status();
        }
        if (UsbMidi::running() || UsbMidiHost::running() || (hid != nullptr && hid->usbRunning())) {
            return "USB serial/JTAG is off. OTA and the UART connector can still flash.";
        }
        return "Rebooting. USB serial/JTAG turns off. OTA and the UART connector can still flash.";
    }
    return "";
}
}

void Connections::attach(MultiMidi* midi, BleRadio* bleRadio, Hid* hidDevice) {
    multiMidi = midi;
    radio = bleRadio;
    hid = hidDevice;
}

void Connections::setShowbox(MackieShowbox* box) {
    showbox = box;
}

void Connections::setDebugSettingsHandler(void (*handler)(bool wifiHistory, bool uartPackets)) {
    debugSettingsHandler = handler;
}

void Connections::setLinkChangeHandler(void (*handler)()) {
    linkChangeHandler = handler;
}

void Connections::setDebugStatus(bool connected, size_t bytes) {
    wifiLogConnected = connected;
    wifiLogBytes = bytes;
}

void Connections::loadDebug() {
    Preferences prefs;
    if (!prefs.begin("debug", true)) {
        return;
    }
    wifiLogHistory = prefs.getBool("wifibuf", true);
    uartPackets = prefs.getBool("uartpkt", false);
    prefs.end();
}

void Connections::applyDebug() {
    if (debugSettingsHandler != nullptr) {
        debugSettingsHandler(wifiLogHistory, uartPackets);
    }
}

void Connections::ensureHidGatt() {
    if (!hidEnabled || hid == nullptr || hid->bleEnabled()) {
        return;
    }
    hid->setBleEnabled(true);
}

void Connections::applyHidAdvertising() {
    if (radio == nullptr) {
        return;
    }
    radio->setHidPeripheral(hidEnabled && hid != nullptr && hid->bleEnabled());
    applyAdvertisingPolicy();
}

void Connections::applyAdvertisingPolicy() {
    if (radio == nullptr) {
        return;
    }
    if (!bluetoothRadioEnabled) {
        radio->setAdvertiseAllowed(false);
        radio->apply();
        return;
    }
    // Advertise while no host is linked so a remembered laptop can come
    // back, or while a pairing window is open for a new phone or tablet.
    // Stop once a host is up and the window is closed — the extra GAP
    // duty cycle is what used to stall mixer↔base.
    bool want = pairingWindowOpen() || !radio->hasIncoming();
    if (gIso == 10 || gIso == 12) {
        want = false;
    } else if (gIso == 11 || gIso == 13) {
        want = true;
    }
    bool pedal = multiMidi != nullptr && multiMidi->outgoingCount() > 0;
    radio->setPedalShare(pedal);
    // A live BLE central plus continuous ShowboxMod ads holds PTA so ARP/ICMP
    // die. Keep ads off until a pairing window, then pulse from BleRadio::apply.
    if (pedal && !pairingWindowOpen()) {
        want = false;
    }
    // Boot ads plus the AIRSTEP connect is what left STA associated with no
    // unicast. Wait for the pedal (or 20 s) before advertising for a laptop.
    if (!pairingWindowOpen() && !pedal && !radio->hasIncoming() && millis() < 20000) {
        want = false;
    }
    radio->setAdvertiseAllowed(want);
    radio->apply();
}

void Connections::load() {
    peers.load();
    loadDebug();
    {
        Preferences radioPrefs;
        if (radioPrefs.begin("radio", true)) {
            bluetoothRadioEnabled = radioPrefs.getBool("bt", true);
            radioPrefs.end();
        }
    }
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return;
    }
    hidEnabled = prefs.getBool("hid", false);
    usbDeviceEnabled = prefs.getBool("usbdev", false);
    usbHostEnabled = prefs.getBool("usbhost", false);
    usbHidEnabled = prefs.getBool("usbhid", false);
    usbHidMix = Hid::Mix::fromStored(prefs.getInt("usbhidmix", 0));
    int pairBits = prefs.getInt("blehidmix", 0);
    blePairMix = (pairBits & 0x10) != 0 ? Hid::Mix::fromStored(pairBits) : Hid::Mix::phone();
    hostSlots = prefs.getInt("hostSlots", 1);
    periSlots = prefs.getInt("periSlots", 2);
    if (hostSlots < 0) hostSlots = 0;
    if (hostSlots > BleRadio::kMaxIncoming) hostSlots = BleRadio::kMaxIncoming;
    if (periSlots < 0) periSlots = 0;
    if (periSlots > BleMidi::kMaxOutgoing) periSlots = BleMidi::kMaxOutgoing;
    if (hostSlots + periSlots > BleRadio::kMaxIncoming) {
        periSlots = BleRadio::kMaxIncoming - hostSlots;
    }
    bool autoOn = prefs.getBool("auto", true);
    String legacy = prefs.getString("bleaddr", "");
    prefs.end();
    if (multiMidi != nullptr) {
        multiMidi->setAutoConnect(autoOn, false);
    }
    if (peers.midiPeerCount() == 0 && legacy.length() >= 11) {
        peers.upsert(legacy.c_str(), "", 1, -1, 1);
    }
    applyDebug();
}

void Connections::startAfterBle() {
    BLEServer* server = multiMidi == nullptr ? nullptr : multiMidi->bleGattServer();
    if (radio != nullptr) {
        radio->attach(server);
        radio->setIncomingHandler(&Connections::incomingTrampoline, this);
    }
    if (hid != nullptr) {
        hid->attachBle(server);
        hid->setBleMix(blePairMix);
        hid->setBleEnabled(hidEnabled);
    }
    if (multiMidi != nullptr) {
        multiMidi->setMaxOutgoing(periSlots);
    }
    applyHidAdvertising();
    if (radio != nullptr) {
        radio->refreshLinkCount();
    }
}

void Connections::tick() {
    applyPending();
    if (hidEnabled && hid != nullptr && !hid->bleEnabled()) {
        bool pedal = multiMidi != nullptr && multiMidi->outgoingCount() > 0;
        // ble_gatts_add_svcs while AIRSTEP is up returns HCI timeout 15
        // and unicast dies. HID has to exist before the pedal connects.
        if (!pedal && (pairingWindowOpen() || millis() > 20000)) {
            ensureHidGatt();
            applyHidAdvertising();
        }
    }
    if (pairingUntilMs != 0 && !pairingWindowOpen()) {
        pairingUntilMs = 0;
        noteLog("Pairing window closed");
        applyAdvertisingPolicy();
    }
    bool uartHot = showbox != nullptr && showbox->uartHot();
    // JSON walk lives in flash on core 1 with the UART ISR. When the mixer
    // link is busy, wait 8s so we are not in flash on every other heartbeat.
    uint32_t jsonEvery = uartHot ? 8000 : 1500;
    if (lastStatusMs == 0 || (int32_t)(millis() - lastStatusMs) > (int32_t)jsonEvery) {
        refreshStatusCache();
    }
    enforcePeripheralSlots();
    int links = radio != nullptr ? radio->gapLinkCount() : 0;
    if (links != lastBleLinks) {
        lastBleLinks = links;
        if (linkChangeHandler != nullptr) {
            linkChangeHandler();
        }
    }
    bool settingsDirty = debugDirty || hidDirty || blePairDirty || usbDirty || slotsDirty;
    // Flash writes stall core 1 (UART + Arduino). Remembered-host NVS is
    // dirty-flagged, so a connected host no longer rewrites flash every 2 s.
    if (!uartHot && (settingsDirty || peers.needsPersist())) {
        if (showbox != nullptr) {
            showbox->waitQuiet(30);
        }
        peers.persist();
    if (debugDirty) {
        debugDirty = false;
        Preferences prefs;
        if (prefs.begin("debug", false)) {
            prefs.putBool("wifibuf", wifiLogHistory);
            prefs.putBool("uartpkt", uartPackets);
            prefs.end();
        }
    }
    if (hidDirty) {
        hidDirty = false;
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            prefs.putBool("hid", hidEnabled);
            prefs.end();
        }
    }
    if (blePairDirty) {
        blePairDirty = false;
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            prefs.putInt("blehidmix", blePairMix.storedBits());
            prefs.end();
        }
    }
    if (slotsDirty) {
        slotsDirty = false;
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            prefs.putInt("hostSlots", hostSlots);
            prefs.putInt("periSlots", periSlots);
            prefs.end();
        }
    }
    if (usbDirty) {
        usbDirty = false;
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            prefs.putBool("usbdev", usbDeviceEnabled);
            prefs.putBool("usbhost", usbHostEnabled);
            prefs.putBool("usbhid", usbHidEnabled);
            prefs.putInt("usbhidmix", usbHidMix.storedBits());
            prefs.putBool("usbtest", (usbDeviceEnabled || usbHostEnabled || usbHidEnabled) && usbSafeTest);
            if (usbDeviceEnabled || usbHostEnabled || usbHidEnabled) {
                prefs.putInt("usbfail", 0);
            }
            prefs.end();
        }
        if (multiMidi != nullptr) {
            multiMidi->rememberUsbDevice(usbDeviceEnabled);
        }
    }
    }
    if (midiTestOffAt != 0 && (int32_t)(millis() - midiTestOffAt) >= 0) {
        midiTestOffAt = 0;
        if (multiMidi != nullptr) {
            multiMidi->noteOff(midiTestNote, 0, midiTestChan);
        }
        noteLog("MIDI test note off");
    }
    if (rebootAt != 0 && (int32_t)(millis() - rebootAt) >= 0) {
        rebootAt = 0;
        // Drain the UART before restart: a blind reboot mid-transaction
        // wedges the Showbox base's UART state machine (soft-brick).
        if (showbox != nullptr) {
            if (showbox->uartHot()) {
                showbox->waitQuiet(40);
            }
        }
        delay(50);
        ESP.restart();
    }
    BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
    int incomingCount = radio == nullptr ? 0 : radio->copyIncoming(incoming, BleRadio::kMaxIncoming);
    if (incomingCount > 0) {
        if (lastHostRememberMs == 0 || (int32_t)(millis() - lastHostRememberMs) >= 2000) {
            lastHostRememberMs = millis();
            for (int i = 0; i < incomingCount; i++) {
                if (incomingIgnored(incoming[i])) {
                    // Leftover tablet bonds reconnect when the pairing window
                    // opens for the laptop. Skipping remember is not enough:
                    // they still occupy the only host ACL slot.
                    if (radio != nullptr) {
                        radio->disconnectIncoming(incoming[i].address);
                    }
                    continue;
                }
                if (!pairingWindowOpen() && !incomingSaved(incoming[i]) && !incoming[i].bonded) {
                    bool ready = (int32_t)(millis() - incoming[i].sinceMs) >= 4000;
                    if (ready && radio != nullptr) {
                        radio->disconnectIncoming(incoming[i].address);
                    }
                    continue;
                }
                rememberIncomingHost(incoming[i]);
            }
            enforceHostSlots();
        }
        if (hid != nullptr) {
            hid->tick();
            publishShowboxBattery();
        }
        return;
    }
    if (peers.needsPersist() && !uartHot) {
        peers.persist();
    }
    lastHostRememberMs = 0;
    if (lastPruneMs == 0 || (int32_t)(millis() - lastPruneMs) >= 5000) {
        lastPruneMs = millis();
        pruneGhostHosts();
    }
    if (multiMidi != nullptr) {
        noteScanFeatures();
    }
    if (hid != nullptr) {
        hid->tick();
        publishShowboxBattery();
    }
}

void Connections::publishShowboxBattery() {
    if (showbox == nullptr || hid == nullptr || !showbox->hasBatteryLevel()) {
        return;
    }
    float level = showbox->getBatteryLevel();
    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }
    hid->setBatteryPercent(static_cast<uint8_t>(level + 0.5f));
}

void Connections::noteScanFeatures() {
    BleMidi::SeenDevice seen[BleMidi::kMaxDevices];
    int seenCount = multiMidi->copyBleScan(seen, BleMidi::kMaxDevices);
    BlePeers::Peer saved[BlePeers::kMax];
    int savedCount = peers.copy(saved, BlePeers::kMax);
    for (int i = 0; i < seenCount; i++) {
        for (int j = 0; j < savedCount; j++) {
            if (!sameAddress(saved[j].address, seen[i].address) || saved[j].host) {
                continue;
            }
            if (seen[i].name[0] != '\0' && strlen(seen[i].name) > strlen(saved[j].name)) {
                peers.upsert(seen[i].address, seen[i].name, -1, -1, -1);
            }
            peers.noteAdvertised(seen[i].address, seen[i].midi, seen[i].hid, seen[i].battery);
        }
    }
}

bool Connections::shouldConnect(const char* name, const char* address) {
    return connectScore(name, address) > 0;
}

int Connections::connectScore(const char* name, const char* address) {
    (void)name;
    if (address == nullptr || !peers.matchesMidiAuto(address)) {
        return 0;
    }
    if (multiMidi != nullptr && multiMidi->addressConnected(address)) {
        return 0;
    }
    BlePeers::Peer peer;
    if (peers.find(address, &peer) && peer.pinned) {
        return 2;
    }
    return 1;
}

int Connections::maxOutgoing() {
    return periSlots;
}

bool Connections::wantsAutoScan() {
    // Remembered peripherals connect by address. Scanning is only for a
    // scan the user started from the page.
    return false;
}

bool Connections::nextAutoOutgoing(char* address, size_t addressSize, char* name, size_t nameSize) {
    if (address == nullptr || addressSize == 0) {
        return false;
    }
    if (outgoingUsed() >= periSlots) {
        return false;
    }
    BlePeers::Peer saved[BlePeers::kMax];
    int count = peers.copy(saved, BlePeers::kMax);
    int best = -1;
    int bestScore = 0;
    for (int i = 0; i < count; i++) {
        if (saved[i].host || !saved[i].midi || !saved[i].autoConnect) {
            continue;
        }
        if (multiMidi != nullptr && multiMidi->addressConnected(saved[i].address)) {
            continue;
        }
        int score = saved[i].pinned ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    strncpy(address, saved[best].address, addressSize - 1);
    address[addressSize - 1] = '\0';
    if (name != nullptr && nameSize > 0) {
        strncpy(name, saved[best].name, nameSize - 1);
        name[nameSize - 1] = '\0';
    }
    return true;
}

bool Connections::yieldOutgoing() {
    return dropNewestUnpinnedOutgoing();
}

void Connections::note(const char* text) {
    noteLog(text);
}

void Connections::incomingTrampoline(void* ctx, const char* address, uint16_t connId, bool connected) {
    static_cast<Connections*>(ctx)->onIncoming(address, connId, connected);
}

Hid::Mix Connections::mixFromPeer(const BlePeers::Peer& peer) const {
    if (!peer.hid) {
        return Hid::Mix{};
    }
    if (!peer.hidMixStored) {
        return Hid::Mix::desktop();
    }
    Hid::Mix mix;
    mix.keyboard = peer.hidKeyboard;
    mix.mouse = peer.hidMouse;
    mix.absPointer = peer.hidAbs;
    mix.digitizer = peer.hidDigitizer;
    return mix.any() ? mix : Hid::Mix::desktop();
}

void Connections::onIncoming(const char* address, uint16_t connId, bool connected) {
    if (!connected) {
        if (hid != nullptr) {
            hid->clearConnMix(connId);
        }
        applyPairingMixIfIdle();
        applyAdvertisingPolicy();
        return;
    }
    if (radio != nullptr) {
        BleRadio::Incoming live[BleRadio::kMaxIncoming];
        int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
        for (int i = 0; i < liveCount; i++) {
            if (incomingIs(live[i], address)) {
                rememberIncomingHost(live[i]);
                break;
            }
        }
    }
    // Do not close the pairing window on GAP-up. BlueZ and Android connect
    // first, then run SMP. Stopping ads here drops that pairing, especially
    // when the host is still on an RPA. Tick closes the window once bonded.
    if (hid == nullptr || address == nullptr) {
        return;
    }
    char otaBuf[18];
    char idBuf[18];
    strncpy(otaBuf, address, sizeof(otaBuf) - 1);
    otaBuf[sizeof(otaBuf) - 1] = '\0';
    strncpy(idBuf, address, sizeof(idBuf) - 1);
    idBuf[sizeof(idBuf) - 1] = '\0';
    if (radio != nullptr) {
        BleRadio::Incoming live[BleRadio::kMaxIncoming];
        int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
        for (int i = 0; i < liveCount; i++) {
            if (incomingIs(live[i], address)) {
                if (live[i].otaAddress[0] != '\0') {
                    strncpy(otaBuf, live[i].otaAddress, sizeof(otaBuf) - 1);
                    otaBuf[sizeof(otaBuf) - 1] = '\0';
                }
                if (live[i].idAddress[0] != '\0') {
                    strncpy(idBuf, live[i].idAddress, sizeof(idBuf) - 1);
                    idBuf[sizeof(idBuf) - 1] = '\0';
                }
                break;
            }
        }
    }
    BlePeers::Peer peer;
    Hid::Mix mix = hidEnabled ? blePairMix : Hid::Mix{};
    if (peers.findMatch(idBuf, otaBuf, &peer) || peers.find(address, &peer)) {
        mix = peer.hid ? mixFromPeer(peer) : Hid::Mix{};
    } else if (!hidEnabled) {
        mix = Hid::Mix{};
    }
    hid->setConnMix(connId, mix);
}

bool Connections::sameAddress(const char* left, const char* right) const {
    if (left == nullptr || right == nullptr || left[0] == '\0' || right[0] == '\0') {
        return false;
    }
    return strcasecmp(left, right) == 0;
}

void Connections::saveUsbMode(bool device, bool host, bool hidPort) {
    usbDeviceEnabled = device;
    usbHostEnabled = host;
    usbHidEnabled = hidPort;
    usbDirty = true;
}

void Connections::setUsbPortMode(UsbPortMode mode) {
    if (mode == UsbPortMode::Host) {
        saveUsbMode(false, true, false);
        scheduleReboot("USB host mode starts after reboot. USB serial/JTAG turns off.");
        return;
    }
    if (mode == UsbPortMode::Peripheral) {
        saveUsbMode(true, false, true);
        scheduleReboot("USB MIDI+HID peripheral starts after reboot. USB serial/JTAG turns off.");
        return;
    }
    saveUsbMode(false, false, false);
    scheduleReboot("Rebooting so the USB serial/JTAG port comes back.");
}

void Connections::setBluetoothRadio(bool on) {
    bluetoothRadioEnabled = on;
    Preferences prefs;
    if (prefs.begin("radio", false)) {
        prefs.putBool("bt", on);
        prefs.end();
    }
    if (!on) {
        closePairingWindow();
        if (radio != nullptr) {
            BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
            int count = radio->copyIncoming(incoming, BleRadio::kMaxIncoming);
            for (int i = 0; i < count; i++) {
                radio->disconnectIncoming(incoming[i].address);
            }
            radio->setAdvertiseAllowed(false);
            radio->apply();
        }
        if (multiMidi != nullptr) {
            multiMidi->allowOutgoing(false);
        }
        noteLog("Bluetooth radio off");
    } else {
        noteLog("Bluetooth radio on");
    }
    scheduleReboot(on ? "Rebooting to start the Bluetooth radio." : "Rebooting to stop the Bluetooth radio.");
}

void Connections::setFrontPanel(FrontPanel* next) {
    panel = next;
}

void Connections::saveHid() {
    hidDirty = true;
}

bool Connections::queueOption(const char* name, bool enabled) {
    if (name == nullptr || pendingCount >= kPendingMax) {
        return false;
    }
    strncpy(pendingOptions[pendingCount].name, name, sizeof(pendingOptions[0].name) - 1);
    pendingOptions[pendingCount].name[sizeof(pendingOptions[0].name) - 1] = '\0';
    pendingOptions[pendingCount].enabled = enabled;
    pendingCount++;
    return true;
}

bool Connections::queuePeerRoles(const String& address, int midi, int serialMidi, int hidRole,
        int hidKeyboard, int hidMouse, int hidAbs, int hidDigitizer, int pinned, String& error) {
    if (address.length() < 11) {
        error = "That device is not saved.";
        return false;
    }
    if (pendingRole) {
        error = "Busy, try again.";
        return false;
    }
    strncpy(pendingRoleAddress, address.c_str(), sizeof(pendingRoleAddress) - 1);
    pendingRoleAddress[sizeof(pendingRoleAddress) - 1] = '\0';
    pendingRoleMidi = midi;
    pendingRoleSerial = serialMidi;
    pendingRoleHid = hidRole;
    pendingRoleKb = hidKeyboard;
    pendingRoleMouse = hidMouse;
    pendingRoleAbs = hidAbs;
    pendingRoleDig = hidDigitizer;
    pendingRolePinned = pinned;
    pendingRole = true;
    return true;
}

bool Connections::queueSlots(int hosts, int peripherals, String& error) {
    if (pendingHostSlots >= 0 || pendingPeriSlots >= 0) {
        error = "Busy, try again.";
        return false;
    }
    pendingHostSlots = hosts;
    pendingPeriSlots = peripherals;
    return true;
}

void Connections::applyPending() {
    if (pendingCount > 0) {
        PendingOption local[kPendingMax];
        int n = pendingCount;
        for (int i = 0; i < n; i++) {
            local[i] = pendingOptions[i];
        }
        pendingCount = 0;
        for (int i = 0; i < n; i++) {
            String error;
            setOption(local[i].name, local[i].enabled, error);
        }
    }
    if (pendingRole) {
        char address[18];
        strncpy(address, pendingRoleAddress, sizeof(address) - 1);
        address[sizeof(address) - 1] = '\0';
        int midi = pendingRoleMidi;
        int serial = pendingRoleSerial;
        int hidRole = pendingRoleHid;
        int kb = pendingRoleKb;
        int mouse = pendingRoleMouse;
        int absPointer = pendingRoleAbs;
        int dig = pendingRoleDig;
        int pinned = pendingRolePinned;
        pendingRole = false;
        String error;
        setPeerRoles(address, midi, serial, hidRole, kb, mouse, absPointer, dig, pinned, error);
    }
    if (pendingHostSlots >= 0 || pendingPeriSlots >= 0) {
        int hosts = pendingHostSlots;
        int peri = pendingPeriSlots;
        pendingHostSlots = -1;
        pendingPeriSlots = -1;
        String error;
        setSlots(hosts, peri, error);
        if (error.length() > 0) {
            noteLog(error.c_str());
        }
    }
    if (pendingForget) {
        char address[18];
        strncpy(address, pendingForgetAddress, sizeof(address) - 1);
        address[sizeof(address) - 1] = '\0';
        pendingForget = false;
        applyForget(address);
    }
}

bool Connections::setUsbHidPart(const String& name, bool enabled, String& error) {
    Hid::Mix next = usbHidMix;
    if (name == "usbHidKeyboard") {
        next.keyboard = enabled;
    } else if (name == "usbHidMouse") {
        next.mouse = enabled;
    } else if (name == "usbHidAbs") {
        next.absPointer = enabled;
    } else if (name == "usbHidDigitizer") {
        next.digitizer = enabled;
    }
    if (!next.any()) {
        error = "Choose at least one USB HID input.";
        return false;
    }
    usbHidMix = next;
    usbDirty = true;
    if (usbHidEnabled) {
        scheduleReboot("Rebooting so the USB HID layout is applied. Forget is not needed for USB; unplug/replug after reboot if the computer keeps the old layout.");
    }
    return true;
}

bool Connections::setBleHidPart(const String& name, bool enabled, String& error) {
    Hid::Mix next = blePairMix;
    if (name == "bleHidKeyboard") {
        next.keyboard = enabled;
    } else if (name == "bleHidMouse") {
        next.mouse = enabled;
    } else if (name == "bleHidAbs") {
        next.absPointer = enabled;
    } else if (name == "bleHidDigitizer") {
        next.digitizer = enabled;
    }
    if (!next.any()) {
        error = "Choose at least one Bluetooth HID input for new hosts.";
        return false;
    }
    Hid::Mix before = blePairMix;
    blePairMix = next;
    blePairDirty = true;
    if (!before.asBleMap().same(next.asBleMap())) {
        advertisePairingMix();
    }
    return true;
}

void Connections::advertisePairingMix() {
    if (hid == nullptr) {
        return;
    }
    // Live incoming host: keep the map that host already cached.
    if (radio != nullptr && radio->hasIncoming()) {
        return;
    }
    if (hid->bleMix().asBleMap().same(blePairMix.asBleMap())) {
        return;
    }
    hid->setBleMix(blePairMix);
}

void Connections::applyPairingMixIfIdle() {
    if (hid == nullptr) {
        return;
    }
    if (radio != nullptr && radio->hasIncoming()) {
        return;
    }
    // A remembered HID host keeps the report map it last selected. Rewriting
    // it to blePairMix on a transient drop makes BlueZ reconnect with a
    // stale cache (abs after-reconnect would snap back to kb+mouse).
    if (anyHostHid()) {
        return;
    }
    hid->setBleMix(blePairMix);
}

void Connections::bleConnectedNames(char* out, size_t cap) const {
    if (cap == 0) return;
    out[0] = '\0';
    if (radio == nullptr) return;
    BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
    int count = radio->copyIncoming(incoming, BleRadio::kMaxIncoming);
    size_t pos = 0;
    for (int i = 0; i < count && pos + 2 < cap; i++) {
        const char* name = incoming[i].name;
        if (name == nullptr || name[0] == '\0') {
            name = incoming[i].address;
        }
        if (name == nullptr || name[0] == '\0') continue;
        if (pos > 0 && pos + 2 < cap) { out[pos++] = ','; out[pos] = '\0'; }
        size_t nlen = strlen(name);
        if (pos + nlen >= cap) nlen = cap - pos - 1;
        memcpy(out + pos, name, nlen);
        pos += nlen;
        out[pos] = '\0';
    }
}

void Connections::rememberIncomingHost(const BleRadio::Incoming& incoming) {
    const char* ota = incoming.otaAddress[0] != '\0' ? incoming.otaAddress : nullptr;
    const char* id = incoming.idAddress[0] != '\0' ? incoming.idAddress : nullptr;
    const char* key = incoming.address;
    const char* hostName = incoming.name;
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    BlePeers::Peer existing;
    bool known = peers.findMatch(key, ota, &existing);
    if (!known && id != nullptr) {
        known = peers.find(id, &existing);
    }
    if (known) {
        if (id != nullptr && strcasecmp(existing.address, id) != 0) {
            peers.rekey(existing.address, id);
            key = id;
            peers.find(id, &existing);
        } else {
            key = existing.address;
        }
        // One tablet can land as both IRK identity and a leftover RPA.
        // findMatch hits identity first and leaves the RPA mix in NVS, so
        // liveHidMix stays digitizer after a kb+mouse role POST.
        if (id != nullptr && ota != nullptr && strcasecmp(ota, id) != 0) {
            peers.rekey(ota, id);
            peers.find(id, &existing);
        }
        if (hostName[0] != '\0' && strcasecmp(existing.name, hostName) != 0) {
            peers.upsert(key, hostName, -1, -1, -1);
        }
        if (existing.name[0] != '\0' && radio != nullptr) {
            radio->preferIncomingName(key, existing.name);
        }
        if (!existing.host) {
            peers.upsert(key, "", -1, hidEnabled && !existing.hid ? 1 : -1, -1, -1, 1);
        }
        if (hidEnabled && existing.host && !existing.hidMixStored) {
            peers.setHidMix(key, blePairMix.keyboard, blePairMix.mouse, blePairMix.absPointer, blePairMix.digitizer);
        }
        return;
    }
    if (!peers.upsert(key, hostName, 1, hidEnabled ? 1 : 0, 0, 0, 1)) {
        pruneGhostHosts();
        if (!peers.upsert(key, hostName, 1, hidEnabled ? 1 : 0, 0, 0, 1)) {
            return;
        }
    }
    if (hidEnabled) {
        peers.setHidMix(key, blePairMix.keyboard, blePairMix.mouse, blePairMix.absPointer, blePairMix.digitizer);
    }
    if (id != nullptr && ota != nullptr && strcasecmp(ota, id) != 0) {
        peers.rekey(ota, id);
    }
}

int Connections::outgoingUsed() const {
    return multiMidi == nullptr ? 0 : multiMidi->outgoingCount();
}

bool Connections::peerPinned(const char* address) const {
    BlePeers::Peer peer;
    return peers.findMatch(address, nullptr, &peer) && peer.pinned;
}

void Connections::enforceHostSlots() {
    if (radio == nullptr) {
        return;
    }
    BleRadio::Incoming live[BleRadio::kMaxIncoming];
    int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
    struct Rank {
        char address[18];
        uint32_t sinceMs;
        bool pinned;
        bool ready;
    };
    Rank keep[BleRadio::kMaxIncoming];
    int n = 0;
    uint32_t now = millis();
    for (int i = 0; i < liveCount; i++) {
        if (incomingIgnored(live[i])) {
            continue;
        }
        Rank& row = keep[n++];
        strncpy(row.address, live[i].address, sizeof(row.address) - 1);
        row.address[sizeof(row.address) - 1] = '\0';
        row.sinceMs = live[i].sinceMs;
        row.pinned = peerPinned(live[i].address) || peerPinned(live[i].idAddress) || peerPinned(live[i].otaAddress);
        row.ready = live[i].bonded || (int32_t)(now - live[i].sinceMs) >= 2000;
    }
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            bool better = keep[j].pinned && !keep[best].pinned;
            if (!better && keep[j].pinned == keep[best].pinned && keep[j].sinceMs < keep[best].sinceMs) {
                better = true;
            }
            if (better) {
                best = j;
            }
        }
        if (best != i) {
            Rank tmp = keep[i];
            keep[i] = keep[best];
            keep[best] = tmp;
        }
    }
    for (int i = hostSlots; i < n; i++) {
        if (!keep[i].ready) {
            continue;
        }
        char line[88];
        snprintf(line, sizeof(line), "Host slot full, dropped %s", keep[i].address);
        noteLog(line);
        radio->disconnectIncoming(keep[i].address);
    }
}

bool Connections::dropNewestUnpinnedOutgoing() {
    if (multiMidi == nullptr) {
        return false;
    }
    BleMidi::Outgoing live[BleMidi::kMaxOutgoing];
    int n = multiMidi->copyOutgoing(live, BleMidi::kMaxOutgoing);
    int newest = -1;
    for (int i = 0; i < n; i++) {
        if (!live[i].connected || peerPinned(live[i].address)) {
            continue;
        }
        if (newest < 0 || live[i].sinceMs >= live[newest].sinceMs) {
            newest = i;
        }
    }
    if (newest < 0) {
        return false;
    }
    char line[88];
    snprintf(line, sizeof(line), "Peripheral slot reserved, dropped %s", live[newest].address);
    noteLog(line);
    multiMidi->dropHost(live[newest].address);
    return true;
}

void Connections::enforcePeripheralSlots() {
    if (multiMidi == nullptr) {
        return;
    }
    BleMidi::Outgoing live[BleMidi::kMaxOutgoing];
    int n = multiMidi->copyOutgoing(live, BleMidi::kMaxOutgoing);
    struct Rank {
        char address[18];
        uint32_t sinceMs;
        bool pinned;
    };
    Rank keep[BleMidi::kMaxOutgoing];
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!live[i].connected) {
            continue;
        }
        Rank& row = keep[count++];
        strncpy(row.address, live[i].address, sizeof(row.address) - 1);
        row.address[sizeof(row.address) - 1] = '\0';
        row.sinceMs = live[i].sinceMs;
        row.pinned = peerPinned(live[i].address);
    }
    for (int i = 0; i < count; i++) {
        int best = i;
        for (int j = i + 1; j < count; j++) {
            bool better = keep[j].pinned && !keep[best].pinned;
            if (!better && keep[j].pinned == keep[best].pinned && keep[j].sinceMs < keep[best].sinceMs) {
                better = true;
            }
            if (better) {
                best = j;
            }
        }
        if (best != i) {
            Rank tmp = keep[i];
            keep[i] = keep[best];
            keep[best] = tmp;
        }
    }
    for (int i = periSlots; i < count; i++) {
        char line[88];
        snprintf(line, sizeof(line), "Peripheral slot full, dropped %s", keep[i].address);
        noteLog(line);
        multiMidi->dropHost(keep[i].address);
    }
}

bool Connections::setSlots(int hosts, int peripherals, String& error) {
    if (hosts < 0 || peripherals < 0 || hosts > BleRadio::kMaxIncoming || peripherals > BleMidi::kMaxOutgoing) {
        error = "Host slots are 0–3. Peripheral slots are 0–2.";
        return false;
    }
    if (hosts + peripherals > BleRadio::kMaxIncoming) {
        error = "Host and peripheral slots together cannot exceed 3 live BLE links.";
        return false;
    }
    if (peers.pinnedCount(true) > hosts) {
        error = "Unpin a host first. Reserved hosts cannot exceed host slots.";
        return false;
    }
    if (peers.pinnedCount(false) > peripherals) {
        error = "Unpin a peripheral first. Reserved peripherals cannot exceed peripheral slots.";
        return false;
    }
    hostSlots = hosts;
    periSlots = peripherals;
    slotsDirty = true;
    if (multiMidi != nullptr) {
        multiMidi->setMaxOutgoing(periSlots);
    }
    enforceHostSlots();
    enforcePeripheralSlots();
    return true;
}

void Connections::pruneGhostHosts() {
    if (radio == nullptr || !BLEDevice::getInitialized()) {
        return;
    }
    BleRadio::Incoming live[BleRadio::kMaxIncoming];
    int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
    if (liveCount > 0) {
        return;
    }
    ble_addr_t bonded[BlePeers::kMax];
    int bondedCount = BlePeers::kMax;
    if (ble_store_util_bonded_peers(bonded, &bondedCount, BlePeers::kMax) != 0) {
        return;
    }
    BlePeers::Peer saved[BlePeers::kMax];
    int savedCount = peers.copy(saved, BlePeers::kMax);
    for (int i = 0; i < savedCount; i++) {
        if (!saved[i].host) {
            continue;
        }
        bool connected = false;
        for (int j = 0; j < liveCount; j++) {
            if (incomingIs(live[j], saved[i].address)) {
                connected = true;
                break;
            }
        }
        if (connected) {
            continue;
        }
        bool bondedPeer = false;
        for (int j = 0; j < bondedCount; j++) {
            String text = BLEAddress(bonded[j]).toString();
            if (sameAddress(saved[i].address, text.c_str())) {
                bondedPeer = true;
                break;
            }
        }
        if (bondedPeer && !resolvablePrivate(saved[i].address)) {
            continue;
        }
        // Keep a desktop public address and an Android static identity.
        // Drop rotating resolvable-private addresses left by previous pairs.
        if (saved[i].hidMixStored && !resolvablePrivate(saved[i].address)) {
            continue;
        }
        unpairAddress(saved[i].address);
        peers.forget(saved[i].address);
    }
}

void Connections::refreshStatusCache() {
    bool empty = statusBuf[statusReady] == nullptr || statusLen[statusReady] == 0;
    uint32_t age = lastStatusMs == 0 ? 0xFFFFFFFFu : (uint32_t)(millis() - lastStatusMs);
    int links = radio != nullptr ? radio->gapLinkCount() : 0;
    int out = outgoingUsed();
    bool force = empty || links != lastCacheLinks || out != lastCacheOut;
    if (!empty && !force && age < 400) {
        return;
    }
    int write = statusReady == 0 ? 1 : 0;
    if (statusBuf[write] == nullptr) {
        statusBuf[write] = static_cast<char*>(heap_caps_malloc(kStatusCacheSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (statusBuf[write] == nullptr) {
            return;
        }
    }
    size_t n = 0;
    writeStatusJson(statusBuf[write], kStatusCacheSize, &n);
    if (n == 0) {
        return;
    }
    statusLen[write] = n;
    statusReady = write;
    lastStatusMs = millis();
    lastCacheLinks = links;
    lastCacheOut = out;
}

const uint8_t* Connections::statusCacheBytes(size_t* len) const {
    int read = statusReady;
    if (statusBuf[read] == nullptr || statusLen[read] == 0) {
        if (len != nullptr) {
            *len = 0;
        }
        return nullptr;
    }
    if (len != nullptr) {
        *len = statusLen[read];
    }
    return reinterpret_cast<const uint8_t*>(statusBuf[read]);
}

String Connections::statusJson() {
    char* buf = static_cast<char*>(heap_caps_malloc(kStatusCacheSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        return String();
    }
    size_t n = 0;
    writeStatusJson(buf, kStatusCacheSize, &n);
    String json;
    if (n > 0) {
        json.concat(buf);
    }
    heap_caps_free(buf);
    return json;
}

void Connections::writeStatusJson(char* buf, size_t cap, size_t* outLen) {
    JsonOut json;
    json.begin(buf, cap);
    json.add("{\"reboot\":");
    json.add(jsonBool(rebootPending));
    json.add(",\"discoverable\":");
    json.add(jsonBool(multiMidi != nullptr && multiMidi->blePeripheralEnabled()));
    json.add(",\"pairingOpen\":");
    json.add(jsonBool(pairingWindowOpen() || !bleIncomingOn()));
    json.add(",\"acceptNewHosts\":");
    json.add(jsonBool(pairingWindowOpen() || !bleIncomingOn()));
    json.add(",\"pairingMs\":");
    json.addU(pairingMsLeft());
    json.add(",\"advertising\":");
    json.add(jsonBool(radio != nullptr && radio->advertiseAllowed()));
    json.add(",\"usbJtag\":");
    json.add(jsonBool(!usbDeviceEnabled && !usbHostEnabled && !usbHidEnabled));
    json.add(",\"bluetoothRadio\":");
    json.add(jsonBool(bluetoothRadioEnabled));
    json.add(",\"wifiRadio\":");
    json.add(jsonBool(panel != nullptr ? panel->wifiRadioOn() : true));
    json.add(",\"https\":");
    json.add(jsonBool(httpsWanted()));
    json.add(",\"httpsListening\":");
    json.add(jsonBool(httpsListening()));
    json.add(",\"httpsHasCa\":");
    json.add(jsonBool(httpsCaPem() != nullptr && httpsCaPem()[0] != '\0'));
    json.add(",\"httpsUrl\":\"");
    json.addEsc(httpsListening() ? "https://showbox.local/" : "");
    json.add("\"");
    json.add(",\"scanning\":");
    json.add(jsonBool(multiMidi != nullptr && multiMidi->bleScanning()));
    json.add(",\"wsLive\":");
    json.add(jsonBool(lanWsLive()));
    json.add(",\"holdMidi\":");
    json.add(jsonBool(lanHoldMidi()));
    json.add(",\"hidReady\":");
    json.add(jsonBool(hid != nullptr && hid->ready()));
    json.add(",\"ip\":\"");
    json.addEsc(WiFi.localIP().toString().c_str());
    json.add("\"");
    json.add(",\"usbHidMix\":{\"keyboard\":");
    json.add(jsonBool(usbHidMix.keyboard));
    json.add(",\"mouse\":");
    json.add(jsonBool(usbHidMix.mouse));
    json.add(",\"absPointer\":");
    json.add(jsonBool(usbHidMix.absPointer));
    json.add(",\"digitizer\":");
    json.add(jsonBool(usbHidMix.digitizer));
    json.add("}");
    json.add(",\"bleHidMix\":{\"keyboard\":");
    json.add(jsonBool(blePairMix.keyboard));
    json.add(",\"mouse\":");
    json.add(jsonBool(blePairMix.mouse));
    json.add(",\"absPointer\":");
    json.add(jsonBool(blePairMix.absPointer));
    json.add(",\"digitizer\":");
    json.add(jsonBool(blePairMix.digitizer));
    json.add("}");
    json.add(",\"liveHidMix\":{\"keyboard\":");
    json.add(jsonBool(hid != nullptr && hid->activeMix().keyboard));
    json.add(",\"mouse\":");
    json.add(jsonBool(hid != nullptr && hid->activeMix().mouse));
    json.add(",\"absPointer\":");
    json.add(jsonBool(hid != nullptr && hid->activeMix().absPointer));
    json.add(",\"digitizer\":");
    json.add(jsonBool(hid != nullptr && hid->activeMix().digitizer));
    json.add("}");
    json.add(",\"bleLinkLimit\":");
    json.addInt(BleRadio::kMaxIncoming);
    json.add(",\"bleLinkCount\":");
    json.addInt(radio != nullptr ? radio->gapLinkCount() : 0);
    json.add(",\"bleSlots\":{\"hostMax\":");
    json.addInt(hostSlots);
    json.add(",\"peripheralMax\":");
    json.addInt(periSlots);
    json.add(",\"hostUsed\":");
    {
        BleRadio::Incoming live[BleRadio::kMaxIncoming];
        int liveCount = radio == nullptr ? 0 : radio->copyIncoming(live, BleRadio::kMaxIncoming);
        int used = 0;
        for (int i = 0; i < liveCount; i++) {
            if (!incomingIgnored(live[i])) {
                used++;
            }
        }
        json.addInt(used);
    }
    json.add(",\"peripheralUsed\":");
    json.addInt(outgoingUsed());
    json.add("}");
    json.add(",\"scanHeld\":");
    json.add(jsonBool(bleIncomingOn()));
    json.add(",\"showboxLink\":{\"ok\":");
    json.add(jsonBool(showbox != nullptr && showbox->linkOk()));
    json.add(",\"ageMs\":");
    json.addU(showbox != nullptr ? showbox->linkAgeMs() : 0xFFFFFFFFu);
    json.add(",\"heartbeatAgeMs\":");
    json.addU(showbox != nullptr ? showbox->heartbeatAgeMs() : 0xFFFFFFFFu);
    json.add(",\"baseAgeMs\":");
    json.addU(showbox != nullptr ? showbox->baseAgeMs() : 0xFFFFFFFFu);
    json.add(",\"mixerAgeMs\":");
    json.addU(showbox != nullptr ? showbox->mixerAgeMs() : 0xFFFFFFFFu);
    json.add(",\"parsing\":");
    json.add(jsonBool(showbox != nullptr && showbox->parsing()));
    json.add("}");
    if (showbox != nullptr && showbox->hasBatteryLevel()) {
        float level = showbox->getBatteryLevel();
        if (level < 0) {
            level = 0;
        }
        if (level > 100) {
            level = 100;
        }
        json.add(",\"showboxBattery\":");
        json.addInt(static_cast<int>(level + 0.5f));
    }
    json.add(",\"transports\":[");
    bool first = true;
    for (const TransportRow& row : kTransports) {
        if (!first) {
            json.add(",");
        }
        first = false;
        bool enabled = false;
        if (strcmp(row.id, "bleHid") == 0) {
            enabled = hidEnabled;
        } else if (row.available && multiMidi != nullptr) {
            enabled = transportEnabled(this, multiMidi, row.id);
        }
        json.add("{\"id\":\"");
        json.add(row.id);
        json.add("\",\"enabled\":");
        json.add(jsonBool(enabled));
        json.add(",\"available\":");
        json.add(jsonBool(row.available));
        json.add(",\"reason\":\"");
        json.addEsc(row.reason == nullptr ? "" : row.reason);
        json.add("\",\"note\":\"");
        json.addEsc(transportNote(multiMidi, hid, row.id, enabled));
        json.add("\"}");
    }

    json.add("],\"debug\":{");
    json.add("\"wifiLogHistory\":");
    json.add(jsonBool(wifiLogHistory));
    json.add(",\"uartPackets\":");
    json.add(jsonBool(uartPackets));
    json.add(",\"wifiLogConnected\":");
    json.add(jsonBool(wifiLogConnected));
    json.add(",\"wifiLogBytes\":");
    json.addU(static_cast<unsigned>(wifiLogBytes));
    json.add(",\"usbSafeTest\":");
    json.add(jsonBool(usbSafeTest));
    json.add("}");

    BlePeers::Peer saved[BlePeers::kMax];
    int savedCount = peers.copy(saved, BlePeers::kMax);
    BleMidi::Outgoing outgoing[BleMidi::kMaxOutgoing];
    int outgoingCount = multiMidi == nullptr ? 0 : multiMidi->copyOutgoing(outgoing, BleMidi::kMaxOutgoing);
    auto outgoingOf = [&](const char* address) -> const BleMidi::Outgoing* {
        for (int i = 0; i < outgoingCount; i++) {
            if (sameAddress(outgoing[i].address, address)) {
                return &outgoing[i];
            }
        }
        return nullptr;
    };
    BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
    int incomingCount = radio == nullptr ? 0 : radio->copyIncoming(incoming, BleRadio::kMaxIncoming);
    auto knownAddress = [&](const char* address) {
        for (int i = 0; i < savedCount; i++) {
            if (sameAddress(saved[i].address, address)) {
                return true;
            }
        }
        return false;
    };
    auto incomingMatch = [&](const char* address) {
        for (int i = 0; i < incomingCount; i++) {
            if (incomingIs(incoming[i], address)) {
                return true;
            }
        }
        return false;
    };
    auto hiddenPeer = [&](const char* address) {
        if (addressIgnored(address) || (pendingForget && sameAddress(pendingForgetAddress, address))) {
            return true;
        }
        for (int i = 0; i < incomingCount; i++) {
            if (incomingIgnored(incoming[i]) && incomingIs(incoming[i], address)) {
                return true;
            }
        }
        return false;
    };
    BleMidi::SeenDevice seen[BleMidi::kMaxDevices];
    int seenCount = multiMidi == nullptr ? 0 : multiMidi->copyBleScan(seen, BleMidi::kMaxDevices);
    auto seenOf = [&](const char* address) -> const BleMidi::SeenDevice* {
        for (int i = 0; i < seenCount; i++) {
            if (sameAddress(seen[i].address, address)) {
                return &seen[i];
            }
        }
        return nullptr;
    };
    auto appendPeer = [&](const char* name, const char* address, bool midi, bool hidRole, Hid::Mix mix, bool autoOn, bool connected,
            bool advertisedHid, bool advertisedBattery, bool haveRssi, int rssi, int battery, bool pinned) {
        json.add("{\"name\":\"");
        json.addEsc(name);
        json.add("\",\"address\":\"");
        json.addEsc(address);
        json.add("\",\"midi\":");
        json.add(jsonBool(midi));
        json.add(",\"hid\":");
        json.add(jsonBool(hidRole));
        json.add(",\"hidKeyboard\":");
        json.add(jsonBool(mix.keyboard));
        json.add(",\"hidMouse\":");
        json.add(jsonBool(mix.mouse));
        json.add(",\"hidAbs\":");
        json.add(jsonBool(mix.absPointer));
        json.add(",\"hidDigitizer\":");
        json.add(jsonBool(mix.digitizer));
        json.add(",\"advertisedHid\":");
        json.add(jsonBool(advertisedHid));
        json.add(",\"advertisedBattery\":");
        json.add(jsonBool(advertisedBattery));
        json.add(",\"autoConnect\":");
        json.add(jsonBool(autoOn));
        json.add(",\"pinned\":");
        json.add(jsonBool(pinned));
        json.add(",\"connected\":");
        json.add(jsonBool(connected));
        if (haveRssi) {
            json.add(",\"rssi\":");
            json.addInt(rssi);
        }
        if (battery >= 0) {
            json.add(",\"battery\":");
            json.addInt(battery);
        }
        json.add("}");
    };

    json.add(",\"nearby\":[");
    bool anyNearby = false;
    for (int i = 0; i < seenCount; i++) {
        if (knownAddress(seen[i].address)) {
            continue;
        }
        if (anyNearby) {
            json.add(",");
        }
        anyNearby = true;
        json.add("{\"name\":\"");
        json.addEsc(seen[i].name);
        json.add("\",\"address\":\"");
        json.addEsc(seen[i].address);
        json.add("\",\"rssi\":");
        json.addInt(seen[i].rssi);
        json.add(",\"midi\":");
        json.add(jsonBool(seen[i].midi));
        json.add(",\"hid\":");
        json.add(jsonBool(seen[i].hid));
        json.add(",\"advertisedBattery\":");
        json.add(jsonBool(seen[i].battery));
        json.add("}");
    }
    json.add("],\"peripherals\":[");
    bool anyPeripheral = false;
    for (int i = 0; i < savedCount; i++) {
        if (saved[i].host) {
            continue;
        }
        if (hiddenPeer(saved[i].address)) {
            continue;
        }
        if (anyPeripheral) {
            json.add(",");
        }
        anyPeripheral = true;
        bool live = false;
        const BleMidi::Outgoing* liveOut = outgoingOf(saved[i].address);
        if (liveOut != nullptr && liveOut->connected) {
            live = true;
        }
        const BleMidi::SeenDevice* found = seenOf(saved[i].address);
        bool haveRssi = false;
        int rssi = 0;
        int battery = -1;
        if (live && liveOut != nullptr && liveOut->haveRssi) {
            haveRssi = true;
            rssi = liveOut->rssi;
        } else if (found != nullptr && found->rssi != 0) {
            haveRssi = true;
            rssi = found->rssi;
        }
        if (live && liveOut != nullptr) {
            battery = liveOut->battery;
        }
        bool advertisedHid = saved[i].advertisedHid || (found != nullptr && found->hid);
        bool advertisedBattery = saved[i].advertisedBattery || (found != nullptr && found->battery) || battery >= 0;
        appendPeer(saved[i].name, saved[i].address, saved[i].midi || (found != nullptr && found->midi), saved[i].hid,
            mixFromPeer(saved[i]), saved[i].autoConnect, live, advertisedHid, advertisedBattery, haveRssi, rssi, battery, saved[i].pinned);
    }
    json.add("],\"hosts\":[");
    bool anyHost = false;
    for (int i = 0; i < savedCount; i++) {
        if (!saved[i].host) {
            continue;
        }
        if (hiddenPeer(saved[i].address)) {
            continue;
        }
        if (anyHost) {
            json.add(",");
        }
        anyHost = true;
        const char* hostName = saved[i].name;
        for (int j = 0; j < incomingCount; j++) {
            if (incomingIs(incoming[j], saved[i].address) && incoming[j].name[0] != '\0') {
                hostName = incoming[j].name;
                break;
            }
        }
        appendPeer(hostName, saved[i].address, saved[i].midi, saved[i].hid, mixFromPeer(saved[i]), saved[i].autoConnect,
            incomingMatch(saved[i].address), false, false, false, 0, -1, saved[i].pinned);
    }
    for (int i = 0; i < incomingCount; i++) {
        if (incomingIgnored(incoming[i]) || !pairingWindowOpen() || knownAddress(incoming[i].address) || knownAddress(incoming[i].otaAddress)
            || (incoming[i].idAddress[0] != '\0' && knownAddress(incoming[i].idAddress))) {
            continue;
        }
        if (anyHost) {
            json.add(",");
        }
        anyHost = true;
        appendPeer(incoming[i].name, incoming[i].address, true, hidEnabled, hidEnabled ? blePairMix : Hid::Mix{}, false, true, false, false, false, 0, -1, false);
    }
    json.add("],\"log\":[");
    int logged = 0;
    int start = logCount == kLogMax ? logNext : 0;
    for (int i = 0; i < logCount; i++) {
        int idx = (start + i) % kLogMax;
        if (logged) {
            json.add(",");
        }
        json.add("\"");
        json.addEsc(logLine[idx]);
        json.add("\"");
        logged++;
    }
    json.add("]}");
    if (outLen != nullptr) {
        *outLen = json.len();
    }
}

bool Connections::setOption(const String& name, bool enabled, String& error) {
    const char* blocked = blockedReason(name);
    if (blocked != nullptr) {
        error = blocked;
        return false;
    }
    if (name == "wifiLogHistory") {
        wifiLogHistory = enabled;
        debugDirty = true;
        applyDebug();
        return true;
    }
    if (name == "uartPackets") {
        uartPackets = enabled;
        debugDirty = true;
        applyDebug();
        return true;
    }
    if (name == "usbSafeTest") {
        usbSafeTest = enabled;
        return true;
    }
    if (name == "acceptNewHosts") {
        setAllowNewHosts(enabled);
        return true;
    }
    if (name == "interceptor") {
        if (showbox == nullptr) {
            error = "The mixer link is not available.";
            return false;
        }
        showbox->setParsing(enabled);
        noteLog(enabled ? "Mixer interceptor parsing on" : "Mixer interceptor passthrough only");
        return true;
    }
    if (name == "bluetooth" || name == "bluetoothRadio") {
        setBluetoothRadio(enabled);
        return true;
    }
    if (name == "wifiRadio") {
        if (panel == nullptr) {
            error = "Wi-Fi radio control is not available.";
            return false;
        }
        panel->applyWifi(enabled);
        return true;
    }
    if (name == "https") {
        return httpsSetEnabled(enabled, error);
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
        if (hid != nullptr) {
            if (enabled) {
                hid->setBleMix(blePairMix);
            }
            hid->setBleEnabled(enabled);
        }
        applyHidAdvertising();
        return true;
    }
    if (name == "usbHidKeyboard" || name == "usbHidMouse" || name == "usbHidAbs" || name == "usbHidDigitizer") {
        return setUsbHidPart(name, enabled, error);
    }
    if (name == "bleHidKeyboard" || name == "bleHidMouse" || name == "bleHidAbs" || name == "bleHidDigitizer") {
        return setBleHidPart(name, enabled, error);
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
        bool ok = multiMidi->setMidiOption(midiName, enabled, error);
        if (ok && midiName == "bleAdvertise" && enabled) {
            openPairingWindow(kApiPairingMs);
        }
        return ok;
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
    BlePeers::Peer existing;
    bool known = peers.find(address.c_str(), &existing);
    int autoConnect = (!known && midi == 1) ? 1 : -1;
    if (!peers.upsert(address.c_str(), name.c_str(), midi, hidRole, autoConnect, -1, 0)) {
        error = "Could not save that device.";
        return false;
    }
    clearIgnore(address.c_str());
    if (multiMidi != nullptr) {
        multiMidi->releaseHost(address.c_str());
    }
    if (midi == 1 && multiMidi != nullptr) {
        multiMidi->connectHost(address.c_str());
    }
    if (hidRole == 1) {
        hidEnabled = true;
        saveHid();
        if (hid != nullptr) {
            hid->setBleEnabled(true);
        }
        applyHidAdvertising();
    }
    char line[88];
    snprintf(line, sizeof(line), "Connecting to %s", name.length() ? name.c_str() : address.c_str());
    noteLog(line);
    return true;
}

bool Connections::disconnectAddress(const String& address, String& error, bool hold) {
    if (hold && multiMidi != nullptr) {
        multiMidi->holdHost(address.c_str());
    } else if (multiMidi != nullptr) {
        multiMidi->releaseHost(address.c_str());
    }
    bool dropped = false;
    if (multiMidi != nullptr && multiMidi->addressConnected(address.c_str())) {
        multiMidi->dropHost(address.c_str());
        dropped = true;
    }
    if (radio != nullptr && radio->disconnectIncoming(address.c_str())) {
        dropped = true;
    }
    if (!dropped && !peers.find(address.c_str(), nullptr)) {
        error = "That device is not connected.";
        return false;
    }
    return true;
}

bool Connections::queueForget(const String& address, String& error) {
    if (address.length() < 11) {
        error = "That device is not saved.";
        return false;
    }
    if (pendingForget) {
        error = "Busy, try again.";
        return false;
    }
    strncpy(pendingForgetAddress, address.c_str(), sizeof(pendingForgetAddress) - 1);
    pendingForgetAddress[sizeof(pendingForgetAddress) - 1] = '\0';
    // Always ignore, even if the address is not in NVS yet. Android RPAs
    // reconnect during the next pairing window and steal the host slot.
    ignoreIncoming(address.c_str());
    pendingForget = true;
    return true;
}

void Connections::ignoreIncoming(const char* address) {
    auto add = [this](const char* value) {
        if (value == nullptr || value[0] == '\0' || addressIgnored(value) || ignoreCount >= kIgnoreMax) {
            return;
        }
        strncpy(ignoreAddress[ignoreCount], value, sizeof(ignoreAddress[0]) - 1);
        ignoreAddress[ignoreCount][sizeof(ignoreAddress[0]) - 1] = '\0';
        ignoreCount++;
    };
    add(address);
    if (radio == nullptr) {
        return;
    }
    BleRadio::Incoming live[BleRadio::kMaxIncoming];
    int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
    for (int i = 0; i < liveCount; i++) {
        if (incomingIs(live[i], address)) {
            add(live[i].address);
            add(live[i].otaAddress);
            add(live[i].idAddress);
        }
    }
}

void Connections::clearIgnore(const char* address) {
    if (address == nullptr || address[0] == '\0') {
        return;
    }
    int write = 0;
    for (int i = 0; i < ignoreCount; i++) {
        if (sameAddress(ignoreAddress[i], address)) {
            continue;
        }
        if (write != i) {
            strncpy(ignoreAddress[write], ignoreAddress[i], sizeof(ignoreAddress[0]));
            ignoreAddress[write][sizeof(ignoreAddress[0]) - 1] = '\0';
        }
        write++;
    }
    ignoreCount = write;
}

bool Connections::addressIgnored(const char* address) const {
    if (address == nullptr || address[0] == '\0') {
        return false;
    }
    for (int i = 0; i < ignoreCount; i++) {
        if (sameAddress(ignoreAddress[i], address)) {
            return true;
        }
    }
    return false;
}

bool Connections::incomingIgnored(const BleRadio::Incoming& incoming) const {
    return addressIgnored(incoming.address) || addressIgnored(incoming.otaAddress)
        || addressIgnored(incoming.idAddress);
}

bool Connections::incomingSaved(const BleRadio::Incoming& incoming) const {
    BlePeers::Peer saved[BlePeers::kMax];
    int savedCount = peers.copy(saved, BlePeers::kMax);
    for (int i = 0; i < savedCount; i++) {
        if (incomingIs(incoming, saved[i].address)) {
            return true;
        }
    }
    return false;
}

void Connections::setAllowNewHosts(bool allowed) {
    if (allowed) {
        openPairingWindow(kApiPairingMs);
    } else {
        closePairingWindow();
    }
}

void Connections::openPairingWindow(uint32_t ms) {
    ignoreCount = 0;
    uint32_t now = millis();
    if (ms == 0) {
        pairingUntilMs = 0;
        applyAdvertisingPolicy();
        noteLog("Pairing window closed");
        return;
    }
    pairingUntilMs = now + ms;
    applyAdvertisingPolicy();
    char line[48];
    snprintf(line, sizeof(line), "Pairing window %u s", (unsigned)((ms + 999) / 1000));
    noteLog(line);
}

void Connections::closePairingWindow() {
    bool wasOpen = pairingUntilMs != 0;
    pairingUntilMs = 0;
    applyAdvertisingPolicy();
    if (wasOpen) {
        noteLog("Pairing window closed");
    }
}

bool Connections::pairingWindowOpen() const {
    return pairingUntilMs != 0 && (int32_t)(millis() - pairingUntilMs) < 0;
}

uint32_t Connections::pairingMsLeft() const {
    if (!pairingWindowOpen()) {
        return 0;
    }
    return pairingUntilMs - millis();
}

void Connections::applyForget(const char* address) {
    if (address == nullptr || address[0] == '\0') {
        return;
    }
    BlePeers::Peer peer;
    bool wasHost = peers.findMatch(address, nullptr, &peer) && peer.host;
    if (radio != nullptr) {
        BleRadio::Incoming live[BleRadio::kMaxIncoming];
        int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
        for (int i = 0; i < liveCount; i++) {
            if (incomingIs(live[i], address)) {
                wasHost = true;
                break;
            }
        }
    }
    if (wasHost) {
        ignoreIncoming(address);
    }
    String error;
    disconnectAddress(address, error, wasHost);
    if (peers.findMatch(address, nullptr, &peer)) {
        wasHost = wasHost || peer.host;
        peers.forget(peer.address);
        unpairAddress(peer.address);
    } else {
        peers.forget(address);
    }
    unpairAddress(address);
    if (radio != nullptr) {
        BleRadio::Incoming live[BleRadio::kMaxIncoming];
        int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
        for (int i = 0; i < liveCount; i++) {
            if (incomingIs(live[i], address)) {
                peers.forget(live[i].address);
                peers.forget(live[i].otaAddress);
                peers.forget(live[i].idAddress);
                unpairAddress(live[i].address);
                unpairAddress(live[i].otaAddress);
                unpairAddress(live[i].idAddress);
            }
        }
    }
    applyPairingMixIfIdle();
}

bool Connections::forget(const String& address, String& error) {
    return queueForget(address, error);
}

bool Connections::setPeerAuto(const String& address, bool enabled, String& error) {
    BlePeers::Peer peer;
    if (!peers.find(address.c_str(), &peer) || !peers.upsert(address.c_str(), "", -1, -1, enabled ? 1 : 0)) {
        error = "That device is not saved.";
        return false;
    }
    if (peer.host || multiMidi == nullptr) {
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
    clearIgnore(address.c_str());
    if (multiMidi != nullptr) {
        multiMidi->releaseHost(address.c_str());
        multiMidi->connectHost(address.c_str());
    }
    char line[88];
    snprintf(line, sizeof(line), "Connecting to %s", name.length() ? name.c_str() : address.c_str());
    noteLog(line);
    return true;
}

bool Connections::setPeerRoles(const String& address, int midi, int serialMidi, int hidRole,
        int hidKeyboard, int hidMouse, int hidAbs, int hidDigitizer, int pinned, String& error) {
    if (serialMidi > 0) {
        error = "The ESP32-S3 has no Bluetooth Classic radio, so Serial MIDI stays off.";
        return false;
    }
    BlePeers::Peer peer;
    if (!resolveSavedPeer(peers, radio, address.c_str(), &peer)) {
        error = "That device is not saved.";
        return false;
    }
    const char* key = peer.address;
    bool mixRequested = hidKeyboard >= 0 || hidMouse >= 0 || hidAbs >= 0 || hidDigitizer >= 0;
    if (!peer.host && (hidRole > 0 || mixRequested)) {
        error = "HID is for a computer in Remembered hosts.";
        return false;
    }
    int serialFlag = serialMidi == 0 ? 0 : -1;
    if (!peers.upsert(key, "", midi, hidRole, -1, serialFlag, -1)) {
        error = "Could not save that device.";
        return false;
    }
    auto incomingMatchesRole = [&](const BleRadio::Incoming& incoming) {
        return incomingIs(incoming, address.c_str()) || incomingIs(incoming, key);
    };
    if (mixRequested) {
        Hid::Mix before = peer.hid ? mixFromPeer(peer) : Hid::Mix::desktop();
        Hid::Mix mix = before;
        if (hidKeyboard >= 0) {
            mix.keyboard = hidKeyboard != 0;
        }
        if (hidMouse >= 0) {
            mix.mouse = hidMouse != 0;
        }
        if (hidAbs >= 0) {
            mix.absPointer = hidAbs != 0;
        }
        if (hidDigitizer >= 0) {
            mix.digitizer = hidDigitizer != 0;
        }
        if (!mix.any()) {
            error = "Choose at least one HID input.";
            return false;
        }
        if (!peers.setHidMix(key, mix.keyboard, mix.mouse, mix.absPointer, mix.digitizer)) {
            error = "Could not save that HID mix.";
            return false;
        }
        hidRole = 1;
        if (hid != nullptr) {
            hid->setBleMix(mix);
            if (radio != nullptr) {
                BleRadio::Incoming live[BleRadio::kMaxIncoming];
                int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
                for (int i = 0; i < liveCount; i++) {
                    if (incomingMatchesRole(live[i])) {
                        hid->setConnMix(live[i].connId, mix);
                    }
                }
            }
        }
    } else if (hid != nullptr && hidRole == 0) {
        Hid::Mix off{};
        // Leave the HOGP report map alone. MIDI-only must not rewrite the
        // descriptor; the host would still have the old map cached.
        if (radio != nullptr) {
            BleRadio::Incoming live[BleRadio::kMaxIncoming];
            int liveCount = radio->copyIncoming(live, BleRadio::kMaxIncoming);
            for (int i = 0; i < liveCount; i++) {
                if (incomingMatchesRole(live[i])) {
                    hid->setConnMix(live[i].connId, off);
                }
            }
        }
    }
    if (pinned >= 0) {
        bool wantPin = pinned != 0;
        BlePeers::Peer latest;
        peers.find(key, &latest);
        if (wantPin && !latest.pinned) {
            bool asHost = latest.host;
            int used = peers.pinnedCount(asHost);
            int cap = asHost ? hostSlots : periSlots;
            if (used >= cap) {
                error = asHost
                    ? "Host slots are full of reserved devices. Raise host slots or unpin another host."
                    : "Peripheral slots are full of reserved devices. Raise peripheral slots or unpin another peripheral.";
                return false;
            }
        }
        if (!peers.setPinned(key, wantPin)) {
            error = "Could not reserve that device.";
            return false;
        }
        if (latest.host) {
            enforceHostSlots();
        } else {
            enforcePeripheralSlots();
        }
    }
    if (!peer.host && multiMidi != nullptr) {
        if (midi == 1) {
            multiMidi->connectHost(address.c_str());
        } else if (midi == 0 && multiMidi->addressConnected(address.c_str())) {
            multiMidi->holdHost(address.c_str());
            multiMidi->dropHost(address.c_str());
        }
    }
    if (peer.host && midi == 0 && hidRole == 0 && radio != nullptr) {
        radio->disconnectIncoming(address.c_str());
    }
    if (hidRole == 1 || (hidRole < 0 && peer.hid) || mixRequested) {
        if (peer.host && (hidRole == 1 || mixRequested)) {
            hidEnabled = true;
            saveHid();
            if (hid != nullptr) {
                hid->setBleEnabled(true);
            }
            // Restarting GAP ads while a host is already linked makes BlueZ
            // drop HOGP, so the matrix loses evdev nodes and the next role
            // change sees no host.
            if (radio == nullptr || !radio->hasIncoming()) {
                applyHidAdvertising();
            } else {
                radio->setHidPeripheral(true);
            }
        }
    }
    if (hidRole == 0 && peer.host && !anyHostHid() && !bleIncomingOn()) {
        hidEnabled = false;
        saveHid();
        if (hid != nullptr) {
            hid->setBleEnabled(false);
        }
        applyHidAdvertising();
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

bool Connections::hidOutputReady(String& error) const {
    if (hid == nullptr) {
        error = "HID is not available.";
        return false;
    }
    if (hid->usbRunning()) {
        return true;
    }
    if (!hidEnabled) {
        error = "Turn on Bluetooth HID Peripheral first. A computer then connects to ShowboxMod.";
        return false;
    }
    if (!hid->ready()) {
        error = "No computer is connected yet.";
        return false;
    }
    return true;
}

void Connections::noteLog(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    snprintf(logLine[logNext], sizeof(logLine[0]), "%lus %s",
        static_cast<unsigned long>(millis() / 1000), text);
    logLine[logNext][sizeof(logLine[0]) - 1] = '\0';
    logNext = (logNext + 1) % kLogMax;
    if (logCount < kLogMax) {
        logCount++;
    }
}

bool Connections::queueMidiTest(String& error, int note, int velocity, int channel, const char* kind) {
    if (multiMidi == nullptr) {
        error = "MIDI is not available.";
        return false;
    }
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    if (velocity < 0) velocity = 0;
    if (velocity > 127) velocity = 127;
    if (channel < 1) channel = 1;
    if (channel > 16) channel = 16;
    midiTestOffAt = 0;
    midiTestNote = static_cast<uint8_t>(note);
    midiTestChan = static_cast<uint8_t>(channel);
    const char* mode = (kind != nullptr && kind[0] != '\0') ? kind : "note";
    char line[80];
    if (strcmp(mode, "cc") == 0) {
        multiMidi->controlChange(midiTestNote, static_cast<uint8_t>(velocity), midiTestChan);
        snprintf(line, sizeof(line), "MIDI test CC %u val %d ch %u", midiTestNote, velocity, midiTestChan);
        noteLog(line);
        return true;
    }
    if (strcmp(mode, "pc") == 0) {
        multiMidi->programChange(midiTestNote, midiTestChan);
        snprintf(line, sizeof(line), "MIDI test PC %u ch %u", midiTestNote, midiTestChan);
        noteLog(line);
        return true;
    }
    if (strcmp(mode, "panic") == 0) {
        multiMidi->controlChange(123, 0, midiTestChan);
        multiMidi->controlChange(120, 0, midiTestChan);
        snprintf(line, sizeof(line), "MIDI panic ch %u", midiTestChan);
        noteLog(line);
        return true;
    }
    if (velocity < 1) velocity = 1;
    multiMidi->noteOn(midiTestNote, static_cast<uint8_t>(velocity), midiTestChan);
    midiTestOffAt = millis() + 250;
    snprintf(line, sizeof(line), "MIDI test note %u vel %d ch %u", midiTestNote, velocity, midiTestChan);
    noteLog(line);
    return true;
}

bool Connections::queueKeys(const String& text, String& error) {
    if (!hidOutputReady(error)) {
        return false;
    }
    if (!hid->sendText(text.c_str())) {
        if (!hid->activeMix().keyboard) {
            error = "Keyboard is off in the HID mix for this connection.";
        } else {
            error = "Those keys could not be sent.";
        }
        return false;
    }
    char line[80];
    snprintf(line, sizeof(line), "HID keys: %s", text.c_str());
    noteLog(line);
    return true;
}

bool Connections::queueMouse(int x, int y, int buttons, String& error, int wheel, int pan) {
    int8_t dx = x > 127 ? 127 : (x < -127 ? -127 : (int8_t)x);
    int8_t dy = y > 127 ? 127 : (y < -127 ? -127 : (int8_t)y);
    int8_t dw = wheel > 127 ? 127 : (wheel < -127 ? -127 : (int8_t)wheel);
    int8_t dp = pan > 127 ? 127 : (pan < -127 ? -127 : (int8_t)pan);
    uint8_t buttonBits = buttons < 0 ? 0 : (buttons > 7 ? 7 : (uint8_t)buttons);
    if (!hidOutputReady(error)) {
        return false;
    }
    if (!hid->sendMouse(dx, dy, buttonBits, dw, dp)) {
        if (!hid->activeMix().mouse) {
            error = "Mouse is off in the HID mix for this connection.";
        } else {
            error = "The mouse action could not be sent.";
        }
        return false;
    }
    char line[80];
    snprintf(line, sizeof(line), "HID mouse %+d,%+d btn=%u wheel=%d pan=%d",
        dx, dy, buttonBits, dw, dp);
    noteLog(line);
    return true;
}

bool Connections::bleIncomingOn() const {
    if (radio == nullptr) {
        return false;
    }
    BleRadio::Incoming incoming[BleRadio::kMaxIncoming];
    return radio->copyIncoming(incoming, BleRadio::kMaxIncoming) > 0;
}

bool Connections::queueKey(const char* name, String& error) {
    static const struct {
        const char* name;
        uint8_t usage;
    } keys[] = {
        {"pageup", 0x4B}, {"pagedown", 0x4E}, {"up", 0x52}, {"down", 0x51},
        {"left", 0x50}, {"right", 0x4F}, {"space", 0x2C}, {"enter", 0x28},
        {"escape", 0x29}, {"tab", 0x2B}, {"backspace", 0x2A}, {"delete", 0x4C},
        {"home", 0x4A}, {"end", 0x4D}, {"insert", 0x49},
        {"f1", 0x3A}, {"f2", 0x3B}, {"f3", 0x3C}, {"f4", 0x3D},
        {"f5", 0x3E}, {"f6", 0x3F}, {"f7", 0x40}, {"f8", 0x41},
        {"f9", 0x42}, {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
        {"ctrl", 0xE0}, {"shift", 0xE1}, {"alt", 0xE2}, {"gui", 0xE3},
    };
    uint8_t usage = 0;
    if (name != nullptr) {
        for (const auto& key : keys) {
            if (strcmp(name, key.name) == 0) {
                usage = key.usage;
                break;
            }
        }
    }
    if (usage == 0) {
        error = "Unknown key.";
        return false;
    }
    if (!hidOutputReady(error)) {
        return false;
    }
    if (!hid->sendUsage(usage)) {
        if (!hid->activeMix().keyboard) {
            error = "Keyboard is off in the HID mix for this connection.";
        } else {
            error = "That key could not be sent.";
        }
        return false;
    }
    char line[48];
    snprintf(line, sizeof(line), "HID key %s", name);
    noteLog(line);
    return true;
}

bool Connections::queueScroll(int amount, String& error) {
    if (amount == 0) {
        error = "Scroll amount is zero.";
        return false;
    }
    int8_t wheel = amount > 127 ? 127 : (amount < -127 ? -127 : (int8_t)amount);
    if (!hidOutputReady(error)) {
        return false;
    }
    if (!hid->sendMouse(0, 0, 0, wheel)) {
        if (!hid->activeMix().mouse) {
            error = "Mouse is off in the HID mix for this connection.";
        } else {
            error = "The scroll could not be sent.";
        }
        return false;
    }
    return true;
}

bool Connections::queueTouch(const String& action, int x, int y, int x2, int y2, String& error, const char* via) {
    if (!hidOutputReady(error)) {
        return false;
    }
    Hid::Mix mix = hid->activeMix();
    if (via != nullptr && via[0] != '\0') {
        if (strcmp(via, "abs") == 0 && !mix.absPointer && !mix.digitizer) {
            error = "Absolute pointer is off in the HID mix for this connection.";
            return false;
        }
        if ((strcmp(via, "touch") == 0 || strcmp(via, "digitizer") == 0) && !mix.digitizer && !mix.absPointer) {
            error = "Touch screen is off in the HID mix for this connection.";
            return false;
        }
    }
    bool ok = false;
    if (action == "tap") {
        ok = hid->sendTap(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
    } else if (action == "swipe") {
        ok = hid->sendSwipe(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
            static_cast<uint16_t>(x2), static_cast<uint16_t>(y2));
    } else if (action == "pinch" || action == "pinch-in") {
        ok = hid->sendPinch(static_cast<uint16_t>(x), static_cast<uint16_t>(y), true);
    } else if (action == "pinch-out") {
        ok = hid->sendPinch(static_cast<uint16_t>(x), static_cast<uint16_t>(y), false);
    } else {
        error = "Use tap, swipe, pinch-in, or pinch-out.";
        return false;
    }
    if (!ok) {
        if (!mix.absPointer && !mix.digitizer) {
            error = "Turn on Touch screen (phones, no cursor) or Absolute pointer (Linux) in the HID mix.";
        } else {
            error = "The touch action could not be sent.";
        }
        return false;
    }
    char line[88];
    snprintf(line, sizeof(line), "HID %s %s (%d,%d)->(%d,%d)",
        via != nullptr && via[0] != '\0' ? via : "pointer",
        action.c_str(), x, y, x2, y2);
    noteLog(line);
    return true;
}
