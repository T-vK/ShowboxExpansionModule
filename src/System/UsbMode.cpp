#include "System/UsbMode.h"

#include "MultiMidi/UsbMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include "System/IsoPolicy.h"
#include "DebugLog.h"
#include <Hid.h>
#include <Preferences.h>
#include <esp_timer.h>
#include "esp_private/usb_phy.h"

extern Hid hid;
extern Print* Debug;

UsbBoot usbBoot = UsbBoot::SerialJtag;
bool usbWantMidi = false;
bool usbWantHid = false;
bool usbTestBoot = false;
bool usbFailCleared = false;

namespace {
esp_timer_handle_t usbSafetyTimer = nullptr;

void usbSafetyTimerCb(void*) {
    usbModeClear();
    usbRestoreJtag();
    delay(50);
    ESP.restart();
}
}

UsbBoot usbBootMode() {
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return UsbBoot::SerialJtag;
    }
    bool host = prefs.getBool("usbhost", false);
    bool hidMode = prefs.getBool("usbhid", false);
    bool device = prefs.getBool("usbdev", false);
    prefs.end();
    usbWantMidi = device;
    usbWantHid = hidMode;
    debugLog("usb boot prefs host=%d hid=%d dev=%d", host ? 1 : 0, hidMode ? 1 : 0, device ? 1 : 0);
    if (host) {
        return UsbBoot::MidiHost;
    }
    if (hidMode) {
        return UsbBoot::HidPeripheral;
    }
    if (device) {
        return UsbBoot::MidiDevice;
    }
    return UsbBoot::SerialJtag;
}

void usbModeClear() {
    Preferences prefs;
    if (!prefs.begin("midi", false)) {
        return;
    }
    prefs.putBool("usbdev", false);
    prefs.putBool("usbhost", false);
    prefs.putBool("usbhid", false);
    prefs.putBool("usbtest", false);
    prefs.putInt("usbfail", 0);
    prefs.end();
}

void usbRestoreJtag() {
    usb_phy_config_t phyConfig = {};
    phyConfig.controller = USB_PHY_CTRL_SERIAL_JTAG;
    phyConfig.target = USB_PHY_TARGET_INT;
    usb_phy_handle_t phy = nullptr;
    usb_new_phy(&phyConfig, &phy);
}

void usbRevertToJtag(const char* reason) {
    usbModeClear();
    if (Debug != nullptr) {
        Debug->println(reason);
    }
    usbRestoreJtag();
    delay(300);
    ESP.restart();
}

void usbArmSafetyTimer(uint32_t ms) {
    const esp_timer_create_args_t args = {
        .callback = &usbSafetyTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "usb-safe",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &usbSafetyTimer) != ESP_OK) {
        return;
    }
    esp_timer_start_once(usbSafetyTimer, static_cast<uint64_t>(ms) * 1000ULL);
}

void usbLoadIsoAndFailCount() {
    Preferences isoPrefs;
    if (isoPrefs.begin("debug", true)) {
        gIso = isoPrefs.getInt("iso", 0);
        isoPrefs.end();
    }
#if UART_SERVICE_LOCK
    gIso = 3;
#endif
    Preferences prefs;
    if (!prefs.begin("midi", false)) {
        return;
    }
    usbTestBoot = prefs.getBool("usbtest", false);
    // Do not auto-clear usbhost/usbdev after failed starts — that made MIDI
    // host look like it never stuck. usbStartSelected() falls back to JTAG
    // for the current boot only; the user preference stays until changed.
    if (usbBoot != UsbBoot::SerialJtag) {
        prefs.putInt("usbfail", 0);
    }
    prefs.end();
}

bool usbStartSelected() {
    bool usbStarted = true;
    if (usbTestBoot) {
        usbArmSafetyTimer(75000);
    }
    if (usbBoot == UsbBoot::MidiDevice) {
        usbStarted = UsbMidi::start();
    } else if (usbBoot == UsbBoot::MidiHost) {
        usbStarted = UsbMidiHost::start();
    } else if (usbBoot == UsbBoot::HidPeripheral) {
        if (usbWantMidi) {
            UsbMidi::enableInterface();
        }
        usbStarted = hid.startUsb();
        if (usbWantMidi && usbStarted) {
            UsbMidi::markStarted();
        }
    }
    if (usbBoot != UsbBoot::SerialJtag && !usbStarted) {
        // Keep the preferred mode in NVS so the next boot retries (and the
        // UI can show host/gadget as selected). Only fall back to JTAG for
        // this boot so Ethernet/Wi‑Fi stay reachable.
        if (Debug != nullptr) {
            Debug->println("[ FAIL ] USB mode did not start; keeping NVS preference, using serial/JTAG this boot.");
        }
        usbRestoreJtag();
        usbBoot = UsbBoot::SerialJtag;
    }
    if (usbBoot != UsbBoot::SerialJtag && Debug != nullptr) {
        Debug->println("[  OK  ] USB serial/JTAG is off. OTA and the UART connector can still flash.");
        if (usbTestBoot) {
            Debug->println("[ INFO ] USB peripheral test: serial/JTAG returns automatically after about 75 seconds.");
        }
    }
    return usbStarted;
}

void usbTickWatch() {
    if (usbBoot != UsbBoot::SerialJtag && !usbFailCleared && millis() > 20000) {
        usbFailCleared = true;
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            prefs.putInt("usbfail", 0);
            prefs.end();
        }
    }
    if (usbTestBoot && millis() > 75000) {
        usbRevertToJtag("[ INFO ] USB peripheral test finished. Rebooting with the serial/JTAG port.");
    }
}
