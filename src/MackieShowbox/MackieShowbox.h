#ifndef MACKIESHOWBOX_H
#define MACKIESHOWBOX_H

#include <unordered_map>
#include <Arduino.h>
#include <UARTInterceptor.h>
#include "constants.h"
#ifdef SHOWBOX_DEBUG
#include "debug.h"
#endif
#include "Packet.h"

class MackieShowbox {
public:
    MackieShowbox(uint8_t mixerRx, uint8_t mixerTx, uint8_t baseRx, uint8_t baseTx);
    
    void setEntityValue(entity_id entityId, bool value, bool emit = true);
    void setEntityValue(entity_id entityId, uint8_t value, bool emit = true);
    void setEntityValue(entity_id entityId, float value, bool emit = true);

    bool getBoolEntityValue(entity_id entityId);
    uint8_t getUint8EntityValue(entity_id entityId);
    float getFloatEntityValue(entity_id entityId);
    void loop();
    void begin();

private:
    struct EntityValue {
        bool boolValue;
        uint8_t uint8Value;
        float floatValue;

        EntityValue() : boolValue(false), uint8Value(0), floatValue(0.0f) {}
    };
    std::unordered_map<entity_id, EntityValue> entityValues;
    UARTInterceptor interceptor;
    UARTInterceptor::Direction TO_MIXER = UARTInterceptor::Direction::TO_A;
    UARTInterceptor::Direction TO_BASE = UARTInterceptor::Direction::TO_B;

    // Instance method for packet handling
    static bool handlePacketStatic(void* context, uint8_t* raw_packet, size_t& length, UARTInterceptor::Direction direction);
    bool handlePacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction);

};

#endif // MACKIESHOWBOX_H
