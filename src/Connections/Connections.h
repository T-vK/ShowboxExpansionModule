#pragma once

#include <Arduino.h>
#include "BlePeers/BlePeers.h"
#include "MultiMidi/BleMidiHostPolicy.h"
#include <BleRadio.h>
#include <Hid.h>

class MultiMidi;
class MackieShowbox;

// Pairing and transport switches. MIDI lives in MultiMidi. HID is passed in.
class Connections : public BleMidiHostPolicy {
public:
    void attach(MultiMidi* midi, BleRadio* radio, Hid* hid);
    void setShowbox(MackieShowbox* showbox);
    void setDebugSettingsHandler(void (*handler)(bool wifiHistory, bool uartPackets));
    void setLinkChangeHandler(void (*handler)());
    void setDebugStatus(bool connected, size_t bytes);
    void load();
    void startAfterBle();
    void tick();

    bool shouldConnect(const char* name, const char* address) override;
    int connectScore(const char* name, const char* address) override;
    int maxOutgoing() override;
    bool wantsAutoScan() override;
    bool nextAutoOutgoing(char* address, size_t addressSize, char* name, size_t nameSize) override;
    bool yieldOutgoing() override;
    void note(const char* text) override;

    String statusJson();
    void refreshStatusCache();
    const uint8_t* statusCacheBytes(size_t* len) const;
    bool setOption(const String& name, bool enabled, String& error);
    bool setSlots(int hosts, int peripherals, String& error);
    bool queueSlots(int hosts, int peripherals, String& error);
    void requestScan();
    bool connect(const String& address, const String& name, int midi, int hid, String& error);
    bool pair(const String& address, const String& name, String& error);
    bool queueOption(const char* name, bool enabled);
    bool queuePeerRoles(const String& address, int midi, int serialMidi, int hid,
        int hidKeyboard, int hidMouse, int hidAbs, int hidDigitizer, int pinned, String& error);
    bool queueForget(const String& address, String& error);
    bool disconnectAddress(const String& address, String& error, bool hold = true);
    bool forget(const String& address, String& error);
    bool setPeerAuto(const String& address, bool enabled, String& error);
    bool queueKeys(const String& text, String& error);
    bool queueMouse(int x, int y, int buttons, String& error, int wheel = 0, int pan = 0);
    bool queueTouch(const String& action, int x, int y, int x2, int y2, String& error, const char* via = nullptr);
    bool queueKey(const char* name, String& error);
    bool queueScroll(int amount, String& error);
    bool queueMidiTest(String& error);
    void noteLog(const char* text);
    bool bleIncomingOn() const;
    void openPairingWindow(uint32_t ms);
    void closePairingWindow();
    bool pairingWindowOpen() const;
    uint32_t pairingMsLeft() const;
    void setBluetoothRadio(bool on);
    bool bluetoothRadioOn() const { return bluetoothRadioEnabled; }
    enum class UsbPortMode {
        Jtag,
        Host,
        Peripheral
    };
    void setUsbPortMode(UsbPortMode mode);
    bool usbDeviceOn() const { return usbDeviceEnabled; }
    bool usbHostOn() const { return usbHostEnabled; }
    bool usbHidOn() const { return usbHidEnabled; }
    bool wifiLogHistoryOn() const { return wifiLogHistory; }
    bool uartPacketsOn() const { return uartPackets; }

private:
    void saveHid();
    void saveUsbMode(bool device, bool host, bool hid);
    void loadDebug();
    void applyDebug();
    void applyHidAdvertising();
    void applyAdvertisingPolicy();
    void scheduleReboot(const char* reason);
    bool sameAddress(const char* left, const char* right) const;
    bool anyHostHid() const;
    bool hidOutputReady(String& error) const;
    Hid::Mix mixFromPeer(const BlePeers::Peer& peer) const;
    void onIncoming(const char* address, uint16_t connId, bool connected);
    static void incomingTrampoline(void* ctx, const char* address, uint16_t connId, bool connected);
    bool setUsbHidPart(const String& name, bool enabled, String& error);
    bool setBleHidPart(const String& name, bool enabled, String& error);
    void applyPairingMixIfIdle();
    void rememberIncomingHost(const BleRadio::Incoming& incoming);
    void enforceHostSlots();
    void enforcePeripheralSlots();
    bool dropNewestUnpinnedOutgoing();
    bool peerPinned(const char* address) const;
    int outgoingUsed() const;
    void pruneGhostHosts();
    void applyPending();
    void publishShowboxBattery();
    void noteScanFeatures();
    void ignoreIncoming(const char* address);
    void clearIgnore(const char* address);
    bool incomingIgnored(const BleRadio::Incoming& incoming) const;
    bool addressIgnored(const char* address) const;
    bool incomingSaved(const BleRadio::Incoming& incoming) const;
    void setAllowNewHosts(bool allowed);
    void applyForget(const char* address);
    bool setPeerRoles(const String& address, int midi, int serialMidi, int hid,
        int hidKeyboard, int hidMouse, int hidAbs, int hidDigitizer, int pinned, String& error);
    void writeStatusJson(char* buf, size_t cap, size_t* outLen);

