#ifndef MACKIESHOWBOX_H
#define MACKIESHOWBOX_H

#include <unordered_map>
#include <Arduino.h>
#include <UARTInterceptor.h>
#include "constants.h"
#ifdef SHOWBOX_DEBUG
#include "string_mappings.h"
#endif
#include "Packet.h"

class MackieShowbox {
public:
    MackieShowbox(uint8_t baseRx, uint8_t baseTx, uint8_t mixerRx, uint8_t mixerTx);
    
    // Low level setters
    void setEntityValue(entity_id entityId, bool value, bool emit = true);
    void setEntityValue(entity_id entityId, uint8_t value, bool emit = true);
    void setEntityValue(entity_id entityId, float value, bool emit = true);

    // Low level getters
    bool getBoolEntityValue(entity_id entityId);
    uint8_t getUint8EntityValue(entity_id entityId);
    float getFloatEntityValue(entity_id entityId);

    void sendLooperButtonAction(looper_button_action action);
    void toggleSdCardRecord();
    void snapshotAction(snapshot_action action, snapshot_slot slot);
    void tunerAction(tuner_action action, tuner_chan chan);
    float getBatteryLevel();
    sd_card_state getSdCardState();

    // High level entity setters
    void setFrontLed(bool state);
    void setFeedbackElim(uint8_t state);
    void setAmpPaMode(uint8_t state);
    void setLocationMode(uint8_t state);
    void setSelectedChan(uint8_t chan);
    void setInputGain(uint8_t input, float gain);
    void setInputVolume(uint8_t input, float volume);
    void setInputMute(uint8_t input, bool mute);
    void setInputEffectMute(uint8_t input, effect_channel effect, bool mute);
    void setInputEffectAmount(uint8_t input, effect_channel effect, float amount);
    void setInputEqEnable(uint8_t input, bool enable);
    void setInputEqGain(uint8_t input, eq_band band, float gain);
    void setInputCompressorEnable(uint8_t input, bool enable);
    void setInputCompressorAmount(uint8_t input, float amount);
    void setInputExtFxMute(uint8_t input, bool mute);
    void setInputExtFxSends(uint8_t input, float sends);
    void setInputEffectType(uint8_t input, effect_channel effect, uint8_t type);
    void setMainHeadphoneGain(float gain);
    void setMainMasterGain(float gain);
    void setMainMute(bool mute);
    void setLooperLevel(uint8_t level);
    void setFxBypassState(bool state);

    // High level entity getters
    bool getFrontLed();
    uint8_t getFeedbackElim();
    bool getAmpPaMode();
    bool getLocationMode();
    uint8_t getSelectedChan();
    float getInputGain(uint8_t input);
    float getInputVolume(uint8_t input);
    bool getInputMute(uint8_t input);
    bool getInputClipOlPre(uint8_t input);
    bool getInputClipOlPost(uint8_t input);
    bool getInputEffectMute(uint8_t input, effect_channel effect);
    float getInputEffectAmount(uint8_t input, effect_channel effect);
    bool getInputEqEnable(uint8_t input);
    float getInputEqGain(uint8_t input, eq_band band);
    bool getInputCompressorEnable(uint8_t input);
    float getInputCompressorAmount(uint8_t input);
    bool getInputExtFxMute(uint8_t input);
    float getInputExtFxSends(uint8_t input);
    uint8_t getInputEffectType(uint8_t input, effect_channel effect);
    float getMainHeadphoneGain();
    float getMainMasterGain();
    bool getMainMute();
    bool getMainClipOl();
    uint8_t getLooperLevel();
    looper_state getLooperState();
    bool getFxBypassState();
    uint8_t getTunerState();
    uint8_t getTunerChan();

    void tick();
    void begin();

    void setDebugSerial(Print* serial);

    void printRawPacket(const char* message, uint8_t* raw_packet);

    // post handle packet hook
    //void postHandlePacketHook(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result);
    std::function<void(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result)> postHandlePacketCallback = nullptr;
    /* Example usage:
    showbox->postHandlePacketCallback = [](uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result) {
        if (result == UARTInterceptor::PacketHandlerResult::PACKET_MODIFIED) {
            showbox->printRawPacket("Modified Packet: ", raw_packet);
        }
    };
    */
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