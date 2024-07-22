#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "TwoButtonLooper/TwoButtonLooper.h"
//#include "SnapshotLoader/SnapshotLoader.h"
#include "BoosterPedal/BoosterPedal.h"

constexpr uint8_t BASE_RX = 9; // Base RX (Green)
constexpr uint8_t BASE_TX = 10; // Base TX (Blue)
constexpr uint8_t MIXER_RX = 17; // Mixer RX (Blue)
constexpr uint8_t MIXER_TX = 18; // Mixer TX (Green)

// Pins for double footswitch to control the looper
constexpr uint8_t RECORD_BUTTON_PIN = 48; // Looper Record/Overdub/Play button GPIO pin
constexpr uint8_t STOP_BUTTON_PIN = 47; // Looper Stop/Delete button GPIO pin

// Pins for snapshot buttons
//constexpr uint8_t SNAPSHOT_BUTTON_PIN1 = 6; // Snapshot button 1 GPIO pin
//constexpr uint8_t SNAPSHOT_BUTTON_PIN2 = 7; // Snapshot button 2 GPIO pin

// Pin for booster pedal button
constexpr uint8_t BOOSTER_PEDAL_PIN = 5; // Booster pedal GPIO pin

MackieShowbox showbox(BASE_RX, BASE_TX, MIXER_RX, MIXER_TX);
TwoButtonLooper looper(RECORD_BUTTON_PIN, STOP_BUTTON_PIN, showbox);
//SnapshotLoader snapshotLoader(SNAPSHOT_BUTTON_PIN1, SNAPSHOT_BUTTON_PIN2, showbox);
BoosterPedal boosterPedal(BOOSTER_PEDAL_PIN, showbox, entity_id::INPUT2_GAIN, Showbox::InputGain::POSITION_11, Showbox::InputGain::POSITION_13);

void setup() {
    #ifdef SHOWBOX_DEBUG
    Serial.begin(921600);
    Serial.println("Debug Serial initialized");
    #endif

    looper.begin();
    //snapshotLoader.begin();
    boosterPedal.begin();
    showbox.begin();
}

void loop() {
    showbox.tick();
    looper.tick();
    //snapshotLoader.tick();
    boosterPedal.tick();
}
