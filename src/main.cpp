#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "TwoButtonLooper/TwoButtonLooper.h"
//#include "SnapshotLoader/SnapshotLoader.h"
//#include "BoosterPedal/BoosterPedal.h"

// Mackie Showbox Base-Unit
constexpr uint8_t SHOWBOX_BASE_TX = 11; // Base TX (Blue)
constexpr uint8_t SHOWBOX_BASE_RX = 12; // Base RX (Green)

// Mackie Showbox Breakaway Mixer
constexpr uint8_t SHOWBOX_MIXER_RX = 13; // Mixer RX (Blue)
constexpr uint8_t SHOWBOX_MIXER_TX = 14; // Mixer TX (Green)

// TRS Jacks (Footswitches / Expression Pedals)
constexpr uint8_t TRS1_TIP = 8;
constexpr uint8_t TRS1_RING = 2;
constexpr uint8_t TRS2_TIP = 21;
constexpr uint8_t TRS2_RING = 1;
constexpr uint8_t TRS3_TIP = 47;
constexpr uint8_t TRS3_RING = 4;
constexpr uint8_t TRS4_TIP = 48;
constexpr uint8_t TRS4_RING = 5;

// MIDI UART Module (Arduino MIDI Shield)
constexpr uint8_t MIDI_TX = 9;
constexpr uint8_t MIDI_RX = 10;

// RGB LED Controller (P9813)
constexpr uint8_t RGB_DATA_IN = 42;
constexpr uint8_t RGB_CLOCK_IN = 41;

// Buttons
constexpr uint8_t GREEN_BUTTON = 40;
constexpr uint8_t RED_BUTTON = 39;
constexpr uint8_t BLUE_BUTTON = 38;

// SPI Ethernet Module (Mini W5500)
constexpr uint8_t W5500_nRESET = 6;
constexpr uint8_t W5500_MISO = 7;
constexpr uint8_t W5500_nINT = 15;
constexpr uint8_t W5500_nSS = 16;
constexpr uint8_t W5500_SCLK = 17;
constexpr uint8_t W5500_MOSI = 18;


// Pins for double footswitch to control the looper
constexpr uint8_t RECORD_BUTTON_PIN = TRS1_TIP; // Looper Record/Overdub/Play button GPIO pin
constexpr uint8_t STOP_BUTTON_PIN = TRS1_RING; // Looper Stop/Delete button GPIO pin

// Pins for snapshot buttons
//constexpr uint8_t SNAPSHOT_BUTTON_PIN1 = TRS2_TIP; // Snapshot button 1 GPIO pin
//constexpr uint8_t SNAPSHOT_BUTTON_PIN2 = TRS2_RING; // Snapshot button 2 GPIO pin

// Pin for booster pedal button
//constexpr uint8_t BOOSTER_PEDAL_PIN = TRS3_TIP; // Booster pedal GPIO pin

MackieShowbox showbox(SHOWBOX_BASE_RX, SHOWBOX_BASE_TX, SHOWBOX_MIXER_RX, SHOWBOX_MIXER_TX);
TwoButtonLooper looper(RECORD_BUTTON_PIN, STOP_BUTTON_PIN, showbox);
//SnapshotLoader snapshotLoader(SNAPSHOT_BUTTON_PIN1, SNAPSHOT_BUTTON_PIN2, showbox);
//BoosterPedal boosterPedal(BOOSTER_PEDAL_PIN, showbox, entity_id::INPUT2_GAIN, Showbox::InputGain::POSITION_11, Showbox::InputGain::POSITION_13);

void setup() {
    #ifdef SHOWBOX_DEBUG
    Serial.begin(921600);
    #endif
    delay(2000);
    #ifdef SHOWBOX_DEBUG
    Serial.println("Debug Serial initialized");
    #endif

    showbox.begin();
    looper.begin();
    //snapshotLoader.begin();
    //boosterPedal.begin();
}

void loop() {
    showbox.tick();
    looper.tick();
    //snapshotLoader.tick();
    //boosterPedal.tick();
}
