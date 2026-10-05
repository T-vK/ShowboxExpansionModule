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
#include <Hid.h>
#include <Preferences.h>
#include "RestApiRouter/RestApiRouter.h"
#include "Actions/ActionRules.h"
#include "Connections/Connections.h"
#include "MultiMidi/MultiMidi.h"
#include <BleRadio.h>
#include "XtouchCompactAdapter/XtouchCompactAdapter.h"
#include "FrontPanel/FrontPanel.h"
#include "WiredNet/WiredNet.h"
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_coexist.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "esp_private/usb_phy.h"
#include "DebugLog.h"

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

UltimateBaseFirmware ubfw("ShowboxExpansionModule Firmware");
Print *Debug = &ubfw.Debug;
MackieShowbox showbox(SHOWBOX_BASE_RX, SHOWBOX_BASE_TX, SHOWBOX_MIXER_RX, SHOWBOX_MIXER_TX);
TwoButtonLooper looper(RECORD_BUTTON_PIN, STOP_BUTTON_PIN, &showbox);
MainMutePedal mutePedal(MUTE_PEDAL_PIN, &showbox);
BoosterPedal boosterPedal(BOOSTER_PEDAL_PIN, &showbox, entity_id::INPUT4_GAIN);
SnapshotLoader snapshotLoader(SNAPSHOT_BUTTON_PIN1, SNAPSHOT_BUTTON_PIN2, &showbox);
RestApiRouter restApiRouter;
ActionRules actionRules;
AsyncWebServer webServer(80);
MultiMidi multiMidi;
Hid hid;
BleRadio bleRadio;
Connections connections;
XtouchCompactAdapter xtouchAdapter(&showbox);
FrontPanel frontPanel;
WiredNet wiredNet;

// Control Change presses, MIDI channel 16 (0-based channel 15).
// A press sends value 127. A release of 0 is ignored.
// Channel 2 gain moves by four input-gain steps while boost is on.
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

UsbBoot usbBoot = UsbBoot::SerialJtag;
bool usbWantMidi = false;
bool usbWantHid = false;
bool btRadioOn = true;
bool wifiRadioOn = true;

UsbBoot usbBootMode() {
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return UsbBoot::SerialJtag;
    }
    bool host = prefs.getBool("usbhost", false);
    bool hid = prefs.getBool("usbhid", false);
    bool device = prefs.getBool("usbdev", false);
    prefs.end();
    usbWantMidi = device;
    usbWantHid = hid;
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
    prefs.putBool("usbtest", false);
    prefs.putInt("usbfail", 0);
    prefs.end();
}

void applyDebugSettings(bool history, bool packets) {
    ubfw.setWifiLogHistory(history);
    showbox.setPacketLogging(packets);
    showbox.setPacketLog(&ubfw.wifiLog());
}

bool usbTestBoot = false;
bool usbFailCleared = false;
esp_timer_handle_t usbSafetyTimer = nullptr;

bool isoBle() { return (gIso == 0 || gIso >= 10) && btRadioOn; }
bool isoApple() { return gIso == 0 || gIso == 1 || gIso >= 10; }
bool isoDin() { return gIso == 0 || gIso == 1 || gIso == 2 || gIso == 4 || gIso >= 10; }
bool isoExtras() { return gIso == 0 || gIso == 1 || gIso == 2 || gIso == 5 || gIso >= 10; }
bool isoOutgoing() { return gIso == 0 || gIso == 12; }
bool isoHid() { return gIso == 0 || gIso == 13; }

constexpr const char* kBleName = "ShowboxMod";

void restoreUsbSerialJtag() {
    usb_phy_config_t phyConfig = {};
    phyConfig.controller = USB_PHY_CTRL_SERIAL_JTAG;
    phyConfig.target = USB_PHY_TARGET_INT;
    usb_phy_handle_t phy = nullptr;
    usb_new_phy(&phyConfig, &phy);
}

void revertUsbToJtag(const char* reason) {
    clearUsbMode();
    if (Debug != nullptr) {
        Debug->println(reason);
    }
    restoreUsbSerialJtag();
    delay(300);
    ESP.restart();
}

void usbSafetyTimerCb(void*) {
    clearUsbMode();
    restoreUsbSerialJtag();
    delay(50);
    ESP.restart();
}

