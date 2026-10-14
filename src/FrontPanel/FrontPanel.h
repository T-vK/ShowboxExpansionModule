#pragma once

#include <Arduino.h>
#include <OneButton.h>
#include "pin_definitions.h"

class Connections;
class Hid;
class UltimateBaseFirmware;

// RGB-ring buttons: blue = Bluetooth, red = USB port, green = Wi-Fi radio.
class FrontPanel {
public:
    void attach(Connections* connections, Hid* hid, UltimateBaseFirmware* firmware);
    void begin();
    void tick();

    bool wifiRadioOn() const { return wifiOn; }
    bool wifiPortalOpen() const;
    void applyWifi(bool on);
    void startWifiPortal();

private:
    enum class LedId : uint8_t {
        Red = 0,
        Green = 1,
        Blue = 2,
    };

    void loadRadio();
    void saveRadio();
    void onBlueClick();
    void onBlueLong();
    void onRedClick();
    void onRedLong();
    void onGreenClick();
    void onGreenLong();
    void applyBluetooth(bool on);
    void stopWifiPortal();
    void updateLeds();
    void writeLeds();
    void setLed(LedId id, uint8_t brightness);
    void sendByte(uint8_t value);
    uint8_t wave(uint8_t low, uint8_t high, uint32_t periodMs) const;
    bool blinkOn(uint32_t periodMs) const;
    bool usbHostConnected() const;
    bool usbPeripheralConnected() const;

    Connections* connections = nullptr;
    Hid* hid = nullptr;
    UltimateBaseFirmware* firmware = nullptr;
    OneButton blue{BLUE_BUTTON, true};
    OneButton red{RED_BUTTON, true};
    OneButton green{GREEN_BUTTON, true};
    bool btOn = true;
    bool wifiOn = true;
    uint32_t wifiPortalUntil = 0;
    uint8_t ledBri[3] = {255, 255, 255};
    uint32_t lastLedMs = 0;
};
