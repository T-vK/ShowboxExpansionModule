#include "debug.h"

void printRawPacket(const char* message, uint8_t* raw_packet) {
    Serial.printf("%s", message);
    uint8_t size = raw_packet[2] + 7;
    for (int i = 0; i < size; i++) {
        Serial.printf("%02X ", raw_packet[i]);
    }
    Serial.println();
}

std::unordered_map<packet_type, std::string> packet_type_to_string = {
    {UNKNOWN_00, "UNKNOWN_00"},
    {ACK, "ACK"},
    {ENTITY, "ENTITY"},
    {UNKNOWN_04, "UNKNOWN_04"},
    {UNKNOWN_MASS_TRANSFER, "UNKNOWN_MASS_TRANSFER"},
    {DATA_REQUEST, "DATA_REQUEST"},
    {UNKNOWN_15, "UNKNOWN_15"},
    {BATTERY_LEVEL, "BATTERY_LEVEL"},
    {LOOPER_BUTTON, "LOOPER_BUTTON"},
    {TUNER_TOGGLE, "TUNER_TOGGLE"},
    {TUNER_FEEDBACK, "TUNER_FEEDBACK"},
    {SNAPSHOT, "SNAPSHOT"},
    {SD_CARD_EVENT, "SD_CARD_EVENT"},
    {UNKNOWN_FE, "UNKNOWN_FE"},
    {HEARTBEAT, "HEARTBEAT"}
};

std::unordered_map<entity_data_type, std::string> entity_data_type_to_string = {
    {BOOL, "BOOL"},
    {UINT8, "UINT8"},
    {FLOAT, "FLOAT"},
};

