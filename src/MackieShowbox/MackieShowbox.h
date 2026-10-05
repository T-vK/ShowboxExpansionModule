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
    bool hasBatteryLevel() const { return batteryHeard; }
    sd_card_state getSdCardState();
    void setParsing(bool enabled) { parseEnabled = enabled; }
    bool parsing() const { return parseEnabled; }
    uint32_t linkAgeMs() const;
    uint32_t heartbeatAgeMs() const;
    uint32_t baseAgeMs() const;
    uint32_t mixerAgeMs() const;
    uint32_t baseTxAgeMs() const;
    uint32_t mixerTxAgeMs() const;
    uint32_t rxFromBase() const;
    uint32_t rxFromMixer() const;
    uint32_t droppedFromBase() const;
    uint32_t droppedFromMixer() const;
    uint32_t overflowFromBase() const;
    uint32_t overflowFromMixer() const;
    uint32_t frameErrFromBase() const;
    uint32_t frameErrFromMixer() const;
    uint32_t maxGapFromBase() const;
    uint32_t maxGapFromMixer() const;
    uint32_t goodFromMixer() const { return goodMix; }
    uint32_t goodFromBase() const { return goodBase; }
    uint32_t badFromMixer() const { return badMix; }
    uint32_t badFromBase() const { return badBase; }
    uint32_t hwRxFifoBase() const;
    uint32_t hwRxFifoMixer() const;
    uint32_t hwIntStBase() const;
    uint32_t hwIntStMixer() const;
    size_t pendingToMixer() const;
    size_t pendingToBase() const;
    size_t ringFromBase() const;
    size_t ringFromMixer() const;
    size_t holdFromBase() const;
    size_t holdFromMixer() const;
    uint32_t cutThroughFromBase() const;
    uint32_t cutThroughFromMixer() const;
    bool txToBasePaused() const;
    uint32_t txPauseCount() const;
    uint32_t initFromMixer() const { return initMix; }
    uint32_t initFromBase() const { return initBase; }
    uint32_t hbFromMixer() const { return hbMix; }
    uint32_t hbFromBase() const { return hbBase; }
    uint32_t entityFromMixer() const { return entMix; }
    uint32_t requestFromMixer() const { return reqMix; }
    uint32_t restoreCountFromMixer() const { return restoreCount; }
    bool linkOk(uint32_t staleMs = 1500) const;
    bool uartHot(uint32_t windowMs = 40) const;
    void waitQuiet(uint32_t maxMs = 40) const;

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
    void setLooperLevel(float level);
    void setLooperState(looper_state state);
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
    float getLooperLevel();
    looper_state getLooperState();
    bool getFxBypassState();
    uint8_t getTunerState();
    uint8_t getTunerChan();

    void tick();
    void begin();
    static void uartTask(void* arg);

    void setDebugSerial(Print* serial);
    void setPacketLog(Print* serial);
    void setPacketLogging(bool enable);
    bool packetLogging() const { return packetLogEnabled; }

    void printRawPacket(const char* message, uint8_t* raw_packet, size_t length);

    // post handle packet hook
    //void postHandlePacketHook(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result);
    std::function<void(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result)> postHandlePacketCallback = nullptr;
    void (*entityListener)(entity_id id, const char* value) = nullptr;
    /* Example usage:
    showbox->postHandlePacketCallback = [](uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result) {
        if (result == UARTInterceptor::PacketHandlerResult::PACKET_MODIFIED) {
            showbox->printRawPacket("Modified Packet: ", raw_packet, length);
        }
    };
    */
private:
    uint8_t baseRx, baseTx, mixerRx, mixerTx;
    float batteryLevel = 0.0f;
    bool batteryHeard = false;
    bool parseEnabled = true;
    bool uartStarted = false;
    volatile uint32_t lastPacketMs = 0;
    volatile uint32_t lastHeartbeatMs = 0;
    static constexpr int kParseQ = 64;
    struct ParseItem {
        uint8_t data[280];
        uint16_t length;
        uint8_t direction;
    };
    TaskHandle_t uartTaskHandle = nullptr;
    ParseItem parseQ[kParseQ] = {};
    volatile uint8_t parseQHead = 0;
    volatile uint8_t parseQTail = 0;
    portMUX_TYPE parseMux = portMUX_INITIALIZER_UNLOCKED;
    void enqueueParse(uint8_t* packet, size_t length, UARTInterceptor::Direction direction);
    void drainParse();
    sd_card_state sdCardState = sd_card_state::NOT_DETECTED;
    Print* Debug = &Serial;
    Print* packetLog = nullptr;
    bool packetLogEnabled = false;
    struct EntityValue {
        bool boolValue;
        uint8_t uint8Value;
        float floatValue;

        EntityValue() : boolValue(false), uint8Value(0), floatValue(0.0f) {}
    };
    std::unordered_map<entity_id, EntityValue> entityValues;
    volatile uint32_t initMix = 0;
    volatile uint32_t initBase = 0;
    volatile uint32_t hbMix = 0;
    volatile uint32_t hbBase = 0;
    volatile uint32_t entMix = 0;
    volatile uint32_t reqMix = 0;
    volatile uint32_t goodMix = 0;
    volatile uint32_t goodBase = 0;
    volatile uint32_t badMix = 0;
    volatile uint32_t badBase = 0;
    bool mixerTouched[FX_BYPASS + 1] = {};
    volatile uint32_t restoreAtMs = 0;
    volatile uint32_t ignoreTouchUntil = 0;
    volatile uint32_t restoreCount = 0;
    void restoreMixerEntities();
    void rememberMixerEntity(const uint8_t* packet, size_t length);
    bool mixerOwnsEntity(entity_id id) const;
    bool uartTaskRunning = false;
    UARTInterceptor interceptor;
    UARTInterceptor::Direction TO_MIXER = UARTInterceptor::DEVICE1_TO_DEVICE2;
    UARTInterceptor::Direction TO_BASE = UARTInterceptor::DEVICE2_TO_DEVICE1;

    UARTInterceptor::PacketHandlerResult handlePacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction);
    void logPacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction);
};

#endif // MACKIESHOWBOX_H