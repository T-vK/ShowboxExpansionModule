#define SHOWBOX_DEBUG

#include "MackieShowbox.h"
#ifdef SHOWBOX_DEBUG
#include "debug.h"
#endif

MackieShowbox::MackieShowbox(uint8_t baseRx, uint8_t baseTx, uint8_t mixerRx, uint8_t mixerTx)
    : interceptor() {
    this->baseRx = baseRx;
    this->baseTx = baseTx;
    this->mixerRx = mixerRx;
    this->mixerTx = mixerTx;

    // Initialize entityValues with default values based on entity_type_mapping
    for (const auto& pair : entity_type_mapping) {
        entity_id entityId = pair.first;
        entityValues[entityId] = EntityValue();
    }
}

void MackieShowbox::begin() {
    interceptor.begin(baseRx, baseTx, mixerRx, mixerTx, 115200, 115200, 256, 256);

    uint8_t startSig[] = {0xBE, 0xEF};
    uint8_t endSig[] = {0xEF, 0xBE};
    interceptor.setPacketFinderStartEndSig(startSig, sizeof(startSig), endSig, sizeof(endSig));

    interceptor.setPacketHandler([this](uint8_t* packet, size_t& length, UARTInterceptor::Direction direction) {
        return this->handlePacket(packet, length, direction);
    });
    

    #ifdef SHOWBOX_DEBUG
    Serial.println("\nMackieShowbox Interceptor ready\n");
    #endif
}

void MackieShowbox::setEntityValue(entity_id entityId, bool value, bool emit) {
    entityValues[entityId].boolValue = value;
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
    }
}
void MackieShowbox::setEntityValue(entity_id entityId, uint8_t value, bool emit) {
    entityValues[entityId].uint8Value = value;
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
    }
}
void MackieShowbox::setEntityValue(entity_id entityId, float value, bool emit) {
    if (emit) {
        uint8_t floatBytes[4];
        memcpy(floatBytes, &value, sizeof(float));

        uint8_t packet[15] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, floatBytes[0], floatBytes[1], floatBytes[2], floatBytes[3], 0xEF, 0xBE };

        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
    }
    entityValues[entityId].floatValue = value;
}
bool MackieShowbox::getBoolEntityValue(entity_id entityId) {
    return entityValues[entityId].boolValue;
}
uint8_t MackieShowbox::getUint8EntityValue(entity_id entityId) {
    return entityValues[entityId].uint8Value;
}
float MackieShowbox::getFloatEntityValue(entity_id entityId) {
    return entityValues[entityId].floatValue;
}

void MackieShowbox::sendLooperButtonAction(looper_button_action action) {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, action, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

UARTInterceptor::PacketHandlerResult MackieShowbox::handlePacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction) {
    #ifdef SHOWBOX_DEBUG
    // printRawPacket("[Raw]: ", raw_packet);
    #endif

    packet_type packetType = static_cast<packet_type>(raw_packet[3]);

    #ifdef SHOWBOX_DEBUG
    String directionString = direction == TO_MIXER ? "BASE->MIXER" : "MIXER->BASE";
    String packetTypeString = packet_type_to_string[packetType].c_str();
    #endif

    if (packetType == ENTITY) {
        entity_id entityId = static_cast<entity_id>(raw_packet[5]);
        entity_data_type entityType = entity_type_mapping[entityId];
        #ifdef SHOWBOX_DEBUG
        String entityString = entity_id_to_string[entityId].c_str();
        Serial.printf("%s [Decoded] Type: %s | Entity: %s | { Set %s to ", directionString.c_str(), packetTypeString.c_str(), entityString.c_str(), entityString.c_str());
        #endif
        if (entityType == BOOL) {
            bool value = raw_packet[9];
            #ifdef SHOWBOX_DEBUG
            Serial.printf("%s", value ? "true" : "false");
            #endif
            setEntityValue(entityId, value, false);
        } else if (entityType == UINT8) {
            uint8_t value = raw_packet[9];
            #ifdef SHOWBOX_DEBUG
            if (entityId == LOOPER_STATE) {
                Serial.printf("%s", looper_state_to_string[value].c_str());
            } else {
                Serial.printf("%d", value);
            }
            #endif
            setEntityValue(entityId, value, false);
        } else if (entityType == FLOAT) {
            float value;
            memcpy(&value, &raw_packet[9], sizeof(float));
            #ifdef SHOWBOX_DEBUG
            Serial.printf("%f", value);
            #endif
            setEntityValue(entityId, value, false);
        }
        #ifdef SHOWBOX_DEBUG
        Serial.print(" }\n");
        #endif
    } else if (packetType == HEARTBEAT) {
        #ifdef SHOWBOX_DEBUG
        // Serial.print("Heartbeat");
        #endif
    } else if (packetType == ACK) {
        uint8_t ackCmd = raw_packet[5];
        if (ackCmd != HEARTBEAT && ackCmd != UNKNOWN_00 && ackCmd != UNKNOWN_FE) {
            #ifdef SHOWBOX_DEBUG
            String ackCmdString = packet_type_to_string[static_cast<packet_type>(ackCmd)].c_str();
            //printRawPacket("[Raw]: ", raw_packet);
            Serial.printf("%s Acknowoleged %s command\n", directionString.c_str(), ackCmdString.c_str());
            #endif
        }
        #ifdef SHOWBOX_DEBUG
        #endif
    } else if (packetType == DATA_REQUEST) {
        #ifdef SHOWBOX_DEBUG
        // Serial.print("Data Request");
        #endif
    } else if (packetType == LOOPER_BUTTON) {
        #ifdef SHOWBOX_DEBUG
        // Serial.print("Data Request");
        #endif
    } else {
        #ifdef SHOWBOX_DEBUG
        Serial.printf("%s [Decoded] Type: %s - ", directionString.c_str(), packetTypeString.c_str());
        printRawPacket("[Raw]: ", raw_packet);
        #endif
    }
    #ifdef SHOWBOX_DEBUG
    //Serial.println();
    //printRawPacket("[Raw]: ", raw_packet);
    //Serial.println();
    #endif
    return UARTInterceptor::PACKET_NOT_MODIFIED; // Packet not modified
}

void MackieShowbox::loop() {
    interceptor.loop();
}