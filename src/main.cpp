//#define SHOWBOX_MOCK_MODE // use fake packet data
#define SHOWBOX_DEBUG // enable debug constants and functions

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"

//const UARTInterceptor::Direction TO_MIXER = UARTInterceptor::Direction::TO_A;
//const UARTInterceptor::Direction TO_BASE = UARTInterceptor::Direction::TO_B;

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

    #ifndef SHOWBOX_MOCK_MODE
    showbox.begin();
    #endif
}

unsigned long last_mute_toggle = 10000;
unsigned long mute_toggle_interval = 3000;

void loop() {
    #ifndef SHOWBOX_MOCK_MODE
    showbox.loop();
    unsigned long current_time = millis();
    if (current_time > last_mute_toggle + mute_toggle_interval) {
        bool mute = showbox.getBoolEntityValue(entity_id::MAIN_MUTE);
        showbox.setEntityValue(entity_id::MAIN_MUTE, !mute);
        last_mute_toggle = current_time;
    }
    #endif

    #ifdef SHOWBOX_MOCK_MODE
    // Example packet
    uint8_t raw_packet_1[] = {
        // ------ HEADER ------
        0xBE, 0xEF, // start sequence
        0x08,       // body size uint8_t
        0x03,       // type uint8_t
        0x00,       // unknown
        // ------ HEADER ------

        // ------ BODY ------
        0x58,                   // entity id uint8_t
        0x00, 0x00, 0x00,       // reserved
        0x00, 0x00, 0xC0, 0x40, // value (float, little-endian) // 6.0f
        // ------ BODY ------

        // ------ FOOTER ------
        0xEF, 0xBE // end sequence
        // ------ FOOTER ------
    };

    bool dropped;

    dropped = showbox.handlePacket(raw_packet_1, TO_BASE);
    if (dropped) {
        Serial.println("Packet dropped");
    }
    Serial.println();

    delay(2000);

    // Example packet 2
    uint8_t raw_packet_2[] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, 0x59, 0x00, 0x00, 0x00, 0x01, 0xEF, 0xBE };
    dropped = showbox.handlePacket(raw_packet_2, TO_BASE);
    if (dropped) {
        Serial.println("Packet dropped");
    }
    Serial.println();

    delay(2000);

    // Example packet 3
    uint8_t raw_packet_3[] = { 0xBE, 0xEF, 0x09, 0x03, 0x00, 0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x41, 0xEF, 0xBE }; // 12.0f
    dropped = showbox.handlePacket(raw_packet_3, TO_BASE);
    if (dropped) {
        Serial.println("Packet dropped");
    }
    Serial.println();

    delay(2000);
    #endif
}
