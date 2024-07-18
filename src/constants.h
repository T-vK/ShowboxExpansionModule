// constants.h
#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <unordered_map>

enum packet_type : uint8_t {
    ACK = 0x01,
    ENTITY = 0x03,
    UNKNOWN_04 = 0x04,
    UNKNOWN_05 = 0x05,
    DATA_REQUEST = 0x09,
    UNKNOWN_15 = 0x15,
    UNKNOWN_16 = 0x16,
    TUNER_TOGGLE = 0x18,
    TUNER_FEEDBACK = 0x19,
    SNAPSHOT = 0x1A,
    SD_CARD_EVENT = 0x1E,
    HEARTBEAT = 0xFF
};

enum entity_data_type : uint8_t {
   BOOL = 0,
   UINT8 = 1,
   FLOAT = 2,
};

enum entity_id : uint8_t {
    FRONT_LED = 0,
    FEEDBACK_ELIM = 1,
    AMP_PA_MODE = 2,
    LOCATION_MODE = 3,
    SELECTED_CHAN = 4,
    INPUT1_GAIN = 5,
    INPUT1_VOLUME = 6,
    INPUT1_MUTE = 7,
    INPUT1_CLIP_OL_PRE = 8,
    INPUT1_CLIP_OL_POST = 9,
    INPUT1_EFFECT_1_MUTE = 10,
    INPUT1_EFFECT_1_AMOUNT = 11,
    INPUT1_EFFECT_2_MUTE = 12,
    INPUT1_EFFECT_2_AMOUNT = 13,
    INPUT1_EQ_ENABLE = 14,
    INPUT1_EQ_LOW_GAIN = 15,
    INPUT1_EQ_MID_GAIN = 16,
    INPUT1_EQ_HIGH_GAIN = 17,
    INPUT1_COMPRESSOR_ENABLE = 18,
    INPUT1_COMPRESSOR_AMOUNT = 19,
    INPUT1_EXT_FX_MUTE = 20,
    INPUT1_EXT_FX_SENDS = 21,
    INPUT2_GAIN = 22,
    INPUT2_VOLUME = 23,
    INPUT2_MUTE = 24,
    INPUT2_CLIP_OL_PRE = 25,
    INPUT2_CLIP_OL_POST = 26,
    INPUT2_EFFECT_1_MUTE = 27,
    INPUT2_EFFECT_1_AMOUNT = 28,
    INPUT2_EFFECT_2_MUTE = 29,
    INPUT2_EFFECT_2_AMOUNT = 30,
    INPUT2_EQ_ENABLE = 31,
    INPUT2_EQ_LOW_GAIN = 32,
    INPUT2_EQ_MID_GAIN = 33,
    INPUT2_EQ_HIGH_GAIN = 34,
    INPUT2_COMPRESSOR_ENABLE = 35,
    INPUT2_COMPRESSOR_AMOUNT = 36,
    INPUT2_EXT_FX_MUTE = 37,
    INPUT2_EXT_FX_SENDS = 38,
    INPUT3_GAIN = 39,
    INPUT3_VOLUME = 40,
    INPUT3_MUTE = 41,
    INPUT3_CLIP_OL_PRE = 42,
    INPUT3_CLIP_OL_POST = 43,
    INPUT3_EFFECT_1_MUTE = 44,
    INPUT3_EFFECT_1_AMOUNT = 45,
    INPUT3_EFFECT_2_MUTE = 46,
    INPUT3_EFFECT_2_AMOUNT = 47,
    INPUT3_EQ_ENABLE = 48,
    INPUT3_EQ_LOW_GAIN = 49,
    INPUT3_EQ_MID_GAIN = 50,
    INPUT3_EQ_HIGH_GAIN = 51,
    INPUT3_COMPRESSOR_ENABLE = 52,
    INPUT3_COMPRESSOR_AMOUNT = 53,
    INPUT3_EXT_FX_MUTE = 54,
    INPUT3_EXT_FX_SENDS = 55,
    INPUT4_GAIN = 56,
    INPUT4_VOLUME = 57,
    INPUT4_MUTE = 58,
    INPUT4_CLIP_OL_PRE = 59,
    INPUT4_CLIP_OL_POST = 60,
    INPUT4_EFFECT_1_MUTE = 61,
    INPUT4_EFFECT_1_AMOUNT = 62,
    INPUT4_EFFECT_2_MUTE = 63,
    INPUT4_EFFECT_2_AMOUNT = 64,
    INPUT4_EQ_ENABLE = 65,
    INPUT4_EQ_LOW_GAIN = 66,
    INPUT4_EQ_MID_GAIN = 67,
    INPUT4_EQ_HIGH_GAIN = 68,
    INPUT4_COMPRESSOR_ENABLE = 69,
    INPUT4_COMPRESSOR_AMOUNT = 70,
    INPUT4_EXT_FX_MUTE = 71,
    INPUT4_EXT_FX_SENDS = 72,
    STEREO_INPUT1_VOLUME = 73,
    STEREO_INPUT1_MUTE = 74,
    STEREO_INPUT1_CLIP_OL_PRE = 75,
    STEREO_INPUT1_CLIP_OL_POST = 76,
    STEREO_INPUT1_EQ_ENABLE = 77,
    STEREO_INPUT1_EQ_LOW_GAIN = 78,
    STEREO_INPUT1_EQ_MID_GAIN = 79,
    STEREO_INPUT1_EQ_HIGH_GAIN = 80,
    EFFECT11_TYPEID = 81,
    EFFECT12_TYPEID = 82,
    EFFECT13_TYPEID = 83,
    EFFECT14_TYPEID = 84,
    EFFECT21_TYPEID = 85,
    EFFECT22_TYPEID = 86,
    MAIN_HEADPHONE_GAIN = 87,
    MAIN_MASTER_GAIN = 88,
    MAIN_MUTE = 89,
    MAIN_CLIP_OL = 90,
    LOOPER_LEVEL = 91,
    LOOPER_STATE = 92,
    FX_BYPASS = 93
};

