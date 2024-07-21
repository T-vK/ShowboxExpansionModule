#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

constexpr uint8_t BASE_RX = 9; // Base RX (Green)
constexpr uint8_t BASE_TX = 10; // Base TX (Blue)
constexpr uint8_t MIXER_RX = 17; // Mixer RX (Blue)
constexpr uint8_t MIXER_TX = 18; // Mixer TX (Green)

// Pins for double footswitch to control the looper
constexpr uint8_t RECORD_BUTTON_PIN = 48; // Looper Record/Overdub/Play button GPIO pin
constexpr uint8_t STOP_BUTTON_PIN = 47; // Looper Stop/Delete button GPIO pin

MackieShowbox showbox(BASE_RX, BASE_TX, MIXER_RX, MIXER_TX);

OneButton recordButton(RECORD_BUTTON_PIN, true);
OneButton stopButton(STOP_BUTTON_PIN, true);

void handleRecordOverdubPlayButton() {
    showbox.sendLooperButtonAction(looper_button_action::DOWN);
    showbox.sendLooperButtonAction(looper_button_action::UP);

    #ifdef SHOWBOX_DEBUG
    uint8_t currentState = showbox.getUint8EntityValue(entity_id::LOOPER_STATE);
    if (currentState == (uint8_t)DELETE || currentState == (uint8_t)STOP_PLAYING) {
        Serial.println("Recording initial loop");
    } else if (currentState == (uint8_t)RECORD_INITIAL_LOOP) {
        Serial.println("Recording overdub");
    } else if (currentState == (uint8_t)RECORD_OVERDUB) {
        Serial.println("Play");
    }
    #endif
}

void handleStopButton() {
    uint8_t currentState = showbox.getUint8EntityValue(entity_id::LOOPER_STATE);

    if (currentState == (uint8_t)PLAY || currentState == (uint8_t)RECORD_INITIAL_LOOP || currentState == (uint8_t)RECORD_OVERDUB) {
        showbox.sendLooperButtonAction(looper_button_action::DOUBLE_PRESS);
        #ifdef SHOWBOX_DEBUG
        Serial.println("Stop playing");
        #endif
    }
}

void handleDeleteButton() {
    showbox.sendLooperButtonAction(looper_button_action::LONG_PRESS);
    #ifdef SHOWBOX_DEBUG
    Serial.println("Set LOOPER_STATE to DELETE");
    #endif
}

void setup() {
    #ifdef SHOWBOX_DEBUG
    Serial.begin(921600);
    Serial.println("Debug Serial initialized");
    #endif

    recordButton.attachClick(handleRecordOverdubPlayButton);
    stopButton.attachClick(handleStopButton);
    stopButton.attachLongPressStart(handleDeleteButton);

    delay(2000);

    showbox.begin();
}

void loop() {
    showbox.loop();
    recordButton.tick();
    stopButton.tick();
}