#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include <UltimateBaseFirmware.h>
#include <MultiPrint.h>
#include "pin_definitions.h"
#include "MackieShowbox/MackieShowbox.h"
#include "TwoButtonLooper/TwoButtonLooper.h"
//#include "SnapshotLoader/SnapshotLoader.h"
#include "BoosterPedal/BoosterPedal.h"
#include "MainMutePedal/MainMutePedal.h"

// Pins for double footswitch to control the looper
constexpr uint8_t RECORD_BUTTON_PIN = TRS1_TIP; // Looper Record/Overdub/Play button GPIO pin
constexpr uint8_t STOP_BUTTON_PIN = TRS1_RING; // Looper Stop/Delete button GPIO pin

// Pins for snapshot buttons
constexpr uint8_t SNAPSHOT_BUTTON_PIN1 = TRS4_TIP; // Snapshot button 1 GPIO pin
constexpr uint8_t SNAPSHOT_BUTTON_PIN2 = TRS4_RING; // Snapshot button 2 GPIO pin

// Pin for booster pedal button
constexpr uint8_t BOOSTER_PEDAL_PIN = RED_BUTTON; //TRS2_TIP; // Booster pedal GPIO pin

// Pin for mute pedal button
constexpr uint8_t MUTE_PEDAL_PIN = BLUE_BUTTON; //TRS3_TIP; // Mute pedal GPIO pin

Print *Debug = &Serial;
UltimateBaseFirmware ubfw("ShowboxExpansionModule Firmware");
MackieShowbox showbox(SHOWBOX_BASE_RX, SHOWBOX_BASE_TX, SHOWBOX_MIXER_RX, SHOWBOX_MIXER_TX);
TwoButtonLooper looper(RECORD_BUTTON_PIN, STOP_BUTTON_PIN, &showbox);
MainMutePedal mutePedal(MUTE_PEDAL_PIN, &showbox);
BoosterPedal boosterPedal(BOOSTER_PEDAL_PIN, &showbox, entity_id::INPUT1_VOLUME);
//SnapshotLoader snapshotLoader(SNAPSHOT_BUTTON_PIN1, SNAPSHOT_BUTTON_PIN2, &showbox);

void setup() {
    #ifdef SHOWBOX_DEBUG
        Serial.begin(921600);
    #endif
    delay(2000);

    if (strcmp(SXM_VERSION, "") == 0) {
        ubfw.setVersion(CURRENT_TIME);
    } else {
        ubfw.setVersion(SXM_VERSION);
    }
    ubfw.setHostName("showbox.local");
    ubfw.begin();
    Debug = &ubfw.Debug;

    Debug->println("[ INFO ] Initializing Showbox...");
    showbox.setDebugSerial(Debug);
    showbox.begin();
    Debug->println("[  OK  ] Showbox initialized.");

    Debug->println("[ INFO ] Initializing Looper...");
    looper.setDebugSerial(Debug);
    looper.begin();
    Debug->println("[  OK  ] Looper initialized.");

    //Debug->println("[ INFO ] Initializing Snapshot Loader...");
    //snapshotLoader.setDebugSerial();
    //snapshotLoader.begin();
    //Debug->println("[  OK  ] Snapshot Loader initialized.");

    Debug->println("[ INFO ] Initializing Mute Pedal...");
    mutePedal.setDebugSerial(Debug);
    mutePedal.begin();
    Debug->println("[  OK  ] Mute Pedal initialized.");

    Debug->println("[ INFO ] Initializing Booster Pedal...");
    boosterPedal.setDebugSerial(Debug);
    boosterPedal.begin();
    Debug->println("[  OK  ] Booster Pedal initialized.");

    #ifdef SHOWBOX_DEBUG
        Debug->print("[ INFO ] IP Address: ");
        Debug->println(WiFi.localIP());
    #endif

}

// helper var to print every 5 seconds
unsigned long lastPrint = 0;

void loop() {
    ubfw.loop();
    showbox.tick();
    looper.tick();
    //snapshotLoader.tick();
    mutePedal.tick();
    boosterPedal.tick();
    // print hello every 5 seconds
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        #ifdef SHOWBOX_DEBUG
            Debug->printf("[ INFO ] Debug Heartbeat from Debug\n");
            //ubfw._remoteDebug.printf("[ INFO ] Debug Heartbeat from _remoteDebug\n");
        #endif
    }
}