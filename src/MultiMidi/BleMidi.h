#ifndef SHOWBOX_BLE_MIDI_H
#define SHOWBOX_BLE_MIDI_H

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <host/ble_gap.h>
#include <BLEClient.h>
#include "MidiAction.h"
#include "MultiMidi/BleMidiDecode.h"
#include "MultiMidi/BleMidiHostPolicy.h"

class BleRadio;
class BLERemoteCharacteristic;
namespace midi {
struct MidiMessage;
}

// Connects out to BLE MIDI peripherals and advertises ShowboxMod
// so a phone or laptop can connect at the same time.
class BleMidi : public BLEClientCallbacks {
public:
    static constexpr int kMaxDevices = 16;
    static constexpr int kMaxOutgoing = 2;

    struct SeenDevice {
        char name[32];
        char address[18];
        int rssi;
        uint8_t addrType;
        bool midi;
        bool hid;
        bool battery;
        bool present;
    };

    struct Outgoing {
        char name[32];
        char address[18];
        int rssi;
        int battery;
        bool haveRssi;
        bool connected;
        uint32_t sinceMs;
    };

    static BleMidi* active();
    void begin(midi::MidiAction* action, Print* debug);
    void tick();

    void setPolicy(BleMidiHostPolicy* connectPolicy);
    void setRadio(class BleRadio* bleRadio);
    void setAdvertising(bool enabled);
    void setCentral(bool enabled);
    void setAutoConnect(bool enabled);
    void allowOutgoing(bool enabled);
    void setTargetAddress(const char* address);
    void connectAddress(const char* address);
    void holdAddress(const char* address);
    void releaseAddress(const char* address);
    void dropLink();
    void dropAddress(const char* address);
    void setMaxOutgoing(int n);
    int maxOutgoing() const { return outgoingMax; }
    bool advertising() const;
    bool central() const;
    bool isConnected() const;
    bool isScanning() const;
    bool addressConnected(const char* address) const;
    int connectedCount() const;
    int copyOutgoing(Outgoing* out, int max) const;
    const char* connectedName() const;
    const char* connectedAddress() const;
    const char* targetAddress() const;
    int connectedRssi() const;
    bool connectedRssiKnown() const;
    int connectedBattery() const;

    void write(midi::MidiMessage* msg, int len);

    void requestScan();
    void disconnect();
    int copyDevices(SeenDevice* out, int max) const;
    void noteDevice(BLEAdvertisedDevice& device);
    void noteName(const char* address, const char* name, bool completeName, int rssi);
    void noteSeen(const char* address, const char* name, bool completeName, int rssi, bool midi, bool hid, bool battery, uint8_t addrType = 0xFF);
    bool wantsDevice(BLEAdvertisedDevice& device) const;
    bool isScanningFull() const;

    void onConnect(BLEClient* client) override;
    void onDisconnect(BLEClient* client) override;

private:
    static void onNotify(BLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify);
    static int onGap(struct ble_gap_event* event, void* arg);
    static void taskEntry(void* arg);
    void scanLoop();
    void connectTo(BLEAdvertisedDevice* device);
    bool connectAddressNow(const char* address, const char* name, uint32_t timeoutMs = 5000, int addrType = -1);
    bool tryConnect(BLEClient* client, const char* address, uint8_t type, uint32_t timeoutMs);
    uint8_t typeForAddress(const char* address) const;
    int freeOutgoingSlot() const;
    int slotOfClient(BLEClient* client) const;
    int slotOfAddress(const char* address) const;
    void clearSlot(int slot);
    void dropClient(int slot);
    void clearSeen();
    void pruneUnseen();
    void advertiseIfEnabled();
    void keepAdvertising();
    void refreshLink();
    void readPeerBattery();
    bool hasRoomForOutgoing() const;

    BleMidiDecode decoder;
    Print* Debug = &Serial;
    BleMidiHostPolicy* policy = nullptr;
    BleRadio* radio = nullptr;
    struct OutgoingLink {
        BLEClient* client = nullptr;
        BLERemoteCharacteristic* characteristic = nullptr;
        volatile bool connected = false;
        char name[32] = {};
        char address[18] = {};
        int rssi = 0;
        int battery = -1;
        bool haveRssi = false;
        uint32_t lastBatteryReadMs = 0;
        uint32_t sinceMs = 0;
    };
    uint8_t timestampLow = 0;
    OutgoingLink outgoing[kMaxOutgoing] = {};
    int outgoingMax = kMaxOutgoing;
    volatile bool advertisingEnabled = true;
    volatile bool centralEnabled = true;
    volatile bool autoConnectOn = true;
    volatile bool outgoingAllowed = false;
    uint32_t outgoingReadyMs = 0;
    volatile bool connectOnce = false;
    volatile bool directConnect = false;
    volatile bool scanRequested = false;
    volatile bool scanning = false;
    volatile bool scanFull = false;

    char target[18] = "";
    char heldAddress[18] = "";
    char lastFailAddress[18] = "";
    uint8_t lastFailType = 0xFF;
    uint32_t lastLinkMs = 0;
    uint32_t lastQuietConnectMs = 0;
    uint32_t lastOutgoingFailMs = 0;

    SeenDevice seen[kMaxDevices] = {};
    volatile int seenCount = 0;
    mutable portMUX_TYPE seenMux = portMUX_INITIALIZER_UNLOCKED;

    static BleMidi* instance;
};

#endif // SHOWBOX_BLE_MIDI_H
