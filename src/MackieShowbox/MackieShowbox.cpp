#define SHOWBOX_DEBUG

#include "MackieShowbox.h"
#ifdef SHOWBOX_DEBUG
#include "debug.h"
#endif

MackieShowbox::MackieShowbox(uint8_t mixerRx, uint8_t mixerTx, uint8_t baseRx, uint8_t baseTx)
    : interceptor(mixerRx, mixerTx, baseRx, baseTx) {

    // Initialize entityValues with default values based on entity_type_mapping
    for (const auto& pair : entity_type_mapping) {
        entity_id entityId = pair.first;
        entityValues[entityId] = EntityValue();
    }
}

void MackieShowbox::begin() {
    // Initialize UARTInterceptor
    interceptor.begin(115200, 115200);
    interceptor.setStartBytes(0xBE, 0xEF);
    interceptor.setEndBytes(0xEF, 0xBE);

    // Set packet handler and injector
    interceptor.setPacketHandler(&MackieShowbox::handlePacketStatic, this);
    #ifdef SHOWBOX_DEBUG
    Serial.println("\nUART Interceptor ready\n");
    #endif
}

void MackieShowbox::setEntityValue(entity_id entityId, bool value, bool emit) {
    entityValues[entityId].boolValue = value;
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.injectPacket(packet, sizeof(packet), TO_MIXER); // Set value on the base
        interceptor.injectPacket(packet, sizeof(packet), TO_BASE); // Notify the mixer of the change
    }
}
void MackieShowbox::setEntityValue(entity_id entityId, uint8_t value, bool emit) {
    entityValues[entityId].uint8Value = value;
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.injectPacket(packet, sizeof(packet), TO_MIXER); // Set value on the base
        interceptor.injectPacket(packet, sizeof(packet), TO_BASE); // Notify the mixer of the change
    }
}
void MackieShowbox::setEntityValue(entity_id entityId, float value, bool emit) {
    if (emit) {
        uint8_t floatBytes[4];
        memcpy(floatBytes, &value, sizeof(float));

        uint8_t packet[15] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, floatBytes[0], floatBytes[1], floatBytes[2], floatBytes[3], 0xEF, 0xBE };

        interceptor.injectPacket(packet, sizeof(packet), TO_MIXER); // Set value on the base
        interceptor.injectPacket(packet, sizeof(packet), TO_BASE); // Notify the mixer of the change
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

// static bool MackieShowbox::handlePacketStatic(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, MackieShowbox* instance); implementation:
bool MackieShowbox::handlePacketStatic(void* context, uint8_t* raw_packet, size_t& length, UARTInterceptor::Direction direction) {
    MackieShowbox* self = static_cast<MackieShowbox*>(context);
    return self->handlePacket(raw_packet, length, direction);
}

bool MackieShowbox::handlePacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction) {
    #ifdef SHOWBOX_DEBUG
    // print raw_packet as hex bytes
    printRawPacket("[Raw]: ", raw_packet);
    // print direction:
    #endif

    packet_type packetType = static_cast<packet_type>(raw_packet[3]);
    #ifdef SHOWBOX_DEBUG
    Serial.printf("%s ", direction == TO_MIXER ? "BASE->MIXER" : "MIXER->BASE");
    Serial.printf("[Decoded] Type: %s", packet_type_to_string[packetType].c_str());
    #endif
    switch (packetType) {
        case ENTITY:
        entity_id entityId = static_cast<entity_id>(raw_packet[5]);
        entity_data_type entityType = entity_type_mapping[entityId];
        #ifdef SHOWBOX_DEBUG
        Serial.printf("| { Set %s to ", entity_id_to_string[entityId].c_str());
        #endif
        switch (entityType) {
            case BOOL:
            {
                //LogBoolEntityPacket boolPacket;
                //boolPacket.handle(raw_packet);
                //bool value = showbox.getBoolEntityValue(entityId);
                break;
            }
            case UINT8:
            {
                //LogUint8EntityPacket uint8Packet;
                //uint8Packet.handle(raw_packet);
                //uint8_t value = showbox.getUint8EntityValue(entityId);
                //Serial.printf("\nValue stored in showbox: %d", value);
                break;
            }
            case FLOAT:
            {
                //LogFloatEntityPacket floatPacket;
                //floatPacket.handle(raw_packet);

                //float value = showbox.getFloatEntityValue(entityId);
                //Serial.printf("\nValue stored in showbox: %f", value);
                break;
            }
        }
        #ifdef SHOWBOX_DEBUG
        Serial.print(" }");
        #endif
    }
    #ifdef SHOWBOX_DEBUG
    Serial.println();
    //printRawPacket("[Raw]: ", raw_packet);
    Serial.println();
    #endif
    return false; // false == do not drop packet
}

void MackieShowbox::loop() {
    interceptor.loop();
}