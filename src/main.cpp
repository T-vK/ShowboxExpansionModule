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
#include "RestApiRouter/RestApiRouter.h"
#include "MultiMidi/MultiMidi.h"
#include "XtouchCompactAdapter/XtouchCompactAdapter.h"
#include <ESPAsyncWebServer.h>

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
XtouchCompactAdapter xtouchAdapter(&showbox);

void setup() {
    #ifdef SHOWBOX_DEBUG
        Serial.begin(921600);
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
    Debug = &ubfw.Debug;

    Debug->println("[ INFO ] Initializing MultiMidi...");
    multiMidi.setDebugSerial(Debug);
    multiMidi.enableHardwareMidi(MIDI_RX, MIDI_TX);
    multiMidi.enableAppleMidi(5004);
    multiMidi.enableBleMidi("ShowboxMidi");
    multiMidi.begin();
    Debug->println("[  OK  ] MultiMidi initialized.");

    Debug->println("[ INFO ] Initializing Showbox MIDI Adapter...");
    xtouchAdapter.setDebugSerial(Debug);
    xtouchAdapter.setMidi(&multiMidi);
    xtouchAdapter.begin();
    Debug->println("[  OK  ] Showbox MIDI Adapter initialized.");

    Debug->println("[ INFO ] Initializing Showbox...");
    showbox.setDebugSerial(Debug);
    showbox.begin();
    Debug->println("[  OK  ] Showbox initialized.");

    Debug->println("[ INFO ] Initializing API Router...");
    restApiRouter.setDebugSerial(Debug);
    restApiRouter.setWebServer(&webServer);
    restApiRouter.setShowbox(&showbox);
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
    Debug->println("[  OK  ] Web Server started.");
}

unsigned long lastPrint = 0;

void loop() {
    ubfw.tick();
    showbox.tick();
    looper.tick();
    snapshotLoader.tick();
    mutePedal.tick();
    boosterPedal.tick();
    multiMidi.tick();
    xtouchAdapter.tick();
    
    if (millis() - lastPrint > 3000) {
        lastPrint = millis();
        Debug->println("[ INFO ] Heartbeat from main execution loop");
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