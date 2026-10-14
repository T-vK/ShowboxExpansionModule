#include "FrontPanel.h"

#include "Actions/ActionRules.h"
#include "Connections/Connections.h"
#include "MultiMidi/UsbMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include <Hid.h>
#include <UltimateBaseFirmware.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include "DebugLog.h"

namespace {
constexpr uint32_t kPairingMs = 30000;
constexpr uint32_t kPortalMs = 30000;
constexpr uint32_t kLedMs = 25;
portMUX_TYPE ledMux = portMUX_INITIALIZER_UNLOCKED;

void claimPin(uint8_t pin) {
    gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(gpio);
    gpio_reset_pin(gpio);
}

void ledLevel(uint8_t pin, int level) {
    gpio_set_level(static_cast<gpio_num_t>(pin), level);
}
}

void FrontPanel::attach(Connections* nextConnections, Hid* nextHid, UltimateBaseFirmware* nextFirmware) {
    connections = nextConnections;
    hid = nextHid;
    firmware = nextFirmware;
}

void FrontPanel::begin() {
    loadRadio();
    // GPIO 39-42 are the ESP32-S3 pad-JTAG pins. USB serial/JTAG should
    // release them, but they still come up as JTAG until reset.
    claimPin(RGB_DATA_IN);
    claimPin(RGB_CLOCK_IN);
    claimPin(BLUE_BUTTON);
    claimPin(RED_BUTTON);
    claimPin(GREEN_BUTTON);
    pinMode(RGB_DATA_IN, OUTPUT);
    pinMode(RGB_CLOCK_IN, OUTPUT);
    gpio_set_drive_capability(static_cast<gpio_num_t>(RGB_DATA_IN), GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(static_cast<gpio_num_t>(RGB_CLOCK_IN), GPIO_DRIVE_CAP_3);
    ledLevel(RGB_DATA_IN, 0);
    ledLevel(RGB_CLOCK_IN, 0);
    pinMode(BLUE_BUTTON, INPUT_PULLUP);
    pinMode(RED_BUTTON, INPUT_PULLUP);
    pinMode(GREEN_BUTTON, INPUT_PULLUP);
    blue.setPressMs(800);
    red.setPressMs(800);
    green.setPressMs(800);
    blue.attachClick([](void* ctx) { static_cast<FrontPanel*>(ctx)->onBlueClick(); }, this);
    blue.attachLongPressStart([](void* ctx) { static_cast<FrontPanel*>(ctx)->onBlueLong(); }, this);
    red.attachClick([](void* ctx) { static_cast<FrontPanel*>(ctx)->onRedClick(); }, this);
    red.attachLongPressStart([](void* ctx) { static_cast<FrontPanel*>(ctx)->onRedLong(); }, this);
    green.attachClick([](void* ctx) { static_cast<FrontPanel*>(ctx)->onGreenClick(); }, this);
    green.attachLongPressStart([](void* ctx) { static_cast<FrontPanel*>(ctx)->onGreenLong(); }, this);
    updateLeds();
    debugLog("front leds bt=%u wifi=%u", btOn ? 1u : 0u, wifiOn ? 1u : 0u);
}

void FrontPanel::tick() {
    blue.tick();
    red.tick();
    green.tick();
    if (wifiPortalUntil != 0 && (int32_t)(millis() - wifiPortalUntil) >= 0) {
        stopWifiPortal();
    }
    if (firmware != nullptr && wifiPortalUntil != 0 && !firmware->setupPortalOpen()) {
        wifiPortalUntil = 0;
        debugLog("wifi portal closed");
    }
    uint32_t now = millis();
    if (lastLedMs == 0 || (int32_t)(now - lastLedMs) >= (int32_t)kLedMs) {
        lastLedMs = now;
        updateLeds();
    }
}

bool FrontPanel::wifiPortalOpen() const {
    return wifiPortalUntil != 0 && (int32_t)(millis() - wifiPortalUntil) < 0;
}

void FrontPanel::loadRadio() {
    Preferences prefs;
    if (!prefs.begin("radio", true)) {
        return;
    }
    btOn = prefs.getBool("bt", true);
    wifiOn = prefs.getBool("wifi", true);
    prefs.end();
}

void FrontPanel::saveRadio() {
    Preferences prefs;
    if (!prefs.begin("radio", false)) {
        return;
    }
    prefs.putBool("bt", btOn);
    prefs.putBool("wifi", wifiOn);
    prefs.end();
}

void FrontPanel::onBlueClick() {
    if (actionRulesClaimPanel("blue", "press")) {
        actionRulesOnPanel("blue", "press");
        return;
    }
    if (connections == nullptr) {
        return;
    }
    if (!btOn) {
        applyBluetooth(true);
        return;
    }
    if (connections->pairingWindowOpen()) {
        connections->closePairingWindow();
        return;
    }
    connections->openPairingWindow(kPairingMs);
}

void FrontPanel::onBlueLong() {
    if (actionRulesClaimPanel("blue", "long")) {
        actionRulesOnPanel("blue", "long");
        return;
    }
    applyBluetooth(!btOn);
}

void FrontPanel::onRedClick() {
    if (actionRulesClaimPanel("red", "press")) {
        actionRulesOnPanel("red", "press");
        return;
    }
    if (connections == nullptr) {
        return;
    }
    Connections::UsbPortMode next = Connections::UsbPortMode::Host;
    if (connections->usbHostOn()) {
        next = Connections::UsbPortMode::Peripheral;
    } else if (connections->usbDeviceOn() || connections->usbHidOn()) {
        next = Connections::UsbPortMode::Jtag;
    }
    connections->setUsbPortMode(next);
}

void FrontPanel::onRedLong() {
    if (actionRulesClaimPanel("red", "long")) {
        actionRulesOnPanel("red", "long");
        return;
    }
    if (connections == nullptr) {
        return;
    }
    connections->setUsbPortMode(Connections::UsbPortMode::Jtag);
}

void FrontPanel::onGreenClick() {
    if (actionRulesClaimPanel("green", "press")) {
        actionRulesOnPanel("green", "press");
        return;
    }
    applyWifi(!wifiOn);
}

void FrontPanel::onGreenLong() {
    if (actionRulesClaimPanel("green", "long")) {
        actionRulesOnPanel("green", "long");
        return;
    }
    if (!wifiOn) {
        applyWifi(true);
    }
    startWifiPortal();
}

void FrontPanel::applyBluetooth(bool on) {
    if (connections == nullptr) {
        return;
    }
    btOn = on;
    saveRadio();
    connections->setBluetoothRadio(on);
}

void FrontPanel::applyWifi(bool on) {
    wifiOn = on;
    saveRadio();
    stopWifiPortal();
    if (firmware != nullptr) {
        if (on) {
            firmware->enableWifiRadio();
        } else {
            firmware->disableWifiRadio();
        }
    } else if (!on) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
    }
    debugLog("wifi radio %s", on ? "on" : "off");
}

