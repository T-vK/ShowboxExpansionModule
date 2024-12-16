#define SHOWBOX_DEBUG

#include "MackieShowbox.h"
#ifdef SHOWBOX_DEBUG
#include "string_mappings.h"
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
        UARTInterceptor::PacketHandlerResult result = this->handlePacket(packet, length, direction);
        if (postHandlePacketCallback != nullptr) {
            postHandlePacketCallback(packet, length, direction, result);
        }
        return result;
    });
    

    #ifdef SHOWBOX_DEBUG
    //Debug->println("\nMackieShowbox Interceptor ready\n");
    #endif
}

void MackieShowbox::setEntityValue(entity_id entityId, bool value, bool emit) {
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    }
    entityValues[entityId].boolValue = value;
}
void MackieShowbox::setEntityValue(entity_id entityId, uint8_t value, bool emit) {
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    }
    entityValues[entityId].uint8Value = value;
}
void MackieShowbox::setEntityValue(entity_id entityId, float value, bool emit) {
    if (emit) {
        uint8_t floatBytes[4];
        memcpy(floatBytes, &value, sizeof(float));

        uint8_t packet[15] = { 0xBE, 0xEF, 0x08, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, floatBytes[0], floatBytes[1], floatBytes[2], floatBytes[3], 0xEF, 0xBE };

        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
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

/**
    void sendLooperButtonAction(looper_button_action action);
    void toggleSdCardRecord();
    void snapshotAction(snapshot_action action, snapshot_slot slot);
    void tunerAction(tuner_action action, tuner_chan chan);
    float getBatteryLevel();
    sd_card_state getSdCardState();
 */

void MackieShowbox::toggleSdCardRecord() {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x00, SD_CARD_EVENT, 0x02, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

void MackieShowbox::snapshotAction(snapshot_action action, snapshot_slot slot) {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x01, SNAPSHOT, action, action, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

void MackieShowbox::tunerAction(tuner_action action, tuner_chan chan) {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x01, TUNER_TOGGLE, action, chan, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

float MackieShowbox::getBatteryLevel() {
    return batteryLevel;
}

sd_card_state MackieShowbox::getSdCardState() {
    return sdCardState;
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
        Debug->printf("%s [Decoded] Type: %s | Entity: %s | { Set %s to ", directionString.c_str(), packetTypeString.c_str(), entityString.c_str(), entityString.c_str());
        #endif
        if (entityType == BOOL) {
            bool value = raw_packet[9];
            #ifdef SHOWBOX_DEBUG
            Debug->printf("%s", value ? "true" : "false");
            #endif
            setEntityValue(entityId, value, false);
        } else if (entityType == UINT8) {
            uint8_t value = raw_packet[9];
            #ifdef SHOWBOX_DEBUG
            if (entityId == LOOPER_STATE) {
                Debug->printf("%s", looper_state_to_string[value].c_str());
            } else {
                Debug->printf("%d", value);
            }
            #endif
            setEntityValue(entityId, value, false);
        } else if (entityType == FLOAT) {
            float value;
            memcpy(&value, &raw_packet[9], sizeof(float));
            #ifdef SHOWBOX_DEBUG
            Debug->printf("%f", value);
            #endif
            setEntityValue(entityId, value, false);
        }
        #ifdef SHOWBOX_DEBUG
        Debug->print(" }\n");
        #endif
    } else if (packetType == HEARTBEAT) {
        #ifdef SHOWBOX_DEBUG
        // Debug->print("Heartbeat");
        //Debug->print(".");
        #endif
    } else if (packetType == ACK) {
        uint8_t ackCmd = raw_packet[5];
        if (ackCmd != HEARTBEAT && ackCmd != UNKNOWN_00 && ackCmd != UNKNOWN_FE) {
            #ifdef SHOWBOX_DEBUG
            String ackCmdString = packet_type_to_string[static_cast<packet_type>(ackCmd)].c_str();
            //printRawPacket("[Raw]: ", raw_packet);
            Debug->printf("%s Acknowoleged %s command\n", directionString.c_str(), ackCmdString.c_str());
            #endif
        }
        #ifdef SHOWBOX_DEBUG
        #endif
    } else if (packetType == DATA_REQUEST) {
        #ifdef SHOWBOX_DEBUG
        Debug->printf("%s [Decoded] Type: %s - ", directionString.c_str(), packetTypeString.c_str());
        printRawPacket("[Raw]: ", raw_packet);
        #endif
    } else if (packetType == LOOPER_BUTTON) {
        #ifdef SHOWBOX_DEBUG
        Debug->println("Looper Button");
        #endif
    } else if (packetType == BATTERY_LEVEL) {
        float value;
        memcpy(&value, &raw_packet[9], sizeof(float));
        batteryLevel = value;
        #ifdef SHOWBOX_DEBUG
        Debug->printf("%s [Decoded] Type: %s | Battery Level: %f\n", directionString.c_str(), packetTypeString.c_str(), value);
        #endif
    } else if (packetType == SD_CARD_EVENT) {
        sdCardState = static_cast<sd_card_state>(raw_packet[5]);
        #ifdef SHOWBOX_DEBUG
        if (sdCardState == sd_card_state::NOT_DETECTED) {
            Debug->printf("%s [Decoded] Type: %s | SD Card: Not Detected\n", directionString.c_str(), packetTypeString.c_str());
        } else if (sdCardState == sd_card_state::DETECTED) {
            Debug->printf("%s [Decoded] Type: %s | SD Card: Detected\n", directionString.c_str(), packetTypeString.c_str());
        } else {
            Debug->printf("%s [Decoded] Type: %s | SD Card: Unknown\n", directionString.c_str(), packetTypeString.c_str());
        } 
        #endif
    } else if (packetType == ALL_ENTITIES) {
        Debug->printf("%s [Decoded] Type: %s - Data:\n", directionString.c_str(), packetTypeString.c_str());
        //printRawPacket("[Raw]: ", raw_packet);

        uint8_t* bodyStart = &raw_packet[9]; // Start of the body (it actually starts at 6 with 3 unknown bytes)
        size_t offset = 0; // Offset from the start of the body

        // iterate over enum entity_id

        for (uint8_t i = 0; i <= FX_BYPASS; i++) {
            entity_id entityId = static_cast<entity_id>(i);
            entity_data_type dataType = entity_type_mapping[entityId];
            Debug->printf("            %s: ", entity_id_to_string[entityId].c_str());
            if (dataType == BOOL) {
                // Read 1 byte as a boolean
                bool value = bodyStart[offset];
                Debug->printf("%s", value ? "true" : "false");
                setEntityValue(entityId, value, false);
                offset += 1; // Advance by 1 byte
            } else if (dataType == UINT8) {
                // Read 1 byte as uint8_t
                uint8_t value = bodyStart[offset];
                Debug->printf("%d", value);
                setEntityValue(entityId, value, false);
                offset += 1; // Advance by 1 byte
            } else if (dataType == FLOAT) {
                // Read 4 bytes as a float
                float value;
                memcpy(&value, &bodyStart[offset], sizeof(float));
                Debug->printf("%f", value);
                setEntityValue(entityId, value, false);
                offset += 4; // Advance by 4 bytes
            }
            Debug->println();
            //Debug->printf(" (%s)\n", entity_data_type_to_string[dataType].c_str());
        }
    } else {
        #ifdef SHOWBOX_DEBUG
        Debug->printf("%s [Decoded] Type: %s - ", directionString.c_str(), packetTypeString.c_str());
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

void MackieShowbox::tick() {
    interceptor.tick();
}

// Set the debug serial
void MackieShowbox::setDebugSerial(Print* serial) {
    Debug = serial;
}

void MackieShowbox::printRawPacket(const char* message, uint8_t* raw_packet) {
    Debug->printf("%s", message);
    uint8_t size = raw_packet[2] + 7;
    for (int i = 0; i < size; i++) {
        Debug->printf("%02X ", raw_packet[i]);
    }
    Debug->println();
}

// High Level Setters

void MackieShowbox::setFrontLed(bool state) {
    setEntityValue(FRONT_LED, state);
}

void MackieShowbox::setFeedbackElim(uint8_t state) {
    setEntityValue(FEEDBACK_ELIM, state);
}

void MackieShowbox::setAmpPaMode(uint8_t mode) {
    setEntityValue(AMP_PA_MODE, mode);
}

void MackieShowbox::setLocationMode(uint8_t mode) {
    setEntityValue(LOCATION_MODE, mode);
}

void MackieShowbox::setSelectedChan(uint8_t chan) {
    setEntityValue(SELECTED_CHAN, chan);
}

void MackieShowbox::setInputGain(uint8_t input, float gain) {
    if (input >= 4) {
        return; // The Stereo channel does not have gain
    }
    uint8_t inputOffset = INPUT1_GAIN + 17 * input;
    uint8_t inputGainOffset = 0;
    entity_id entityId = static_cast<entity_id>(inputOffset + inputGainOffset);
    setEntityValue(entityId, gain);
}

void MackieShowbox::setInputVolume(uint8_t input, float volume) {
    uint8_t inputOffset = INPUT1_VOLUME + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_VOLUME;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, volume);
}

void MackieShowbox::setInputMute(uint8_t input, bool mute) {
    uint8_t inputOffset = INPUT1_MUTE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_MUTE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, mute);
}

void MackieShowbox::setInputEffectMute(uint8_t input, effect_channel effect, bool mute) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_MUTE + 17 * input;
    uint8_t inputEffectOffset = effect*2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    setEntityValue(entityId, mute);
}

void MackieShowbox::setInputEffectAmount(uint8_t input, effect_channel effect, float amount) {
    uint8_t inputOffset = INPUT1_EFFECT_1_AMOUNT + 17 * input;
    uint8_t inputEffectOffset = effect*2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    setEntityValue(entityId, amount);
}

void MackieShowbox::setInputEqEnable(uint8_t input, bool enable) {
    uint8_t inputOffset = INPUT1_EQ_ENABLE + 17 * input;
    if (input == 4) {
        inputOffset += 4; // Skip input effects on stereo channels because they are not available
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, enable);
}

void MackieShowbox::setInputEqGain(uint8_t input, eq_band band, float gain) {
    uint8_t inputOffset = INPUT1_EQ_LOW_GAIN + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_LOW_GAIN;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset + band);
    setEntityValue(entityId, gain);
}

void MackieShowbox::setInputCompressorEnable(uint8_t input, bool enable) {
    if (input >= 4) {
        return; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_ENABLE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, enable);
}

void MackieShowbox::setInputCompressorAmount(uint8_t input, float amount) {
    if (input >= 4) {
        return; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_AMOUNT + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, amount);
}

void MackieShowbox::setInputExtFxMute(uint8_t input, bool mute) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_MUTE + 17 * input;
    uint8_t inputExtFxMuteOffset = 15;
    entity_id entityId = static_cast<entity_id>(inputOffset + inputExtFxMuteOffset);
    setEntityValue(entityId, mute);
}

void MackieShowbox::setInputExtFxSends(uint8_t input, float sends) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_SENDS + 17 * input;
    uint8_t inputExtFxSendsOffset = 17;
    entity_id entityId = static_cast<entity_id>(inputOffset + inputExtFxSendsOffset);
    setEntityValue(entityId, sends);
}

