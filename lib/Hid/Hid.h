#pragma once

#include <Arduino.h>
#include "sdkconfig.h"

class BLEServer;

// Keyboard, relative mouse, Generic Desktop absolute pointers, and a Digitizer
// touch screen. USB uses one mix for the cable. Bluetooth exposes one GATT HID
// service and one report map to every host (HOGP has no per-connection map).
// The BLE map is keyboard + mouse + absolute pointer: a Digitizer Touch Screen
// collection makes Linux bind hid-multitouch and drop relative mouse. Touch
// frames are notified on the absolute pointer characteristic. Each host mix
// only chooses which input reports we notify on that link.
class Hid {
public:
    static constexpr uint16_t kAxisMax = 10000;
#ifdef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
    static constexpr int kMaxBleConn = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#else
    static constexpr int kMaxBleConn = 3;
#endif

    struct Mix {
        bool keyboard = false;
        bool mouse = false;
        bool absPointer = false;
        bool digitizer = false;

        static Mix desktop() {
            return {true, true, true, false};
        }

        // Android-oriented: real touch, no Linux tablet cursor.
        static Mix phone() {
            return {true, true, false, true};
        }

        bool any() const {
            return keyboard || mouse || absPointer || digitizer;
        }

        bool same(const Mix& other) const {
            return keyboard == other.keyboard && mouse == other.mouse
                && absPointer == other.absPointer && digitizer == other.digitizer;
        }

        // Linux hid-multitouch claims the whole HOGP device if a Digitizer
        // Touch Screen collection is in the map. BLE touch uses the absolute
        // pointer collection instead. Real mouse + real abs in one map makes
        // Linux create no ShowboxMod evdev nodes, so BLE never advertises both.
        // The host must unpair when this mix changes because BlueZ caches the
        // report map.
        Mix asBleMap() const {
            Mix m = *this;
            if (m.digitizer) {
                m.absPointer = true;
                m.digitizer = false;
            }
            if (m.absPointer && m.mouse) {
                m.mouse = false;
            }
            return m;
        }

        // Bit 4 marks an explicit mix. Zero means "legacy default" (desktop).
        static Mix fromStored(int bits) {
            if ((bits & 0x10) == 0) {
                return desktop();
            }
            Mix mix;
            mix.keyboard = (bits & 0x01) != 0;
            mix.mouse = (bits & 0x02) != 0;
            mix.absPointer = (bits & 0x04) != 0;
            mix.digitizer = (bits & 0x08) != 0;
            return mix.any() ? mix : desktop();
        }

        int storedBits() const {
            return 0x10
                | (keyboard ? 0x01 : 0)
                | (mouse ? 0x02 : 0)
                | (absPointer ? 0x04 : 0)
                | (digitizer ? 0x08 : 0);
        }
    };

    void setDebug(Print* serial);
    void setIdentity(const char* manufacturer, const char* product);
    void attachBle(BLEServer* server);

    void setBleEnabled(bool enabled);
    bool bleEnabled() const { return bleOn; }
    bool bleReady() const;
    void setBleMix(Mix mix);
    void setConnMix(uint16_t connId, Mix mix);
    void clearConnMix(uint16_t connId);
    Mix unionConnMix() const;
    void setBatteryPercent(uint8_t percent);
    Mix bleMix() const { return bleActive; }

    bool startUsb();
    bool usbRunning() const { return usbOn; }
    bool usbReady() const;
    Mix usbMix() const { return usbActive; }

    Mix currentMix() const;
    Mix activeMix() const { return currentMix(); }
    bool ready() const { return usbReady() || bleReady(); }
    void tick();

    bool sendText(const char* text);
    bool sendUsage(uint8_t key);
    bool sendKey(uint8_t usage, uint8_t modifiers, bool down, bool autoUp = false);
    bool sendMouse(int8_t x, int8_t y, uint8_t buttons, int8_t wheel = 0, int8_t pan = 0, bool autoRelease = true);
    bool sendConsumer(uint16_t usage, bool down, bool autoUp = false);
    bool sendAbs(uint16_t x, uint16_t y, bool down, uint8_t buttons = 1);
    bool sendTap(uint16_t x, uint16_t y);
    bool sendSwipe(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    bool sendPinch(uint16_t cx, uint16_t cy, bool inward);

private:
    struct CombinedReport {
        uint8_t parts;
        uint8_t kbMod;
        uint8_t kbReserved;
        uint8_t kbKeys[6];
        uint8_t mouseButtons;
        int8_t mouseX;
        int8_t mouseY;
        int8_t mouseWheel;
        int8_t mousePan;
        uint8_t contact0Flags;
        uint8_t contact0Id;
        uint16_t contact0X;
        uint16_t contact0Y;
        uint8_t contact1Flags;
        uint8_t contact1Id;
        uint16_t contact1X;
        uint16_t contact1Y;
        uint8_t contactCount;
        uint16_t consumer;
    } __attribute__((packed));

    bool ensureBleStarted();
    bool emit(const CombinedReport& report);
    bool pushBle(const CombinedReport& report);
    bool dispatch(const CombinedReport& report, Mix mix);
    bool notifyParts(const CombinedReport& report, Mix mix, uint16_t connId);
    bool notifyInput(class BLECharacteristic* ch, uint16_t connId, const uint8_t* data, size_t len);
    CombinedReport blank() const;
    bool pushTouchFrame(uint16_t x0, uint16_t y0, bool down0, uint16_t x1, uint16_t y1, bool down1, uint8_t count);
    static uint16_t clampAxis(int value);
    void rebuildBleMap();
    static Mix loadUsbMix();
    static uint16_t buildMap(uint8_t* out, Mix mix);
    static size_t packReport(uint8_t* out, Mix mix, const CombinedReport& report);

    Print* debug = &Serial;
    const char* manufacturerName = "ShowboxMod";
    const char* productName = "ShowboxMod";
    BLEServer* bleServer = nullptr;
    class BLEHIDDevice* bleDevice = nullptr;
    class BLECharacteristic* bleInputKb = nullptr;
    class BLECharacteristic* bleInputMouse = nullptr;
    class BLECharacteristic* bleInputAbs = nullptr;
    class BLECharacteristic* bleInputDig = nullptr;
    class BLECharacteristic* bleInputConsumer = nullptr;
    bool bleStarted = false;
    bool bleOn = false;
    bool bleSecurityReady = false;
    bool usbOn = false;
    Mix bleActive{true, true, false, false};
    Mix usbActive = Mix::desktop();
    struct ConnMix {
        uint16_t connId;
        Mix mix;
    };
    ConnMix connMix[kMaxBleConn] = {};
    int connMixCount = 0;
    uint8_t bleBattery = 255;
    uint8_t bleMap[384] = {};
    uint16_t bleMapLen = 0;
    uint8_t usbMap[384] = {};
    uint16_t usbMapLen = 0;
    unsigned long lastSentMs = 0;
    bool kbReleasePending = false;
    uint32_t kbReleaseAtMs = 0;

    class UsbCombinedDevice* usbDevice = nullptr;

    static constexpr int kQueueSize = 96;
    CombinedReport queue[kQueueSize] = {};
    Mix queueMix[kQueueSize] = {};
    int queueHead = 0;
    int queueCount = 0;
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