extern std::unordered_map<entity_id, entity_data_type> entity_type_mapping;

enum looper_state : uint8_t {
    RECORD_INITIAL_LOOP = 1,
    PLAY = 3,
    STOP_PLAYING = 2,
    RECORD_OVERDUB = 4,
    DELETE = 0
};

namespace Showbox {
    namespace DataTypes {
        typedef uint8_t FRONT_LED_STATE;
        typedef uint8_t FEEDBACK_ELIM_STATE;
        typedef uint8_t AMP_PA_MODE_STATE;
        typedef uint8_t LOCATION_MODE_STATE;
        typedef uint8_t SELECTED_CHAN;
        typedef uint8_t INPUT_GAIN;
        typedef uint8_t INPUT_VOLUME;
        typedef uint8_t INPUT_MUTE_STATE;
        typedef uint8_t INPUT_CLIP_OL_PRE_STATE;
        typedef uint8_t INPUT_CLIP_OL_POST_STATE;
        typedef uint8_t INPUT_EFFECT_MUTE_STATE;
        typedef uint8_t INPUT_EFFECT_AMOUNT;
        typedef uint8_t INPUT_EQ_ENABLE_STATE;
        typedef uint8_t INPUT_EQ_GAIN;
        typedef uint8_t INPUT_COMPRESSOR_ENABLE_STATE;
        typedef uint8_t INPUT_COMPRESSOR_AMOUNT;
        typedef uint8_t INPUT_EXT_FX_MUTE_STATE;
        typedef uint8_t INPUT_EXT_FX_SENDS;
        typedef uint8_t EFFECT_TYPEID;
        typedef uint8_t EFFECT1_CHAN;
        typedef uint8_t EFFECT2_CHAN;
        typedef uint8_t MAIN_HEADPHONE_GAIN;
        typedef uint8_t MAIN_MASTER_GAIN;
        typedef uint8_t MAIN_MUTE_STATE;
        typedef uint8_t MAIN_CLIP_OL_STATE;
        typedef uint8_t LOOPER_LEVEL;
        typedef uint8_t LOOPER_STATE;
        typedef uint8_t FX_BYPASS_STATE;
        typedef uint8_t TUNER_STATE;
        typedef uint8_t TUNER_CHAN;
        typedef uint8_t SNAPSHOT_ACTION;
        typedef uint8_t SNAPSHOT_SLOT;
        typedef uint8_t SD_RECORD_STATE;
    } // namespace DataTypes

    namespace FrontLed {
        constexpr uint8_t ON = 0x01;
        constexpr uint8_t OFF = 0x00;
    } // namespace FrontLed
    namespace FeedbackElim {
        constexpr uint8_t ON = 0x01;
        constexpr uint8_t OFF = 0x00;
        constexpr uint8_t UNKNOWN = 0x02;
    } // namespace FeedbackElim
    namespace AmpPaMode {
        constexpr uint8_t AMP = 0x01;
        constexpr uint8_t PA = 0x00;
    } // namespace AmpPaMode
    namespace LocationMode {
        constexpr uint8_t OUTDOOR = 0x01;
        constexpr uint8_t INDOOR = 0x00;
    } // namespace LocationMode
    namespace SelectedChan {
        constexpr uint8_t CHAN1 = 0x00;
        constexpr uint8_t CHAN2 = 0x01;
        constexpr uint8_t CHAN3 = 0x02;
        constexpr uint8_t CHAN4 = 0x03;
        constexpr uint8_t CHAN5_6 = 0x04;
    } // namespace SelectedChan
    namespace MainMasterGain {
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
    } // namespace MainMasterGain
} // namespace Showbox


#endif // CONSTANTS_H