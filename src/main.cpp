#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include <UltimateBaseFirmware.h>
#include <MultiPrint.h>
#include "pin_definitions.h"
#include "MackieShowbox/MackieShowbox.h"
#include "TwoButtonLooper/TwoButtonLooper.h"
#include "SnapshotLoader/SnapshotLoader.h"
#include "BoosterPedal/BoosterPedal.h"
#include "MainMutePedal/MainMutePedal.h"
#include "MidiBoostLoop/MidiBoostLoop.h"
#include "MultiMidi/UsbMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include "UsbHid/UsbHid.h"
#include <Preferences.h>
#include "RestApiRouter/RestApiRouter.h"
#include "Connections/Connections.h"
#include "MultiMidi/MultiMidi.h"
#include "XtouchCompactAdapter/XtouchCompactAdapter.h"
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_heap_caps.h>

// Pins for double footswitch to control the looper
constexpr uint8_t RECORD_BUTTON_PIN = TRS1_TIP; // Looper Record/Overdub/Play button GPIO pin
constexpr uint8_t STOP_BUTTON_PIN = TRS1_RING; // Looper Stop/Delete button GPIO pin

// Pin for booster pedal button
constexpr uint8_t BOOSTER_PEDAL_PIN = TRS2_TIP; //RED_BUTTON; //TRS2_TIP; // Booster pedal GPIO pin

// Pin for mute pedal button
constexpr uint8_t MUTE_PEDAL_PIN = TRS3_TIP; //BLUE_BUTTON; //TRS3_TIP; // Mute pedal GPIO pin

// Pins for snapshot buttons
constexpr uint8_t SNAPSHOT_BUTTON_PIN1 = TRS4_TIP; // Snapshot button 1 GPIO pin
constexpr uint8_t SNAPSHOT_BUTTON_PIN2 = TRS4_RING; // Snapshot button 2 GPIO pin

Print *Debug = &Serial;
UltimateBaseFirmware ubfw("ShowboxExpansionModule Firmware");
MackieShowbox showbox(SHOWBOX_BASE_RX, SHOWBOX_BASE_TX, SHOWBOX_MIXER_RX, SHOWBOX_MIXER_TX);
TwoButtonLooper looper(RECORD_BUTTON_PIN, STOP_BUTTON_PIN, &showbox);
MainMutePedal mutePedal(MUTE_PEDAL_PIN, &showbox);
BoosterPedal boosterPedal(BOOSTER_PEDAL_PIN, &showbox, entity_id::INPUT4_GAIN);
SnapshotLoader snapshotLoader(SNAPSHOT_BUTTON_PIN1, SNAPSHOT_BUTTON_PIN2, &showbox);
RestApiRouter restApiRouter;
AsyncWebServer webServer(80);
MultiMidi multiMidi;
Connections connections;
XtouchCompactAdapter xtouchAdapter(&showbox);

// Control Change presses, MIDI channel 16 (0-based channel 15).
// A press sends value 127. A release of 0 is ignored.
// Channel 2 gain moves by three input-gain steps while boost is on.
constexpr uint8_t MIDI_BOOST_CHANNEL = 15;
constexpr uint8_t MIDI_BOOST_REC_PLAY_OVERDUB_CC = 102;
constexpr uint8_t MIDI_BOOST_STOP_CC = 103;
constexpr uint8_t MIDI_BOOST_DELETE_CC = 104;
constexpr uint8_t MIDI_BOOST_ON_CC = 105;
constexpr uint8_t MIDI_BOOST_OFF_CC = 106;
MidiBoostLoop midiBoost(&showbox, MIDI_BOOST_CHANNEL,
    MIDI_BOOST_REC_PLAY_OVERDUB_CC, MIDI_BOOST_STOP_CC, MIDI_BOOST_DELETE_CC,
    MIDI_BOOST_ON_CC, MIDI_BOOST_OFF_CC);

enum class UsbBoot {
    SerialJtag,
    MidiDevice,
    MidiHost,
    HidPeripheral
};

UsbBoot usbBootMode() {
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return UsbBoot::SerialJtag;
    }
    bool host = prefs.getBool("usbhost", false);
    bool hid = prefs.getBool("usbhid", false);
    bool device = prefs.getBool("usbdev", false);
    prefs.end();
    if (host) {
        return UsbBoot::MidiHost;
    }
    if (hid) {
        return UsbBoot::HidPeripheral;
    }
    if (device) {
        return UsbBoot::MidiDevice;
    }
    return UsbBoot::SerialJtag;
}

