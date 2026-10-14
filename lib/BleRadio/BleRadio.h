#pragma once

#include <Arduino.h>
#include <BLEServer.h>
#include "sdkconfig.h"

#ifndef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 3
#endif

// Shared BLE advertising for MIDI peripheral and HID peripheral.
// One radio, one short name. A host pairs once and can use MIDI, HID, or both.
class BleRadio : public BLEServerCallbacks {
public:
    // Arduino-ESP32 NimBLE on this S3 is built with 3 ACL slots. Incoming
    // hosts and the AIRSTEP central share that pool; 4 was a leftover default
    // above what the stack actually allocates.
    static constexpr int kMaxIncoming = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;

    struct Incoming {
        uint16_t connId;
        char address[18];
        char otaAddress[18];
        char idAddress[18];
        char name[32];
        bool namePending;
        uint8_t nameTries;
        uint32_t sinceMs;
        bool intervalPending;
        bool bonded;
    };

    void setName(const char* name);
    const char* name() const { return gapName; }

    void setMidiPeripheral(bool enabled);
    void setHidPeripheral(bool enabled);
    bool midiPeripheral() const;
    bool hidPeripheral() const;
    // Advertising is a radio duty cycle. Keep it off once a host is linked
    // unless Connections opens a pairing window.
    void setAdvertiseAllowed(bool allowed);
    bool advertiseAllowed() const { return adsAllowed; }
    bool advertising() const { return advertised; }
    uint16_t advInterval() const { return kAdvItvlMin; }
    static constexpr uint16_t kAdvItvlMin = 0x1000;  // 2560 ms. 1280 ms ads plus AIRSTEP still killed ARP.
    static constexpr uint16_t kAdvItvlMax = 0x1400;  // 3200 ms. Still found in an 8s scan.
    // 200 ms. AIRSTEP floors here and rejects slower offers. Treating 200 ms as
    // "too fast" sent endless LL_CONNECTION_UPDATE and killed Wi-Fi.
    static constexpr uint16_t kWifiSafeItvl = 0xA0;
    static constexpr uint16_t kSlowItvlMax = 0x1E0;  // 480 ms when a peer will take it.
    // 2.5 ms. AIRSTEP will not take a slower interval than 200 ms; leaving
    // max_ce_len uncapped lets that event occupy the whole interval and ARP dies.
    static constexpr uint16_t kMaxCeLen = 8;
    // While a BLE central (AIRSTEP) is up, continuous GAP ads still hold PTA
    // even at 2560 ms. Pulse connectable ads only when a pairing window is open.
    static constexpr uint32_t kPulseOnMs = 2500;
    static constexpr uint32_t kPulsePeriodMs = 8000;
    static constexpr uint16_t kPulseAdvMin = 0x280;  // 400 ms during pairing with a pedal
    static constexpr uint16_t kPulseAdvMax = 0x320;  // 500 ms
    void apply();
    void restartWhenIdle();
    void stopNow();
    void setPedalShare(bool share);
    // Start advertising again after a GAP connect without rebuilding the
    // payload. setAdvertisementData after AIRSTEP was BLE_INIT malloc-failed
    // and left the controller holding the radio.
    void resumeExisting();
    void service();
    void attach(BLEServer* server);
    void setIncomingHandler(void (*handler)(void* ctx, const char* address, uint16_t connId, bool connected), void* ctx);

    int copyIncoming(Incoming* out, int max) const;
    // Cached ACL count. Refresh from the BLE task; HTTP must not walk GAP.
    void refreshLinkCount();
    int gapLinkCount() const;
    uint16_t minConnInterval() const { return minItvl; }
    bool hasIncoming() const;
    bool disconnectIncoming(const char* address);
    void preferIncomingName(const char* address, const char* name);
    void queryIncomingNames();

    void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override;
    void onDisconnect(BLEServer* server, ble_gap_conn_desc* desc) override;
    void onBonded(ble_gap_conn_desc* desc);
    void storeIncomingName(uint16_t connId, const char* name);
    void finishIncomingName(uint16_t connId, bool keepPending);

private:
    BLEServer* server = nullptr;
    const char* gapName = "BLE";
    bool midiOn = true;
    bool hidOn = false;
    bool adsAllowed = false;
    bool advertised = false;
    bool advertisedMidi = false;
    bool advertisedHid = false;
    volatile bool restartAdvertising = false;
    bool advIntervalApplied = false;
    bool pedalShare = false;
    uint32_t lastAdvStartMs = 0;
    uint32_t lastIncomingMs = 0;
    volatile int liveLinks = 0;
    volatile uint16_t minItvl = 0;
    uint32_t ceCapped = 0;
    Incoming incoming[kMaxIncoming] = {};
    int incomingCount = 0;
    char nameSkip[8][18] = {};
    int nameSkipCount = 0;
    void (*incomingHandler)(void* ctx, const char* address, uint16_t connId, bool connected) = nullptr;
    void* incomingHandlerCtx = nullptr;
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
