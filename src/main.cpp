#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"

// Define UART pins
constexpr uint8_t BASE_RX = 11; // Base RX (Green)
constexpr uint8_t BASE_TX = 12; // Base TX (Blue)
constexpr uint8_t MIXER_RX = 14; // Mixer RX (Blue)
constexpr uint8_t MIXER_TX = 13; // Mixer TX (Green)
MackieShowbox showbox(MIXER_RX, MIXER_TX, BASE_RX, BASE_TX); // pass the pin numbers to the constructor

void setup() {
    #ifdef SHOWBOX_DEBUG
    Serial.begin(921600);
    Serial.println("Debug Serial initialized");
    #endif

    delay(2000);

    showbox.begin();
}

//unsigned long last_mute_toggle = 10000;
//unsigned long mute_toggle_interval = 3000;

void loop() {
    showbox.loop();
    /*unsigned long current_time = millis();
    if (current_time > last_mute_toggle + mute_toggle_interval) {
        bool mute = showbox.getBoolEntityValue(entity_id::MAIN_MUTE);
        showbox.setEntityValue(entity_id::MAIN_MUTE, !mute);
        last_mute_toggle = current_time;
    }*/
}
