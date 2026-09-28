#ifndef SHOWBOX_BLE_MIDI_H
#define SHOWBOX_BLE_MIDI_H

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <host/ble_gap.h>
#include <BLEClient.h>
#include "MidiAction.h"
#include "MidiBleParser.h"
#include "MultiMidi/BleMidiHostPolicy.h"

// Connects out to BLE MIDI peripherals and advertises ShowboxMod
// so a phone or laptop can connect at the same time.
class BleMidi : public BLEClientCallbacks {
public:
    static constexpr int kMaxDevices = 16;

    struct SeenDevice {
        char name[32];
        char address[18];
        int rssi;
        bool midi;
        bool present;
    };

    static BleMidi* active();
    void begin(midi::MidiAction* action, Print* debug);
    void tick();

    void setPolicy(BleMidiHostPolicy* connectPolicy);
    void setAdvertising(bool enabled);
    void setCentral(bool enabled);
    void setAutoConnect(bool enabled);
    void setTargetAddress(const char* address);
    void connectAddress(const char* address);
    void holdAddress(const char* address);
    void dropLink();
    bool advertising() const;
    bool central() const;
    bool isConnected() const;
    bool isScanning() const;
    const char* connectedName() const;
    const char* connectedAddress() const;
    const char* targetAddress() const;

    void requestScan();
    void disconnect();
    int copyDevices(SeenDevice* out, int max) const;
    void noteDevice(BLEAdvertisedDevice& device);
    void noteName(const char* address, const char* name, bool completeName, int rssi);
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
    void clearSeen();
    void pruneUnseen();
    void advertiseIfEnabled();

    midi::MidiBleParser* parser = nullptr;
    Print* Debug = &Serial;
    BLEClient* client = nullptr;
    BleMidiHostPolicy* policy = nullptr;
    volatile bool connected = false;
    volatile bool advertisingEnabled = true;
    volatile bool centralEnabled = true;
    volatile bool autoConnectOn = true;
    volatile bool connectOnce = false;
    volatile bool scanRequested = false;
    volatile bool scanning = false;
    volatile bool scanFull = false;

    char target[18] = "";
    char heldAddress[18] = "";
    char peerName[32] = "";
    char peerAddress[18] = "";

    SeenDevice seen[kMaxDevices] = {};
    volatile int seenCount = 0;
    mutable portMUX_TYPE seenMux = portMUX_INITIALIZER_UNLOCKED;

    static BleMidi* instance;
};

#endif // SHOWBOX_BLE_MIDI_H