void MackieShowbox::setInputEffectType(uint8_t input, effect_channel effect, uint8_t type) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t entityId;
    if (effect == effect_channel::EFFECT1) {
        entityId = 81+input;
    } else if (effect == effect_channel::EFFECT2) {
        if (input == 0 || input == 2) {
            entityId = 85;
        } else if (input == 1 || input == 3) {
            entityId = 86;
        }
    }
    setEntityValue(static_cast<entity_id>(entityId), type);
}

void MackieShowbox::setMainHeadphoneGain(float gain) {
    setEntityValue(MAIN_HEADPHONE_GAIN, gain);
}

void MackieShowbox::setMainMasterGain(float gain) {
    setEntityValue(MAIN_MASTER_GAIN, gain);
}

void MackieShowbox::setMainMute(bool mute) {
    setEntityValue(MAIN_MUTE, mute);
}

void MackieShowbox::setLooperLevel(uint8_t level) {
    setEntityValue(LOOPER_LEVEL, level);
}

void MackieShowbox::setFxBypassState(bool state) {
    setEntityValue(FX_BYPASS, state);
}

// High Level Getters

bool MackieShowbox::getFrontLed() {
    return getBoolEntityValue(FRONT_LED);
}

uint8_t MackieShowbox::getFeedbackElim() {
    return getUint8EntityValue(FEEDBACK_ELIM);
}