void armUsbSafetyTimer(uint32_t ms) {
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

void refreshMdns();
void queueMdnsRefresh();
void keepMdns();
volatile bool gMdnsUp = false;
volatile bool gSetupDone = false;
volatile int gSoakSeconds = 0;
volatile bool gSoakRunning = false;
static char gSoakJson[512] = "{\"done\":false}";

bool uartSoakResult(char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    strncpy(out, gSoakJson, cap - 1);
    out[cap - 1] = 0;
    return gSoakJson[0] != 0;
}

extern "C" void bt_osi_psram_stats(uint32_t* hits, uint32_t* misses, uint32_t* bytes);

void setup() {
    usbBoot = usbBootMode();
    {
        Preferences radioPrefs;
        if (radioPrefs.begin("radio", true)) {
            btRadioOn = radioPrefs.getBool("bt", true);
            wifiRadioOn = radioPrefs.getBool("wifi", true);
            radioPrefs.end();
        }
        if (!wifiRadioOn) {
            ubfw.setWifiRadioEnabled(false);
        }
        if (!btRadioOn) {
            esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        }
    }
    {
        Preferences isoPrefs;
        if (isoPrefs.begin("debug", true)) {
            gIso = isoPrefs.getInt("iso", 0);
            isoPrefs.end();
        }
#if UART_SERVICE_LOCK
        gIso = 3;
#endif
    }
    {
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            int fails = prefs.getInt("usbfail", 0);
            usbTestBoot = prefs.getBool("usbtest", false);
            if (usbBoot != UsbBoot::SerialJtag && fails >= 2) {
                prefs.putBool("usbdev", false);
                prefs.putBool("usbhost", false);
                prefs.putBool("usbhid", false);
                prefs.putBool("usbtest", false);
                prefs.putInt("usbfail", 0);
                usbBoot = UsbBoot::SerialJtag;
                usbTestBoot = false;
            } else if (usbBoot != UsbBoot::SerialJtag) {
                prefs.putInt("usbfail", fails + 1);
            }
            prefs.end();
        }
    }
    #ifdef SHOWBOX_DEBUG
        if (usbBoot == UsbBoot::SerialJtag) {
            restoreUsbSerialJtag();
            Serial.begin(921600);
            Serial.setTxTimeoutMs(0);
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
    {
        Preferences debugPrefs;
        if (debugPrefs.begin("debug", true)) {
            ubfw.setWifiLogHistory(debugPrefs.getBool("wifibuf", true));
            showbox.setPacketLogging(debugPrefs.getBool("uartpkt", false));
            debugPrefs.end();
        }
    }
    ubfw.begin();
    wiredNet.begin();
    if (wifiRadioOn) {
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else {
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
    }
    if (!wifiRadioOn && !btRadioOn) {
        debugLog("radios off wifi=0 bt=0");
    }
    Debug = &ubfw.Debug;
    debugLog("iso=%d", gIso);
    pinMode(SHOWBOX_BASE_TX, OUTPUT);
    pinMode(SHOWBOX_MIXER_TX, OUTPUT);
    digitalWrite(SHOWBOX_BASE_TX, HIGH);
    digitalWrite(SHOWBOX_MIXER_TX, HIGH);
    showbox.setPacketLog(&ubfw.wifiLog());
    // Routes, OTA, and mDNS are armed on the first Wi-Fi-up tick. Do that
    // before Bluetooth allocates, or mDNS comes up with no internal heap and
    // the module reboots.
    ubfw.tick();
    debugLog("wifi ip=%s heap=%u largest=%u",
        WiFi.localIP().toString().c_str(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#if UART_SERVICE_LOCK
    esp_wifi_set_max_tx_power(20);
#endif
    // The async TCP task needs a contiguous internal block. Bluetooth later
    // leaves too little, so listen while the heap is still free.
    webServer.begin();
    // NimBLE C++ maps and Strings are hundreds of small mallocs. The default
    // 4096-byte internal cutoff left largest=12 and HTTP/AppleMIDI died once
    // AIRSTEP was up. 64 still keeps lwIP pbufs in DIRAM (they use DMA caps).
    heap_caps_malloc_extmem_enable(64);
    hid.setDebug(Debug);
    hid.setIdentity(kBleName, kBleName);
    bool usbStarted = true;
    if (usbTestBoot) {
        // Arm before USB.begin(). If TinyUSB hangs, this still restores JTAG.
        armUsbSafetyTimer(75000);
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
        clearUsbMode();
        Debug->println("[ FAIL ] USB mode did not start. Rebooting with the serial/JTAG port.");
        restoreUsbSerialJtag();
        delay(200);
        ESP.restart();
    }
    if (usbBoot != UsbBoot::SerialJtag) {
        Debug->println("[  OK  ] USB serial/JTAG is off. OTA and the UART connector can still flash.");
        if (usbTestBoot) {
            Debug->println("[ INFO ] USB peripheral test: serial/JTAG returns automatically after about 75 seconds.");
        }
    }

    Debug->println("[ INFO ] Initializing Showbox...");
    showbox.setDebugSerial(Debug);
    showbox.begin();
    Debug->println("[  OK  ] Showbox UART is forwarding.");

    // Register HTTP routes before Bluetooth. BLE used to run first and leave
    // /services as HTTP 500 empty until setup() finished two minutes later.
    restApiRouter.setDebugSerial(Debug);
    restApiRouter.setWebServer(&webServer);
    restApiRouter.setShowbox(&showbox);
    restApiRouter.setConnections(&connections);
    restApiRouter.setActions(&actionRules);
    restApiRouter.setup();
    keepMdns();
    debugLog("routes heap=%u largest=%u mdns=%d",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        gMdnsUp ? 1 : 0);
    Debug->println("[  OK  ] API routes and showbox.local are up before Bluetooth.");

    Debug->println("[ INFO ] Initializing MultiMidi...");
    bleRadio.setName(kBleName);
    multiMidi.setDebugSerial(Debug);
    multiMidi.setRadio(&bleRadio);
    multiMidi.setEthernetPins(W5500_SCLK, W5500_MISO, W5500_MOSI, W5500_nSS, W5500_nRESET);
    connections.attach(&multiMidi, &bleRadio, &hid);
    connections.setShowbox(&showbox);
    connections.setDebugSettingsHandler(applyDebugSettings);
    connections.setLinkChangeHandler(queueMdnsRefresh);
    connections.load();
    multiMidi.setHostPolicy(&connections);
    if (isoDin()) {
        multiMidi.enableHardwareMidi(MIDI_RX, MIDI_TX);
    }
    if (isoApple()) {
        multiMidi.enableAppleMidi(5004);
    }
    if (isoBle()) {
        multiMidi.enableBleMidi(kBleName);
    }
    multiMidi.begin();
    if (isoBle()) {
        connections.startAfterBle();
        multiMidi.finishBle();
        if (isoOutgoing()) {
            multiMidi.allowOutgoing(true);
            debugLog("outgoing on");
        } else {
            multiMidi.allowOutgoing(false);
            debugLog("outgoing off iso=%d", gIso);
        }
        if (!isoHid() && gIso != 0) {
            hid.setBleEnabled(false);
            bleRadio.setHidPeripheral(false);
        }
        if (gIso == 10 || gIso == 12) {
            bleRadio.setAdvertiseAllowed(false);
            bleRadio.apply();
        } else if (gIso == 11 || gIso == 13) {
            bleRadio.setAdvertiseAllowed(true);
            bleRadio.apply();
        }
    }
    // PREFER_WIFI starves BLE GAP connect. Software coexistence is already
    // on; BALANCE is the documented default for Wi-Fi + BLE together.
    if (wifiRadioOn || btRadioOn) {
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    }
    if (wifiRadioOn) {
        wifi_config_t wifiConfig = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &wifiConfig) == ESP_OK) {
            wifiConfig.sta.listen_interval = 1;
            esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
        }
    }
    Debug->printf("[  OK  ] MultiMidi initialized (iso=%d).\n", gIso);
    uint32_t wrapHits = 0, wrapMiss = 0, wrapBytes = 0;
    bt_osi_psram_stats(&wrapHits, &wrapMiss, &wrapBytes);
    debugLog("after ble heap=%u largest=%u psram=%u wrap=%u/%u/%u iso=%d",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)wrapHits, (unsigned)wrapMiss, (unsigned)wrapBytes, gIso);
    if (isoExtras()) {
        debugLog("xtouch begin");
        xtouchAdapter.setDebugSerial(Debug);
        xtouchAdapter.setMidi(&multiMidi);
        xtouchAdapter.begin();
        debugLog("boost begin");
        midiBoost.setDebugSerial(Debug);
        midiBoost.setNextHandlers(
            XtouchCompactAdapter::onNoteOn,
            XtouchCompactAdapter::onNoteOff,
            XtouchCompactAdapter::onControlChange,
            XtouchCompactAdapter::onPitchBend);
        midiBoost.begin(&multiMidi);
        debugLog("actions begin");
        actionRules.setDebugSerial(Debug);
        actionRules.begin(&multiMidi, &showbox, &connections);
        debugLog("looper begin");
        looper.setDebugSerial(Debug);
        looper.begin();
        debugLog("snapshot begin");
        snapshotLoader.setDebugSerial(Debug);
        snapshotLoader.begin();
        debugLog("mute begin");
        mutePedal.setDebugSerial(Debug);
        mutePedal.begin();
        debugLog("booster begin");
        boosterPedal.setDebugSerial(Debug);
        boosterPedal.begin();
    }
    frontPanel.attach(&connections, &hid, &ubfw);
    frontPanel.begin();
    refreshMdns();
    keepMdns();
    if (frontPanel.wifiRadioOn()) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    gSetupDone = true;
    debugLog("setup done heap=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

unsigned long lastPrint = 0;
unsigned long lastWifiFix = 0;
unsigned long lastMdnsRefresh = 0;
bool wifiWasDown = false;
volatile bool mdnsRefreshQueued = false;

void queueMdnsRefresh() {
    mdnsRefreshQueued = true;
}

void keepMdns() {
    if (!lanHasIp()) {
        return;
    }
    if (!gMdnsUp) {
        bool ok = MDNS.begin("showbox");
        IPAddress ip = lanIP();
        debugLog("mdns begin=%d ip=%s heap=%u largest=%u",
            ok ? 1 : 0,
            ip.toString().c_str(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (!ok) {
            mdnsRefreshQueued = true;
            return;
        }
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("telnet", "tcp", 23);
        MDNS.addService("_apple-midi", "_udp", 5004);
        MDNS.enableArduino(3232);
        gMdnsUp = true;
        if (Debug != nullptr) {
            Debug->printf("[  OK  ] mDNS advertised as showbox.local at %s\n", ip.toString().c_str());
        }
    }
    mdnsRefreshQueued = false;
    lastMdnsRefresh = millis();
}

void refreshMdns() {
    keepMdns();
}

void keepWifi() {
    if (!frontPanel.wifiRadioOn() || frontPanel.wifiPortalOpen()) {
        return;
    }
    if (millis() - lastWifiFix < 5000) {
        return;
    }
    lastWifiFix = millis();
    wifi_ap_record_t ap;
    wl_status_t st = WiFi.status();
    bool associated = st == WL_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    static int misses = 0;
    if (associated) {
        misses = 0;
        static int upLogs = 0;
        if (wifiWasDown) {
            wifiWasDown = false;
            upLogs = 0;
            debugLog("wifi back ip=%s rssi=%d heap=%u largest=%u",
                WiFi.localIP().toString().c_str(), ap.rssi,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            Debug->printf("[ INFO ] WiFi back at %s\n", WiFi.localIP().toString().c_str());
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
            refreshMdns();
            lastMdnsRefresh = millis();
        } else if (++upLogs >= 6) {
            upLogs = 0;
            debugLog("wifi up ip=%s rssi=%d heap=%u largest=%u",
                WiFi.localIP().toString().c_str(), ap.rssi,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        return;
    }
    wifiWasDown = true;
    if (++misses < 3) {
        debugLog("wifi down status=%d rssi=%d miss=%d", (int)st, WiFi.RSSI(), misses);
        return;
    }
    misses = 0;
    Debug->printf("[ INFO ] WiFi link is down (status %d rssi %d). Reconnecting.\n", (int)st, WiFi.RSSI());
    // disconnect() emits WIFI_REASON_ASSOC_LEAVE, which Arduino treats as a
    // voluntary leave and will not auto-reconnect. A connect issued in the
    // middle of that disconnect also fails in IDF. If STA is already down,
    // only esp_wifi_connect() via reconnect().
    if (st == WL_CONNECTED) {
        WiFi.disconnect(false, false);
    }
    bool ok = WiFi.reconnect();
    debugLog("wifi reconnect=%d status=%d rssi=%d ip=%s",
        ok ? 1 : 0, (int)WiFi.status(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
}

void loop() {
    static uint32_t lastMdnsKeep = 0;
    if (!gSoakRunning && (mdnsRefreshQueued || lastMdnsKeep == 0 || (int32_t)(millis() - lastMdnsKeep) >= 2000)) {
        lastMdnsKeep = millis();
        keepMdns();
    }
    wiredNet.tick();
    if (lanHasIp() && !ubfw.lanServicesUp()) {
        ubfw.startLanServices();
        queueMdnsRefresh();
    }
    ubfw.tick();
    frontPanel.tick();
    connections.setDebugStatus(ubfw.wifiLogConnected(), ubfw.wifiLogBytes());
    keepWifi();
    showbox.tick();
    if (isoExtras()) {
        looper.tick();
        snapshotLoader.tick();
        mutePedal.tick();
        boosterPedal.tick();
        actionRules.tick();
        xtouchAdapter.tick();
    }
    multiMidi.tick();
    midiBoost.tick();
    connections.tick();
    ubfw.flushWifiLog();

    if (gSoakSeconds > 0 && !gSoakRunning) {
        int seconds = gSoakSeconds;
        gSoakSeconds = 0;
        gSoakRunning = true;
        uint32_t initM0 = showbox.initFromMixer();
        uint32_t initB0 = showbox.initFromBase();
        uint32_t hbM0 = showbox.hbFromMixer();
        uint32_t hbB0 = showbox.hbFromBase();
        uint32_t rxB0 = showbox.rxFromBase();
        uint32_t rxM0 = showbox.rxFromMixer();
        uint32_t maxBase = 0;
        uint32_t maxMix = 0;
        uint32_t deaths = 0;
        bool wasLive = showbox.linkOk(2000);
        debugLog("uart soak start %ds wifi off", seconds);
        delay(200);
        esp_wifi_stop();
        uint32_t t0 = millis();
        uint32_t dur = static_cast<uint32_t>(seconds) * 1000u;
        while ((int32_t)(millis() - t0) < (int32_t)dur) {
            showbox.tick();
            uint32_t ba = showbox.baseAgeMs();
            uint32_t ma = showbox.mixerAgeMs();
            if (ba != 0xFFFFFFFFu && ba > maxBase) {
                maxBase = ba;
            }
            if (ma != 0xFFFFFFFFu && ma > maxMix) {
                maxMix = ma;
            }
            bool live = showbox.linkOk(2000);
            if (wasLive && !live) {
                deaths = deaths + 1;
            }
            wasLive = live;
            delay(20);
        }
        esp_wifi_start();
        WiFi.reconnect();
        snprintf(gSoakJson, sizeof(gSoakJson),
            "{\"done\":true,\"running\":false,\"seconds\":%d,\"deaths\":%u,\"maxBase\":%u,\"maxMix\":%u,"
            "\"initM\":[%u,%u],\"initB\":[%u,%u],\"hbM\":[%u,%u],\"hbB\":[%u,%u],"
            "\"rxB\":[%u,%u],\"rxM\":[%u,%u],\"dropB\":%u,\"dropM\":%u,\"frmB\":%u,\"frmM\":%u,\"ok\":%s}",
            seconds, (unsigned)deaths, (unsigned)maxBase, (unsigned)maxMix,
            (unsigned)initM0, (unsigned)showbox.initFromMixer(),
            (unsigned)initB0, (unsigned)showbox.initFromBase(),
            (unsigned)hbM0, (unsigned)showbox.hbFromMixer(),
            (unsigned)hbB0, (unsigned)showbox.hbFromBase(),
            (unsigned)rxB0, (unsigned)showbox.rxFromBase(),
            (unsigned)rxM0, (unsigned)showbox.rxFromMixer(),
            (unsigned)showbox.droppedFromBase(), (unsigned)showbox.droppedFromMixer(),
            (unsigned)showbox.frameErrFromBase(), (unsigned)showbox.frameErrFromMixer(),
            (deaths == 0 && maxBase < 2000 && maxMix < 2000
                && showbox.rxFromBase() > rxB0 && showbox.rxFromMixer() > rxM0
                && showbox.initFromMixer() == initM0 && showbox.initFromBase() == initB0)
                ? "true" : "false");
        debugLog("uart soak done deaths=%u maxB=%u maxM=%u", (unsigned)deaths, (unsigned)maxBase, (unsigned)maxMix);
        gSoakRunning = false;
    }
    if (gIsoPending >= 0) {
        int level = gIsoPending;
        gIsoPending = -1;
        if (showbox.uartHot()) {
            showbox.waitQuiet(40);
        }
        Preferences prefs;
        if (prefs.begin("debug", false)) {
            prefs.putInt("iso", level);
            prefs.end();
        }
        debugLog("iso save %d reboot", level);
        delay(50);
        ESP.restart();
    }
    if (gIsoRebootAt != 0 && (int32_t)(millis() - gIsoRebootAt) >= 0) {
        gIsoRebootAt = 0;
        ESP.restart();
    }

    if (usbBoot != UsbBoot::SerialJtag && !usbFailCleared && millis() > 20000) {
        usbFailCleared = true;
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            prefs.putInt("usbfail", 0);
            prefs.end();
        }
    }
    if (usbTestBoot && millis() > 75000) {
        revertUsbToJtag("[ INFO ] USB peripheral test finished. Rebooting with the serial/JTAG port.");
    }
    
    if (millis() - lastPrint > 3000) {
        lastPrint = millis();
        // Heartbeat telnet spam is off while debugging MIDI/looper.
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