    static constexpr int kPendingMax = 8;
    static constexpr uint32_t kApiPairingMs = 90000;
    struct PendingOption {
        char name[28];
        bool enabled;
    };
    PendingOption pendingOptions[kPendingMax] = {};
    int pendingCount = 0;
    char pendingRoleAddress[18] = {};
    int pendingRoleMidi = -1;
    int pendingRoleSerial = -1;
    int pendingRoleHid = -1;
    int pendingRoleKb = -1;
    int pendingRoleMouse = -1;
    int pendingRoleAbs = -1;
    int pendingRoleDig = -1;
    int pendingRolePinned = -1;
    bool pendingRole = false;
    int pendingHostSlots = -1;
    int pendingPeriSlots = -1;
    bool pendingForget = false;
    char pendingForgetAddress[18] = {};
    static constexpr int kIgnoreMax = 8;
    char ignoreAddress[kIgnoreMax][18] = {};
    int ignoreCount = 0;
    uint32_t lastPruneMs = 0;
    uint32_t lastHostRememberMs = 0;
    uint32_t midiTestOffAt = 0;
    static constexpr int kLogMax = 16;
    char logLine[kLogMax][88] = {};
    int logCount = 0;
    int logNext = 0;

    MultiMidi* multiMidi = nullptr;
    BleRadio* radio = nullptr;
    Hid* hid = nullptr;
    MackieShowbox* showbox = nullptr;
    void (*debugSettingsHandler)(bool, bool) = nullptr;
    void (*linkChangeHandler)() = nullptr;
    int lastBleLinks = -1;
    BlePeers peers;
    bool wifiLogConnected = false;
    size_t wifiLogBytes = 0;
    bool hidEnabled = false;
    bool bluetoothRadioEnabled = true;
    bool usbDeviceEnabled = false;
    bool usbHostEnabled = false;
    bool usbHidEnabled = false;
    Hid::Mix usbHidMix = Hid::Mix::desktop();
    Hid::Mix blePairMix = Hid::Mix::phone();
    bool usbSafeTest = false;
    bool wifiLogHistory = true;
    bool uartPackets = false;
    bool debugDirty = false;
    bool usbDirty = false;
    bool hidDirty = false;
    bool blePairDirty = false;
    bool slotsDirty = false;
    uint32_t pairingUntilMs = 0;
    int hostSlots = 1;
    int periSlots = 2;
    bool rebootPending = false;
    uint32_t rebootAt = 0;
    static constexpr size_t kStatusCacheSize = 8192;
    char* statusBuf[2] = {};
    size_t statusLen[2] = {};
    volatile int statusReady = 0;
    uint32_t lastStatusMs = 0;
    int lastCacheLinks = -1;
    int lastCacheOut = -1;
};