bool MackieShowbox::getAmpPaMode() {
    return getBoolEntityValue(AMP_PA_MODE);
}

bool MackieShowbox::getLocationMode() {
    return getBoolEntityValue(LOCATION_MODE);
}

uint8_t MackieShowbox::getSelectedChan() {
    return getUint8EntityValue(SELECTED_CHAN);
}

float MackieShowbox::getInputGain(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have gain
    }
    uint8_t inputOffset = INPUT1_GAIN + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Gain Entity ID: %d\n", entityId);
    return getFloatEntityValue(entityId);
}

float MackieShowbox::getInputVolume(uint8_t input) {
    uint8_t inputOffset = INPUT1_VOLUME + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_VOLUME;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Volume Entity ID: %d\n", entityId);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputMute(uint8_t input) {
    uint8_t inputOffset = INPUT1_MUTE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_MUTE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Muting Entity ID: %d\n", entityId);
    return getBoolEntityValue(entityId);
}

bool MackieShowbox::getInputEffectMute(uint8_t input, effect_channel effect) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_MUTE + 17 * input;
    uint8_t inputEffectOffset = effect * 2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    Debug->printf("Effect Mute Entity ID: %d\n", entityId);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputEffectAmount(uint8_t input, effect_channel effect) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_AMOUNT + 17 * input;
    uint8_t inputEffectOffset = effect * 2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    Debug->printf("Effect Amount Entity ID: %d\n", entityId);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputEqEnable(uint8_t input) {
    uint8_t inputOffset = INPUT1_EQ_ENABLE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_ENABLE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("EQ Enable Entity ID: %d\n", entityId);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputEqGain(uint8_t input, eq_band band) {
    uint8_t inputOffset = INPUT1_EQ_LOW_GAIN + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_LOW_GAIN;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset + band);
    Debug->printf("EQ Gain Entity ID: %d\n", entityId);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputCompressorEnable(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_ENABLE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Compressor Enable Entity ID: %d\n", entityId);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputCompressorAmount(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_AMOUNT + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Compressor Amount Entity ID: %d\n", entityId);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputExtFxMute(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have external effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_MUTE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Ext FX Mute Entity ID: %d\n", entityId);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputExtFxSends(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have external effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_SENDS + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    Debug->printf("Ext FX Sends Entity ID: %d\n", entityId);
    return getFloatEntityValue(entityId);
}

uint8_t MackieShowbox::getInputEffectType(uint8_t input, effect_channel effect) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have effects
    }
    uint8_t entityId;
    if (effect == effect_channel::EFFECT1) {
        entityId = 81 + input;
    } else if (effect == effect_channel::EFFECT2) {
        if (input == 0 || input == 2) {
            entityId = 85;
        } else if (input == 1 || input == 3) {
            entityId = 86;
        }
    }
    Debug->printf("Effect Type Entity ID: %d\n", entityId);
    return getUint8EntityValue(static_cast<entity_id>(entityId));
}

float MackieShowbox::getMainHeadphoneGain() {
    return getFloatEntityValue(MAIN_HEADPHONE_GAIN);
}

float MackieShowbox::getMainMasterGain() {
    return getFloatEntityValue(MAIN_MASTER_GAIN);
}

bool MackieShowbox::getMainMute() {
    return getBoolEntityValue(MAIN_MUTE);
}

uint8_t MackieShowbox::getLooperLevel() {
    return getUint8EntityValue(LOOPER_LEVEL);
}

looper_state MackieShowbox::getLooperState() {
    return static_cast<looper_state>(getUint8EntityValue(LOOPER_STATE));
}

bool MackieShowbox::getFxBypassState() {
    return getBoolEntityValue(FX_BYPASS);
}