void FrontPanel::startWifiPortal() {
    if (firmware == nullptr || !wifiOn) {
        return;
    }
    if (!firmware->startSetupPortal(kPortalMs)) {
        debugLog("wifi portal start failed");
        return;
    }
    wifiPortalUntil = millis() + kPortalMs;
    debugLog("wifi portal %u s", (unsigned)(kPortalMs / 1000));
}

void FrontPanel::stopWifiPortal() {
    wifiPortalUntil = 0;
    if (firmware != nullptr) {
        firmware->stopSetupPortal();
    }
}

void FrontPanel::updateLeds() {
    if (connections != nullptr) {
        btOn = connections->bluetoothRadioOn();
    }
    if (firmware != nullptr) {
        wifiOn = firmware->wifiRadioEnabled();
    }
    const bool wifiLive = wifiOn || WiFi.status() == WL_CONNECTED;
    const bool btLive = btOn;

    if (!btLive) {
        setLed(LedId::Blue, 0);
    } else if (connections != nullptr && connections->pairingWindowOpen()) {
        setLed(LedId::Blue, blinkOn(400) ? 255 : 0);
    } else {
        setLed(LedId::Blue, 255);
    }

    if (UsbMidiHost::running()) {
        uint32_t period = usbHostConnected() ? 800 : 160;
        setLed(LedId::Red, blinkOn(period) ? 255 : 0);
    } else if ((hid != nullptr && hid->usbRunning()) || UsbMidi::running()) {
        if (usbPeripheralConnected()) {
            setLed(LedId::Red, wave(128, 255, 700));
        } else {
            setLed(LedId::Red, 255);
        }
    } else {
        setLed(LedId::Red, 0);
    }

    setLed(LedId::Green, wifiLive ? 255 : 0);
    writeLeds();
}

void FrontPanel::setLed(LedId id, uint8_t brightness) {
    ledBri[static_cast<uint8_t>(id)] = brightness;
}

void FrontPanel::sendByte(uint8_t value) {
    for (int bit = 7; bit >= 0; bit--) {
        ledLevel(RGB_DATA_IN, (value >> bit) & 0x01);
        ledLevel(RGB_CLOCK_IN, 1);
        esp_rom_delay_us(1);
        ledLevel(RGB_CLOCK_IN, 0);
        esp_rom_delay_us(1);
    }
}

void FrontPanel::writeLeds() {
    // One P9813, three channels: R=red button, G=green button, B=blue button.
    uint8_t r = ledBri[static_cast<uint8_t>(LedId::Red)];
    uint8_t g = ledBri[static_cast<uint8_t>(LedId::Green)];
    uint8_t b = ledBri[static_cast<uint8_t>(LedId::Blue)];
    uint8_t flag = static_cast<uint8_t>(
        0xC0
        | ((~b & 0xC0) >> 6)
        | ((~g & 0xC0) >> 4)
        | ((~r & 0xC0) >> 2));
    portENTER_CRITICAL(&ledMux);
    sendByte(0);
    sendByte(0);
    sendByte(0);
    sendByte(0);
    sendByte(flag);
    sendByte(b);
    sendByte(g);
    sendByte(r);
    sendByte(0);
    sendByte(0);
    sendByte(0);
    sendByte(0);
    ledLevel(RGB_CLOCK_IN, 0);
    ledLevel(RGB_DATA_IN, 0);
    portEXIT_CRITICAL(&ledMux);
}

uint8_t FrontPanel::wave(uint8_t low, uint8_t high, uint32_t periodMs) const {
    if (high < low || periodMs == 0) {
        return low;
    }
    uint32_t phase = millis() % periodMs;
    uint32_t half = periodMs / 2;
    uint32_t span = static_cast<uint32_t>(high - low);
    if (phase <= half) {
        return static_cast<uint8_t>(low + (span * phase) / (half == 0 ? 1 : half));
    }
    uint32_t down = phase - half;
    return static_cast<uint8_t>(high - (span * down) / (half == 0 ? 1 : half));
}

bool FrontPanel::blinkOn(uint32_t periodMs) const {
    if (periodMs < 2) {
        return true;
    }
    return (millis() / (periodMs / 2)) & 1;
}

bool FrontPanel::usbHostConnected() const {
    return UsbMidiHost::connected();
}

bool FrontPanel::usbPeripheralConnected() const {
    if (hid != nullptr && hid->usbReady()) {
        return true;
    }
    return UsbMidi::mounted();
}
