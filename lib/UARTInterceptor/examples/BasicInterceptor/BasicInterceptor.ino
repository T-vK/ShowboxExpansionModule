#include <Arduino.h>
//#define DEBUG_UART_INTERCEPTOR // Enable debug output
#include "UARTInterceptor.h"

UARTInterceptor uartInterceptor;

UARTInterceptor::Direction BASE = UARTInterceptor::DEVICE1_TO_DEVICE2;
UARTInterceptor::Direction MIXER = UARTInterceptor::DEVICE2_TO_DEVICE1;

constexpr uint8_t BASE_RX = 9; // Base RX (Green)
constexpr uint8_t BASE_TX = 10; // Base TX (Blue)
constexpr uint8_t MIXER_RX = 17; // Mixer RX (Blue)
constexpr uint8_t MIXER_TX = 18; // Mixer TX (Green)

void setup() {
    Serial.begin(115200); // For debugging
    delay(2000); // Delay for 2 seconds
    Serial.println("Starting UART Interceptor");

    uartInterceptor.begin(BASE_RX, BASE_TX, MIXER_RX, MIXER_TX, 115200, 115200, 256, 256);

    uint8_t startSig[] = {0xBE, 0xEF};
    uint8_t endSig[] = {0xEF, 0xBE};
    uartInterceptor.setPacketFinderStartEndSig(startSig, sizeof(startSig), endSig, sizeof(endSig));

    uartInterceptor.setPacketHandler([](uint8_t* packet, size_t& length, UARTInterceptor::Direction direction) {
        // Print format [MIXER/BASE] BYTES:
        Serial.printf("[%s] ", direction == BASE ? "BASE" : "MIXER");
        for (size_t i = 0; i < length; ++i) {
            Serial.printf("%02X ", packet[i]);
        }
        Serial.println();
        // if (length > 0) {
        //     packet[0] = 0xAA;
        //     return UARTInterceptor::PACKET_MODIFIED; // Packet modified
        // }
        return UARTInterceptor::PACKET_NOT_MODIFIED; // Packet not modified
    });
}

void loop() {
    uartInterceptor.loop();
}
