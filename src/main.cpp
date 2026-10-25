#define SHOWBOX_DEBUG

#include <Arduino.h>
#include <UltimateBaseFirmware.h>
#include <MultiPrint.h>
#include "pin_definitions.h"
#include "MackieShowbox/MackieShowbox.h"
#include "TwoButtonLooper/TwoButtonLooper.h"
#include "SnapshotLoader/SnapshotLoader.h"
#include "BoosterPedal/BoosterPedal.h"
#include "MainMutePedal/MainMutePedal.h"
#include "XtouchCompactAdapter/XtouchCompactAdapter.h"
#include "Actions/ActionRules.h"
#include "MultiMidi/UsbMidi.h"
#include "MultiMidi/UsbMidiHost.h"
#include <Hid.h>
#include <Preferences.h>
#include "RestApiRouter/RestApiRouter.h"
#include "Connections/Connections.h"
#include "MultiMidi/MultiMidi.h"
#include <BleRadio.h>
#include "XtouchCompactAdapter/XtouchCompactAdapter.h"
#include "FrontPanel/FrontPanel.h"
#include "WiredNet/WiredNet.h"
#include "System/UsbMode.h"
#include "System/IsoPolicy.h"
#include "System/LanKeep.h"
#include "System/HttpsGate.h"
#include "System/Diag.h"
#include "LiveSocket/LiveSocket.h"
#include "CaptivePortal/CaptivePortal.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_coexist.h>
#include <esp_heap_caps.h>
#include "DebugLog.h"

constexpr uint8_t RECORD_BUTTON_PIN = TRS1_TIP;
constexpr uint8_t STOP_BUTTON_PIN = TRS1_RING;
constexpr uint8_t BOOSTER_PEDAL_PIN = TRS2_TIP;
constexpr uint8_t MUTE_PEDAL_PIN = TRS3_TIP;
constexpr uint8_t SNAPSHOT_BUTTON_PIN1 = TRS4_TIP;
constexpr uint8_t SNAPSHOT_BUTTON_PIN2 = TRS4_RING;

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
LiveSocket liveSocket;
CaptivePortal captivePortal;

bool btRadioOn = true;
bool wifiRadioOn = true;
constexpr const char* kBleName = "ShowboxMod";

void applyDebugSettings(bool history, bool packets) {
    ubfw.setWifiLogHistory(history);
    showbox.setPacketLogging(packets);
    showbox.setPacketLog(&ubfw.wifiLog());
}

extern "C" void bt_osi_psram_stats(uint32_t* hits, uint32_t* misses, uint32_t* bytes);
extern "C" void wifi_init_wrap_stats(int* called);