std::unordered_map<entity_id, std::string> entity_id_to_string = {
    {FRONT_LED, "FRONT_LED"},
    {FEEDBACK_ELIM, "FEEDBACK_ELIM"},
    {AMP_PA_MODE, "AMP_PA_MODE"},
    {LOCATION_MODE, "LOCATION_MODE"},
    {SELECTED_CHAN, "SELECTED_CHAN"},
    {INPUT1_GAIN, "INPUT1_GAIN"},
    {INPUT1_VOLUME, "INPUT1_VOLUME"},
    {INPUT1_MUTE, "INPUT1_MUTE"},
    {INPUT1_CLIP_OL_PRE, "INPUT1_CLIP_OL_PRE"},
    {INPUT1_CLIP_OL_POST, "INPUT1_CLIP_OL_POST"},
    {INPUT1_EFFECT_1_MUTE, "INPUT1_EFFECT_1_MUTE"},
    {INPUT1_EFFECT_1_AMOUNT, "INPUT1_EFFECT_1_AMOUNT"},
    {INPUT1_EFFECT_2_MUTE, "INPUT1_EFFECT_2_MUTE"},
    {INPUT1_EFFECT_2_AMOUNT, "INPUT1_EFFECT_2_AMOUNT"},
    {INPUT1_EQ_ENABLE, "INPUT1_EQ_ENABLE"},
    {INPUT1_EQ_LOW_GAIN, "INPUT1_EQ_LOW_GAIN"},
    {INPUT1_EQ_MID_GAIN, "INPUT1_EQ_MID_GAIN"},
    {INPUT1_EQ_HIGH_GAIN, "INPUT1_EQ_HIGH_GAIN"},
    {INPUT1_COMPRESSOR_ENABLE, "INPUT1_COMPRESSOR_ENABLE"},
    {INPUT1_COMPRESSOR_AMOUNT, "INPUT1_COMPRESSOR_AMOUNT"},
    {INPUT1_EXT_FX_MUTE, "INPUT1_EXT_FX_MUTE"},
    {INPUT1_EXT_FX_SENDS, "INPUT1_EXT_FX_SENDS"},
    {INPUT2_GAIN, "INPUT2_GAIN"},
    {INPUT2_VOLUME, "INPUT2_VOLUME"},
    {INPUT2_MUTE, "INPUT2_MUTE"},
    {INPUT2_CLIP_OL_PRE, "INPUT2_CLIP_OL_PRE"},
    {INPUT2_CLIP_OL_POST, "INPUT2_CLIP_OL_POST"},
    {INPUT2_EFFECT_1_MUTE, "INPUT2_EFFECT_1_MUTE"},
    {INPUT2_EFFECT_1_AMOUNT, "INPUT2_EFFECT_1_AMOUNT"},
    {INPUT2_EFFECT_2_MUTE, "INPUT2_EFFECT_2_MUTE"},
    {INPUT2_EFFECT_2_AMOUNT, "INPUT2_EFFECT_2_AMOUNT"},
    {INPUT2_EQ_ENABLE, "INPUT2_EQ_ENABLE"},
    {INPUT2_EQ_LOW_GAIN, "INPUT2_EQ_LOW_GAIN"},
    {INPUT2_EQ_MID_GAIN, "INPUT2_EQ_MID_GAIN"},
    {INPUT2_EQ_HIGH_GAIN, "INPUT2_EQ_HIGH_GAIN"},
    {INPUT2_COMPRESSOR_ENABLE, "INPUT2_COMPRESSOR_ENABLE"},
    {INPUT2_COMPRESSOR_AMOUNT, "INPUT2_COMPRESSOR_AMOUNT"},
    {INPUT2_EXT_FX_MUTE, "INPUT2_EXT_FX_MUTE"},
    {INPUT2_EXT_FX_SENDS, "INPUT2_EXT_FX_SENDS"},
    {INPUT3_GAIN, "INPUT3_GAIN"},
    {INPUT3_VOLUME, "INPUT3_VOLUME"},
    {INPUT3_MUTE, "INPUT3_MUTE"},
    {INPUT3_CLIP_OL_PRE, "INPUT3_CLIP_OL_PRE"},
    {INPUT3_CLIP_OL_POST, "INPUT3_CLIP_OL_POST"},
    {INPUT3_EFFECT_1_MUTE, "INPUT3_EFFECT_1_MUTE"},
    {INPUT3_EFFECT_1_AMOUNT, "INPUT3_EFFECT_1_AMOUNT"},
    {INPUT3_EFFECT_2_MUTE, "INPUT3_EFFECT_2_MUTE"},
    {INPUT3_EFFECT_2_AMOUNT, "INPUT3_EFFECT_2_AMOUNT"},
    {INPUT3_EQ_ENABLE, "INPUT3_EQ_ENABLE"},
    {INPUT3_EQ_LOW_GAIN, "INPUT3_EQ_LOW_GAIN"},
    {INPUT3_EQ_MID_GAIN, "INPUT3_EQ_MID_GAIN"},
    {INPUT3_EQ_HIGH_GAIN, "INPUT3_EQ_HIGH_GAIN"},
    {INPUT3_COMPRESSOR_ENABLE, "INPUT3_COMPRESSOR_ENABLE"},
    {INPUT3_COMPRESSOR_AMOUNT, "INPUT3_COMPRESSOR_AMOUNT"},
    {INPUT3_EXT_FX_MUTE, "INPUT3_EXT_FX_MUTE"},
    {INPUT3_EXT_FX_SENDS, "INPUT3_EXT_FX_SENDS"},
    {INPUT4_GAIN, "INPUT4_GAIN"},
    {INPUT4_VOLUME, "INPUT4_VOLUME"},
    {INPUT4_MUTE, "INPUT4_MUTE"},
    {INPUT4_CLIP_OL_PRE, "INPUT4_CLIP_OL_PRE"},
    {INPUT4_CLIP_OL_POST, "INPUT4_CLIP_OL_POST"},
    {INPUT4_EFFECT_1_MUTE, "INPUT4_EFFECT_1_MUTE"},
    {INPUT4_EFFECT_1_AMOUNT, "INPUT4_EFFECT_1_AMOUNT"},
    {INPUT4_EFFECT_2_MUTE, "INPUT4_EFFECT_2_MUTE"},
    {INPUT4_EFFECT_2_AMOUNT, "INPUT4_EFFECT_2_AMOUNT"},
    {INPUT4_EQ_ENABLE, "INPUT4_EQ_ENABLE"},
    {INPUT4_EQ_LOW_GAIN, "INPUT4_EQ_LOW_GAIN"},
    {INPUT4_EQ_MID_GAIN, "INPUT4_EQ_MID_GAIN"},
    {INPUT4_EQ_HIGH_GAIN, "INPUT4_EQ_HIGH_GAIN"},
    {INPUT4_COMPRESSOR_ENABLE, "INPUT4_COMPRESSOR_ENABLE"},
    {INPUT4_COMPRESSOR_AMOUNT, "INPUT4_COMPRESSOR_AMOUNT"},
    {INPUT4_EXT_FX_MUTE, "INPUT4_EXT_FX_MUTE"},
    {INPUT4_EXT_FX_SENDS, "INPUT4_EXT_FX_SENDS"},
    {STEREO_INPUT1_VOLUME, "STEREO_INPUT1_VOLUME"},
    {STEREO_INPUT1_MUTE, "STEREO_INPUT1_MUTE"},
    {STEREO_INPUT1_CLIP_OL_PRE, "STEREO_INPUT1_CLIP_OL_PRE"},
    {STEREO_INPUT1_CLIP_OL_POST, "STEREO_INPUT1_CLIP_OL_POST"},
    {STEREO_INPUT1_EQ_ENABLE, "STEREO_INPUT1_EQ_ENABLE"},
    {STEREO_INPUT1_EQ_LOW_GAIN, "STEREO_INPUT1_EQ_LOW_GAIN"},
    {STEREO_INPUT1_EQ_MID_GAIN, "STEREO_INPUT1_EQ_MID_GAIN"},
    {STEREO_INPUT1_EQ_HIGH_GAIN, "STEREO_INPUT1_EQ_HIGH_GAIN"},
    {EFFECT11_TYPEID, "EFFECT11_TYPEID"},
    {EFFECT12_TYPEID, "EFFECT12_TYPEID"},
    {EFFECT13_TYPEID, "EFFECT13_TYPEID"},
    {EFFECT14_TYPEID, "EFFECT14_TYPEID"},
    {EFFECT21_TYPEID, "EFFECT21_TYPEID"},
    {EFFECT22_TYPEID, "EFFECT22_TYPEID"},
    {MAIN_HEADPHONE_GAIN, "MAIN_HEADPHONE_GAIN"},
    {MAIN_MASTER_GAIN, "MAIN_MASTER_GAIN"},
    {MAIN_MUTE, "MAIN_MUTE"},
    {MAIN_CLIP_OL, "MAIN_CLIP_OL"},
    {LOOPER_LEVEL, "LOOPER_LEVEL"},
    {LOOPER_STATE, "LOOPER_STATE"},
    {FX_BYPASS, "FX_BYPASS"}
};

std::unordered_map<uint8_t, std::string> looper_state_to_string = {
    {RECORD_INITIAL_LOOP, "RECORD_INITIAL_LOOP"},
    {PLAY, "PLAY"},
    {STOP_PLAYING, "STOP_PLAYING"},
    {RECORD_OVERDUB, "RECORD_OVERDUB"},
    {DELETE, "DELETE"}
};

std::map<float, std::string> master_gain_to_string = {
    {master_gain::POSITION_16, "16/16"},
    {master_gain::POSITION_15, "15/16"},
    {master_gain::POSITION_14, "14/16"},
    {master_gain::POSITION_13, "13/16"},
    {master_gain::POSITION_12, "12/16"},
    {master_gain::POSITION_11, "11/16"},
    {master_gain::POSITION_10, "10/16"},
    {master_gain::POSITION_9, "9/16"},
    {master_gain::POSITION_8, "8/16"},
    {master_gain::POSITION_7, "7/16"},
    {master_gain::POSITION_6, "6/16"},
    {master_gain::POSITION_5, "5/16"},
    {master_gain::POSITION_4, "4/16"},
    {master_gain::POSITION_3, "3/16"},
    {master_gain::POSITION_2, "2/16"},
    {master_gain::POSITION_1, "1/16"},
};