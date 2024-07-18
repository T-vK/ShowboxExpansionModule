#define SHOWBOX_MOCK_MODE // use fake packet data
#define DEBUG // enable debug constants and functions

#include <Arduino.h>
#include <UARTInterceptor.h>
#include "Packet.h"
#include "constants.h"
#ifdef DEBUG
#include "debug.h"
#endif

const UARTInterceptor::Direction TO_MIXER = UARTInterceptor::Direction::TO_A;
const UARTInterceptor::Direction TO_BASE = UARTInterceptor::Direction::TO_B;

#ifndef SHOWBOX_MOCK_MODE
// Define UART pins
constexpr uint8_t MIXER_RX = 16; // Mixer RX (Blue) | GPIO16 = TX2
constexpr uint8_t MIXER_TX = 17; // Mixer TX (Green) | GPIO17 = RX2
constexpr uint8_t BASE_RX = 10; // Base RX (Green) | GPIO10 = RX1 = SD3
constexpr uint8_t BASE_TX = 9; // Base TX (Blue) | GPIO09 = TX1 = SD2

UARTInterceptor interceptor(MIXER_RX, MIXER_TX, BASE_RX, BASE_TX);

void injectPacket(const uint8_t* packet, size_t length, UARTInterceptor::Direction direction) {
    interceptor.injectPacket(packet, length, direction);
}
#endif

struct LogFloatEntityPacket : Packet<ENTITY, EntityBody<FLOAT, float>> {
    void intercept() override {
        Serial.printf("Value: %f", p.value);
    }
};

struct LogBoolEntityPacket : Packet<ENTITY, EntityBody<BOOL, bool>> {
    void intercept() override {
        Serial.printf("Value: %d", p.value);
    }
};

struct LogUint8EntityPacket : Packet<ENTITY, EntityBody<UINT8, uint8_t>> {
    void intercept() override {
        Serial.printf("Value: %d", p.value);
    }
};

void handlePacket(uint8_t *raw_packet, UARTInterceptor::Direction direction) {
    #ifdef DEBUG
    printRawPacket("[Raw] (before): ", raw_packet);
    #endif

    packet_type packetType = static_cast<packet_type>(raw_packet[3]);
    #ifdef DEBUG
    Serial.printf("[Decoded] Type: %s | ", packet_type_to_string[packetType].c_str());
    #endif
    switch (packetType) {
        case ENTITY:
        entity_id entityId = static_cast<entity_id>(raw_packet[5]);
        entity_data_type entityType = entity_type_mapping[entityId];
        #ifdef DEBUG
        Serial.printf("Decoded Entity { Name: %s, Data Type: %s, ", entity_id_to_string[entityId].c_str(), entity_data_type_to_string[entityType].c_str());
        #endif
        switch (entityType) {
            case BOOL:
            {
                //Serial.println("BoolEntityPacket");
                LogBoolEntityPacket boolPacket;
                boolPacket.handle(raw_packet);
                break;
            }
            case UINT8:
            {
                //Serial.println("Uint8EntityPacket");
                LogUint8EntityPacket uint8Packet;
                uint8Packet.handle(raw_packet);
                break;
            }
            case FLOAT:
            {
                //Serial.println("FloatEntityPacket");
                LogFloatEntityPacket floatPacket;
                floatPacket.handle(raw_packet);
                break;
            }
        }
        Serial.println(" }");
    }

    #ifdef DEBUG
    printRawPacket("[Raw] (after):  ", raw_packet);
    Serial.println();
    #endif
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    #ifndef SHOWBOX_MOCK_MODE
    interceptor.begin(115200, 115200); // Baud rates for Base and Mixer
    interceptor.setStartBytes(0xBE, 0xEF);
    interceptor.setEndBytes(0xEF, 0xBE);
    interceptor.setPacketHandler(handlePacket);
    interceptor.setPacketInjector(injectPacket);
    Serial.println("\nUART Interceptor ready\n");
    #endif
}

void loop() {
    #ifndef SHOWBOX_MOCK_MODE
    interceptor.loop();
    #endif

    #ifdef SHOWBOX_MOCK_MODE
    // Example packet
    uint8_t raw_packet[] = {
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

    handlePacket(raw_packet, TO_BASE);

    delay(5000);
    #endif
}
