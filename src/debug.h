// debug.h
#ifndef DEBUG_H
#define DEBUG_H

#include <map>
#include <string>
#include <Arduino.h>
#include "constants.h"

extern std::unordered_map<packet_type, std::string> packet_type_to_string;
extern std::unordered_map<entity_data_type, std::string> entity_data_type_to_string;
extern std::unordered_map<entity_id, std::string> entity_id_to_string;
extern std::unordered_map<uint8_t, std::string> looper_state_to_string;
extern std::map<float, std::string> master_gain_to_string;

/*
void printRawPacket(const char* message, uint8_t* raw_packet) {
    Serial.printf("%s", message);
    uint8_t size = raw_packet[2] + 7;
    for (int i = 0; i < size; i++) {
        Serial.printf("%02X ", raw_packet[i]);
    }
    Serial.println();
}*/
//Header:
void printRawPacket(const char* message, uint8_t* raw_packet);

// template <typename T>
// void printRawPacket(const char* message, T *packet) {
//     Serial.printf("%s: ", message);
//     auto rawBytes = reinterpret_cast<uint8_t *>(packet);
//     for (int i = 4 /* skip vtable */; i < sizeof(T); i++) {
//         Serial.printf("%02X ", rawBytes[i]);
//     }
//     Serial.println();
// }


namespace master_gain {
    constexpr float POSITION_16 = 6.0;
    constexpr float POSITION_15 = 4.112315;
    constexpr float POSITION_14 = 2.084685;
    constexpr float POSITION_13 = -0.10532999;
    constexpr float POSITION_12 = -2.48601365;
    constexpr float POSITION_11 = -5.093745;
    constexpr float POSITION_10 = -7.97647;
    constexpr float POSITION_9 = -11.1990757;
    constexpr float POSITION_8 = -14.85257;
    constexpr float POSITION_7 = -19.0702229;
    constexpr float POSITION_6 = -24.0586376;
    constexpr float POSITION_5 = -30.1639671;
    constexpr float POSITION_4 = -38.0351067;
    constexpr float POSITION_3 = -49.12886;
    constexpr float POSITION_2 = -68.09374;
    constexpr float POSITION_1 = -120.0;
}

#endif // DEBUG_H