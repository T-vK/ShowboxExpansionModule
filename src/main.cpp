#include <UARTInterceptor.h>

// Define UART pins
const uint8_t MIXER_RX = 16; // Mixer RX (Blue) | GPIO16 = TX2
const uint8_t MIXER_TX = 17; // Mixer TX (Green) | GPIO17 = RX2
const uint8_t BASE_RX = 10; // Base RX (Green) | GPIO10 = RX1 = SD3
const uint8_t BASE_TX = 9; // Base TX (Blue) | GPIO09 = TX1 = SD2

const UARTInterceptor::Direction TO_MIXER = UARTInterceptor::Direction::TO_A;
const UARTInterceptor::Direction TO_BASE = UARTInterceptor::Direction::TO_B;

UARTInterceptor interceptor(MIXER_RX, MIXER_TX, BASE_RX, BASE_TX);

void handlePacket(uint8_t* packet, size_t& length, UARTInterceptor::Direction direction) {
    if (direction == TO_BASE) {
        Serial.print("From Mixer: ");
    } else if (direction == TO_MIXER) {
        Serial.print("From Base:  ");
    }

    // Print the incoming packet
    for (size_t i = 0; i < length; ++i) {
        Serial.printf("%02X ", packet[i]);
    }
    Serial.println();
    
    // Random packet manipulation example (inverting with bitwise NOT)
    //for (size_t i = 0; i < length; ++i) {
    //    packet[i] ^= 0xFF;
    //}

    // Print the outgoing packet
    // if (direction == TO_BASE) {
    //     Serial.print("To Base:    ");
    // } else if (direction == TO_MIXER) {
    //     Serial.print("To Mixer:   ");
    // }
    //
    // for (size_t i = 0; i < length; ++i) {
    //     Serial.print(packet[i], HEX);
    //     Serial.print(" ");
    // }
    // Serial.println();
}

void injectPacket(const uint8_t* packet, size_t length, UARTInterceptor::Direction direction) {
    interceptor.injectPacket(packet, length, direction);
}

void setup() {
    Serial.begin(115200);
    interceptor.begin(115200, 115200); // Baud rates for Base and Mixer
    interceptor.setStartBytes(0xBE, 0xEF);
    interceptor.setEndBytes(0xEF, 0xBE);
    interceptor.setPacketHandler(handlePacket);
    interceptor.setPacketInjector(injectPacket);
    Serial.println("UART Interceptor ready");
}

void loop() {
    interceptor.loop();
}
