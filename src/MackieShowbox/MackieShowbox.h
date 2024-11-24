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
    MackieShowbox(uint8_t baseRx, uint8_t baseTx, uint8_t mixerRx, uint8_t mixerTx);
    
    void setEntityValue(entity_id entityId, bool value, bool emit = true);
    void setEntityValue(entity_id entityId, uint8_t value, bool emit = true);
    void setEntityValue(entity_id entityId, float value, bool emit = true);

    bool getBoolEntityValue(entity_id entityId);
    uint8_t getUint8EntityValue(entity_id entityId);
    float getFloatEntityValue(entity_id entityId);

    void sendLooperButtonAction(looper_button_action action);
    void toggleSdCardRecord();
    void snapshotAction(snapshot_action action, snapshot_slot slot);
    void tunerAction(tuner_action action, tuner_chan chan);
    float getBatteryLevel();
    sd_card_state getSdCardState();

    void tick();
    void begin();

    void setDebugSerial(Print* serial);

private:
    uint8_t baseRx, baseTx, mixerRx, mixerTx;
    float batteryLevel = 0.0f;
    sd_card_state sdCardState = sd_card_state::NOT_DETECTED;
    Print* Debug = &Serial;
    struct EntityValue {
        bool boolValue;
        uint8_t uint8Value;
        float floatValue;

        EntityValue() : boolValue(false), uint8Value(0), floatValue(0.0f) {}
    };
    std::unordered_map<entity_id, EntityValue> entityValues;
    UARTInterceptor interceptor;
    UARTInterceptor::Direction TO_MIXER = UARTInterceptor::DEVICE1_TO_DEVICE2;
    UARTInterceptor::Direction TO_BASE = UARTInterceptor::DEVICE2_TO_DEVICE1;

    UARTInterceptor::PacketHandlerResult handlePacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction);
};

#endif // MACKIESHOWBOX_H