void setup() {
    usbBoot = usbBootMode();
    {
        Preferences radioPrefs;
        if (radioPrefs.begin("radio", true)) {
            btRadioOn = radioPrefs.getBool("bt", true);
            wifiRadioOn = radioPrefs.getBool("wifi", true);
            radioPrefs.end();
        }
        httpsLoad();
        if (!wifiRadioOn) {
            ubfw.setWifiRadioEnabled(false);
        }
        if (!btRadioOn) {
            esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        }
    }
    usbLoadIsoAndFailCount();
#ifdef SHOWBOX_DEBUG
    if (usbBoot == UsbBoot::SerialJtag) {
        usbRestoreJtag();
        Serial.begin(921600);
        Serial.setTxTimeoutMs(0);
    } else {
        Serial0.begin(115200);
        ubfw.setCustomDebugSerial(Serial0);
    }
#endif
    if (strcmp(SXM_VERSION, "") == 0) {
        ubfw.setVersion("0.0.0");
    } else {
        ubfw.setVersion(SXM_VERSION);
    }
    ubfw.setHostName("showbox");
    ubfw.setCustomServer(&webServer);
    if (wifiRadioOn) {
        lanInstallWifiEvents();
    }
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
        lanPreferWifiAirtime();
    } else {
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
    }
    if (!wifiRadioOn && !btRadioOn) {
        debugLog("radios off wifi=0 bt=0");
    }
    Debug = &ubfw.Debug;
    diagBegin();
    diagOnLoopStall([](uint32_t stallMs) {
        // Fired from the esp_timer watchdog while the loop is still stuck.
        // A blocked telnet peer is the known offender; drop its socket so the
        // blocked write returns instead of crawling for minutes.
        diagEventForce("loop", "stall %u ms (watchdog)", (unsigned)stallMs);
        ubfw.quarantineTelnet(60000);
        WiFiClient* telnet = ubfw.telnetClient();
        if (telnet != nullptr) {
            telnet->stop();
        }
    });
    {
        Preferences diagPrefs;
        if (diagPrefs.begin("debug", true)) {
            gDiagWatch = diagPrefs.getBool("diag", true);
            diagPrefs.end();
        }
        debugLog("diag watch=%d iso=%d", (int)gDiagWatch, gIso);
    }
    debugLog("iso=%d", gIso);
    pinMode(SHOWBOX_BASE_TX, OUTPUT);
    pinMode(SHOWBOX_MIXER_TX, OUTPUT);
    digitalWrite(SHOWBOX_BASE_TX, HIGH);
    digitalWrite(SHOWBOX_MIXER_TX, HIGH);
    showbox.setPacketLog(&ubfw.wifiLog());
    ubfw.tick();
    debugLog("wifi ip=%s heap=%u largest=%u",
        WiFi.localIP().toString().c_str(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#if UART_SERVICE_LOCK
    esp_wifi_set_max_tx_power(20);
#endif
    webServer.begin();
    heap_caps_malloc_extmem_enable(16);
    hid.setDebug(Debug);
    hid.setIdentity(kBleName, kBleName);
    usbStartSelected();

    Debug->println("[ INFO ] Initializing Showbox...");
    showbox.setDebugSerial(Debug);
    showbox.begin();
    Debug->println("[  OK  ] Showbox UART is forwarding.");

    restApiRouter.setDebugSerial(Debug);
    restApiRouter.setWebServer(&webServer);
    restApiRouter.setShowbox(&showbox);
    restApiRouter.setConnections(&connections);
    restApiRouter.setActions(&actionRules);
    restApiRouter.setLiveSocket(&liveSocket);
    restApiRouter.setCaptivePortal(&captivePortal);
    restApiRouter.setup();
    liveSocket.setConnections(&connections);
    liveSocket.begin(&webServer, &showbox);
    captivePortal.begin(&webServer, &ubfw);
    lanKeepMdns();
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
    lanSetMultiMidi(&multiMidi);
    connections.attach(&multiMidi, &bleRadio, &hid);
    connections.setShowbox(&showbox);
    connections.setDebugSettingsHandler(applyDebugSettings);
    connections.setLinkChangeHandler(lanQueueMdnsRefresh);
    connections.load();
    multiMidi.setHostPolicy(&connections);
    if (isoDin()) {
        multiMidi.enableHardwareMidi(MIDI_RX, MIDI_TX);
    }
    if (isoApple()) {
        multiMidi.enableAppleMidi(5004);
    }
    if (isoBle(btRadioOn)) {
        multiMidi.enableBleMidi(kBleName);
    }
    multiMidi.begin();
    if (isoBle(btRadioOn)) {
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
    if (wifiRadioOn || btRadioOn) {
        lanPreferWifiAirtime();
    }
    if (wifiRadioOn) {
        wifi_config_t wifiConfig = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &wifiConfig) == ESP_OK) {
            wifiConfig.sta.listen_interval = 1;
            esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
        }
    }
    uint32_t wrapHits = 0, wrapMiss = 0, wrapBytes = 0;
    bt_osi_psram_stats(&wrapHits, &wrapMiss, &wrapBytes);
    int wifiWrap = 0;
    wifi_init_wrap_stats(&wifiWrap);
    Debug->printf("[  OK  ] MultiMidi initialized (iso=%d wifiInit=%d).\n", gIso, wifiWrap);
    debugLog("after ble heap=%u largest=%u psram=%u wrap=%u/%u/%u wifiInit=%d iso=%d",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)wrapHits, (unsigned)wrapMiss, (unsigned)wrapBytes, wifiWrap, gIso);
    if (isoExtras()) {
        xtouchAdapter.setDebugSerial(Debug);
        xtouchAdapter.setMidi(&multiMidi);
        xtouchAdapter.begin();
        // Inbound MIDI feeds the X-Touch adapter and the reaction engine;
        // the old MidiBoostLoop hop is gone - the looper/booster CCs are
        // owned by the seeded reaction rules now.
        multiMidi.action.setCallbackOnNoteOn([](uint8_t channel, uint8_t note, uint8_t velocity) {
            XtouchCompactAdapter::onNoteOn(channel, note, velocity);
            actionRulesOnMidi("noteOn", channel, note, velocity);
        });
        multiMidi.action.setCallbackOnNoteOff([](uint8_t channel, uint8_t note, uint8_t velocity) {
            XtouchCompactAdapter::onNoteOff(channel, note, velocity);
            actionRulesOnMidi("noteOff", channel, note, velocity);
        });
        multiMidi.action.setCallbackOnControlChange([](uint8_t channel, uint8_t controller, uint8_t value) {
            XtouchCompactAdapter::onControlChange(channel, controller, value);
            actionRulesOnMidi("cc", channel, controller, value);
        });
        multiMidi.action.setCallbackOnPitchBend([](uint8_t channel, uint8_t value) {
            XtouchCompactAdapter::onPitchBend(channel, value);
            actionRulesOnMidi("pitchBend", channel, value, 0);
        });
        actionRules.setDebugSerial(Debug);
        actionRules.begin(&multiMidi, &showbox, &connections, &hid);
        looper.setDebugSerial(Debug);
        looper.begin();
        snapshotLoader.setDebugSerial(Debug);
        snapshotLoader.begin();
        mutePedal.setDebugSerial(Debug);
        mutePedal.begin();
        boosterPedal.setDebugSerial(Debug);
        boosterPedal.begin();
    }
    frontPanel.attach(&connections, &hid, &ubfw);
    frontPanel.begin();
    actionRules.setFrontPanel(&frontPanel);
    connections.setFrontPanel(&frontPanel);
    lanKeepMdns();
    if (frontPanel.wifiRadioOn()) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    gSetupDone = true;
    httpsAllowBind();
    debugLog("setup done heap=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void loop() {
    uint32_t loopT0 = micros();
    diagLoopBeat();
    {
        // Gap-based stall detector: a pass stuck anywhere (telnet drain, UART,
        // lwip) shows up as an outsized interval between consecutive passes.
        static uint32_t lastPassMs = 0;
        uint32_t nowMs = millis();
        if (lastPassMs != 0 && nowMs - lastPassMs > 1500) {
            diagEventForce("loop", "stall %u ms", (unsigned)(nowMs - lastPassMs));
            // The last pass stalled somewhere inside a tick; a stalled telnet
            // peer is the known offender (Debug print blocks in lwip_select).
            ubfw.quarantineTelnet(60000);
        }
        lastPassMs = nowMs;
    }
    debugLogUsbTick();
    lanKeepMdns();
    lanTickRejoin();
    if (lanTakeHttpRestart()) {
        // JTAG: after AIRSTEP, ICMP and :80 SYN-ACK stay up, AsyncTCP
        // slots are free, but GET bodies never start. Rebind listen.
        webServer.end();
        webServer.begin();
        debugLog("http listen restart");
    }
    httpsTick();
    wiredNet.tick();
    lanTickHttpHealth();
    if (lanHasIp() && !ubfw.lanServicesUp()) {
        ubfw.startLanServices();
        lanQueueMdnsRefresh();
    }
    ubfw.tick();
    frontPanel.tick();
    connections.setDebugStatus(ubfw.wifiLogConnected(), ubfw.wifiLogBytes());
    lanKeepWifi(frontPanel);
    lanTickUnicastHealth();
    captivePortal.tick();
    liveSocket.tick();
    restApiRouter.tick();
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
    connections.tick();
    if (gDiagWatch) {
        diagTick();
    }
    {
        uint32_t loopMs = (micros() - loopT0) / 1000;
        if (loopMs > 300) {
            diagEvent("loop", "pass took %u ms", (unsigned)loopMs);
        }
    }
    ubfw.flushWifiLog();
    lanTickSoak(showbox);
    lanTickIsoReboot(showbox);
    usbTickWatch();
    {
// Yield to idle. Every tick above is millis()-based, so a short sleep
// cadence costs nothing functional — but an unsleeped loop pinned core
// 1 at ~99% while the console sat idle. Long passes skip the sleep so
// busy periods keep full speed. 8 ms keeps DIN MIDI polling (buffered
// serial), HID smooth moves (16 ms steps), and entity deltas snappy.
constexpr uint32_t kLoopPeriodMs = 8;
        uint32_t passMs = (micros() - loopT0) / 1000;
        if (passMs < kLoopPeriodMs) {
            delay(kLoopPeriodMs - passMs);
        }
    }
}