void clearUsbMode() {
    Preferences prefs;
    if (!prefs.begin("midi", false)) {
        return;
    }
    prefs.putBool("usbdev", false);
    prefs.putBool("usbhost", false);
    prefs.putBool("usbhid", false);
    prefs.end();
}

void setup() {
    UsbBoot usbBoot = usbBootMode();
    #ifdef SHOWBOX_DEBUG
        if (usbBoot == UsbBoot::SerialJtag) {
            Serial.begin(921600);
        } else {
            Serial0.begin(115200);
            ubfw.setCustomDebugSerial(Serial0);
        }
    #endif
    //delay(2000);

    if (strcmp(SXM_VERSION, "") == 0) {
        ubfw.setVersion("0.0.0");
    } else {
        ubfw.setVersion(SXM_VERSION);
    }
    ubfw.setHostName("showbox");
    ubfw.setCustomServer(&webServer);
    ubfw.begin();
    WiFi.setAutoReconnect(true);
    Debug = &ubfw.Debug;
    // Routes, OTA, and mDNS are armed on the first Wi-Fi-up tick. Do that
    // before Bluetooth allocates, or mDNS comes up with no internal heap and
    // the module reboots.
    ubfw.tick();
    // The async TCP task needs a contiguous internal block. Bluetooth later
    // leaves too little, so listen while the heap is still free.
    webServer.begin();
    bool usbStarted = true;
    if (usbBoot == UsbBoot::MidiDevice) {
        usbStarted = UsbMidi::start();
    } else if (usbBoot == UsbBoot::MidiHost) {
        usbStarted = UsbMidiHost::start();
    } else if (usbBoot == UsbBoot::HidPeripheral) {
        usbStarted = UsbHid::start();
    }
    if (usbBoot != UsbBoot::SerialJtag && !usbStarted) {
        clearUsbMode();
        Debug->println("[ FAIL ] USB mode did not start. Rebooting with the serial/JTAG port.");
        delay(200);
        ESP.restart();
    }
    if (usbBoot != UsbBoot::SerialJtag) {
        Debug->println("[  OK  ] USB serial/JTAG is off. OTA and the UART connector can still flash.");
    }

    Debug->println("[ INFO ] Initializing MultiMidi...");
    multiMidi.setDebugSerial(Debug);
    connections.attach(&multiMidi);
    connections.load();
    multiMidi.setHostPolicy(&connections);
    multiMidi.enableHardwareMidi(MIDI_RX, MIDI_TX);
    multiMidi.enableAppleMidi(5004);
    multiMidi.enableBleMidi("ShowboxMod");
    multiMidi.begin();
    connections.startAfterBle();
    multiMidi.finishBle();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    wifi_config_t wifiConfig = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifiConfig) == ESP_OK) {
        wifiConfig.sta.listen_interval = 1;
        esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    }
    Debug->println("[  OK  ] MultiMidi initialized.");

    Debug->println("[ INFO ] Initializing Showbox MIDI Adapter...");
    xtouchAdapter.setDebugSerial(Debug);
    xtouchAdapter.setMidi(&multiMidi);
    xtouchAdapter.begin();
    Debug->println("[  OK  ] Showbox MIDI Adapter initialized.");

    Debug->println("[ INFO ] Initializing MIDI boost/loop...");
    midiBoost.setDebugSerial(Debug);
    midiBoost.setNextHandlers(
        XtouchCompactAdapter::onNoteOn,
        XtouchCompactAdapter::onNoteOff,
        XtouchCompactAdapter::onControlChange,
        XtouchCompactAdapter::onPitchBend);
    midiBoost.begin(&multiMidi);
    Debug->println("[  OK  ] MIDI boost/loop initialized.");

    Debug->println("[ INFO ] Initializing Showbox...");
    showbox.setDebugSerial(Debug);
    showbox.begin();
    Debug->println("[  OK  ] Showbox initialized.");

    Debug->println("[ INFO ] Initializing API Router...");
    restApiRouter.setDebugSerial(Debug);
    restApiRouter.setWebServer(&webServer);
    restApiRouter.setShowbox(&showbox);
    restApiRouter.setConnections(&connections);
    restApiRouter.setup();
    Debug->println("[  OK  ] API Router initialized.");

    Debug->println("[ INFO ] Initializing Looper...");
    looper.setDebugSerial(Debug);
    looper.begin();
    Debug->println("[  OK  ] Looper initialized.");

    Debug->println("[ INFO ] Initializing Snapshot Loader...");
    snapshotLoader.setDebugSerial(Debug);
    snapshotLoader.begin();
    Debug->println("[  OK  ] Snapshot Loader initialized.");

    Debug->println("[ INFO ] Initializing Mute Pedal...");
    mutePedal.setDebugSerial(Debug);
    mutePedal.begin();
    Debug->println("[  OK  ] Mute Pedal initialized.");

    Debug->println("[ INFO ] Initializing Booster Pedal...");
    boosterPedal.setDebugSerial(Debug);
    boosterPedal.begin();
    Debug->println("[  OK  ] Booster Pedal initialized.");

    Debug->print("[ INFO ] IP Address: ");
    Debug->println(WiFi.localIP());
    
    Debug->println("[ INFO ] Starting Web Server...");
    webServer.begin();
    Debug->printf("[  OK  ] Web Server started. Internal heap %u, largest %u.\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

unsigned long lastPrint = 0;
unsigned long lastWifiFix = 0;
bool wifiWasDown = false;

void refreshMdns() {
    MDNS.end();
    if (!MDNS.begin("showbox")) {
        return;
    }
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("telnet", "tcp", 23);
    MDNS.enableArduino(3232);
}

void keepWifi() {
    if (millis() - lastWifiFix < 5000) {
        return;
    }
    lastWifiFix = millis();
    wifi_ap_record_t ap;
    bool associated = WiFi.status() == WL_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    static int misses = 0;
    bool up = associated;
    if (up) {
        misses = 0;
        if (wifiWasDown) {
            wifiWasDown = false;
            Debug->printf("[ INFO ] WiFi back at %s\n", WiFi.localIP().toString().c_str());
            refreshMdns();
        }
        return;
    }
    if (++misses < 3) {
        return;
    }
    misses = 0;
    wifiWasDown = true;
    Debug->printf("[ INFO ] WiFi link is down (status %d rssi %d). Reconnecting.\n", WiFi.status(), WiFi.RSSI());
    WiFi.disconnect(false, false);
    WiFi.reconnect();
}

void loop() {
    ubfw.tick();
    keepWifi();
    showbox.tick();
    looper.tick();
    snapshotLoader.tick();
    mutePedal.tick();
    boosterPedal.tick();
    midiBoost.tick();
    multiMidi.tick();
    connections.tick();
    xtouchAdapter.tick();
    
    if (millis() - lastPrint > 3000) {
        lastPrint = millis();
        Debug->printf("[ INFO ] Heartbeat wifi=%d rssi=%d ip=%s\n", WiFi.status(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
        //multiMidi.noteOn(0, 60, 127);
        //multiMidi.noteOff(0, 60, 127);

        // for (uint8_t i = 1; i < 10; i++) {
        //     Debug->printf("Setting fader %d to 0\n", i);
        //     multiMidi.controlChange(i, 0);
        // }

        // Set encoder behavior
        /*for (uint8_t i = 0; i < 16; i++) {
            uint8_t value = (i == 9 || i == 10 || i == 13) ? 4 : 2;
            Debug->printf("Setting encoder %d to behavior %d\n", i, value);
            xtouchAdapter.setLedRing(i, value);
        }

        // Turn off button leds
        for (uint8_t i = 0; i < 39; i++) {
            Debug->printf("Turning off button %d led\n", i);
            xtouchAdapter.setButtonLed(i, false);
        }

        // Turn off encoder leds
        for (uint8_t i = 0; i < 16; i++) {
            Debug->printf("Turning off encoder %d led\n", i);
            xtouchAdapter.setLedRing(i, 0);
        }

        // Move all faders to the bottom
        for (uint8_t i = 0; i < 9; i++) {
            Debug->printf("Setting fader %d to 0\n", i);
            xtouchAdapter.setFader(i, 0);
        }*/
    }
}