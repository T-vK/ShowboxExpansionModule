// Entity.h
#ifndef ENTITY_H
#define ENTITY_H

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace Showbox {
    namespace Entity {
        constexpr uint8_t FRONT_LED = 0;
        constexpr uint8_t FEEDBACK_ELIM = 1;
        constexpr uint8_t AMP_PA_MODE = 2;
        constexpr uint8_t LOCATION_MODE = 3;
        constexpr uint8_t SELECTED_CHAN = 4;
        constexpr uint8_t INPUT1_GAIN = 5;
        constexpr uint8_t INPUT1_VOLUME = 6;
        constexpr uint8_t INPUT1_MUTE = 7;
        constexpr uint8_t INPUT1_CLIP_OL_PRE = 8;
        constexpr uint8_t INPUT1_CLIP_OL_POST = 9;
        constexpr uint8_t INPUT1_EFFECT_1_MUTE = 10;
        constexpr uint8_t INPUT1_EFFECT_1_AMOUNT = 11;
        constexpr uint8_t INPUT1_EFFECT_2_MUTE = 12;
        constexpr uint8_t INPUT1_EFFECT_2_AMOUNT = 13;
        constexpr uint8_t INPUT1_EQ_ENABLE = 14;
        constexpr uint8_t INPUT1_EQ_LOW_GAIN = 15;
        constexpr uint8_t INPUT1_EQ_MID_GAIN = 16;
        constexpr uint8_t INPUT1_EQ_HIGH_GAIN = 17;
        constexpr uint8_t INPUT1_COMPRESSOR_ENABLE = 18;
        constexpr uint8_t INPUT1_COMPRESSOR_AMOUNT = 19;
        constexpr uint8_t INPUT1_EXT_FX_MUTE = 20;
        constexpr uint8_t INPUT1_EXT_FX_SENDS = 21;
        constexpr uint8_t INPUT2_GAIN = 22;
        constexpr uint8_t INPUT2_VOLUME = 23;
        constexpr uint8_t INPUT2_MUTE = 24;
        constexpr uint8_t INPUT2_CLIP_OL_PRE = 25;
        constexpr uint8_t INPUT2_CLIP_OL_POST = 26;
        constexpr uint8_t INPUT2_EFFECT_1_MUTE = 27;
        constexpr uint8_t INPUT2_EFFECT_1_AMOUNT = 28;
        constexpr uint8_t INPUT2_EFFECT_2_MUTE = 29;
        constexpr uint8_t INPUT2_EFFECT_2_AMOUNT = 30;
        constexpr uint8_t INPUT2_EQ_ENABLE = 31;
        constexpr uint8_t INPUT2_EQ_LOW_GAIN = 32;
        constexpr uint8_t INPUT2_EQ_MID_GAIN = 33;
        constexpr uint8_t INPUT2_EQ_HIGH_GAIN = 34;
        constexpr uint8_t INPUT2_COMPRESSOR_ENABLE = 35;
        constexpr uint8_t INPUT2_COMPRESSOR_AMOUNT = 36;
        constexpr uint8_t INPUT2_EXT_FX_MUTE = 37;
        constexpr uint8_t INPUT2_EXT_FX_SENDS = 38;
        constexpr uint8_t INPUT3_GAIN = 39;
        constexpr uint8_t INPUT3_VOLUME = 40;
        constexpr uint8_t INPUT3_MUTE = 41;
        constexpr uint8_t INPUT3_CLIP_OL_PRE = 42;
        constexpr uint8_t INPUT3_CLIP_OL_POST = 43;
        constexpr uint8_t INPUT3_EFFECT_1_MUTE = 44;
        constexpr uint8_t INPUT3_EFFECT_1_AMOUNT = 45;
        constexpr uint8_t INPUT3_EFFECT_2_MUTE = 46;
        constexpr uint8_t INPUT3_EFFECT_2_AMOUNT = 47;
        constexpr uint8_t INPUT3_EQ_ENABLE = 48;
        constexpr uint8_t INPUT3_EQ_LOW_GAIN = 49;
        constexpr uint8_t INPUT3_EQ_MID_GAIN = 50;
        constexpr uint8_t INPUT3_EQ_HIGH_GAIN = 51;
        constexpr uint8_t INPUT3_COMPRESSOR_ENABLE = 52;
        constexpr uint8_t INPUT3_COMPRESSOR_AMOUNT = 53;
        constexpr uint8_t INPUT3_EXT_FX_MUTE = 54;
        constexpr uint8_t INPUT3_EXT_FX_SENDS = 55;
        constexpr uint8_t INPUT4_GAIN = 56;
        constexpr uint8_t INPUT4_VOLUME = 57;
        constexpr uint8_t INPUT4_MUTE = 58;
        constexpr uint8_t INPUT4_CLIP_OL_PRE = 59;
        constexpr uint8_t INPUT4_CLIP_OL_POST = 60;
        constexpr uint8_t INPUT4_EFFECT_1_MUTE = 61;
        constexpr uint8_t INPUT4_EFFECT_1_AMOUNT = 62;
        constexpr uint8_t INPUT4_EFFECT_2_MUTE = 63;
        constexpr uint8_t INPUT4_EFFECT_2_AMOUNT = 64;
        constexpr uint8_t INPUT4_EQ_ENABLE = 65;
        constexpr uint8_t INPUT4_EQ_LOW_GAIN = 66;
        constexpr uint8_t INPUT4_EQ_MID_GAIN = 67;
        constexpr uint8_t INPUT4_EQ_HIGH_GAIN = 68;
        constexpr uint8_t INPUT4_COMPRESSOR_ENABLE = 69;
        constexpr uint8_t INPUT4_COMPRESSOR_AMOUNT = 70;
        constexpr uint8_t INPUT4_EXT_FX_MUTE = 71;
        constexpr uint8_t INPUT4_EXT_FX_SENDS = 72;
        constexpr uint8_t STEREO_INPUT1_VOLUME = 73;
        constexpr uint8_t STEREO_INPUT1_MUTE = 74;
        constexpr uint8_t STEREO_INPUT1_CLIP_OL_PRE = 75;
        constexpr uint8_t STEREO_INPUT1_CLIP_OL_POST = 76;
        constexpr uint8_t STEREO_INPUT1_EQ_ENABLE = 77;
        constexpr uint8_t STEREO_INPUT1_EQ_LOW_GAIN = 78;
        constexpr uint8_t STEREO_INPUT1_EQ_MID_GAIN = 79;
        constexpr uint8_t STEREO_INPUT1_EQ_HIGH_GAIN = 80;
        constexpr uint8_t EFFECT11_TYPEID = 81;
        constexpr uint8_t EFFECT12_TYPEID = 82;
        constexpr uint8_t EFFECT13_TYPEID = 83;
        constexpr uint8_t EFFECT14_TYPEID = 84;
        constexpr uint8_t EFFECT21_TYPEID = 85;
        constexpr uint8_t EFFECT22_TYPEID = 86;
        constexpr uint8_t MAIN_HEADPHONE_GAIN = 87;
        constexpr uint8_t MAIN_MASTER_GAIN = 88;
        constexpr uint8_t MAIN_MUTE = 89;
        constexpr uint8_t MAIN_CLIP_OL = 90;
        constexpr uint8_t LOOPER_LEVEL = 91;
        constexpr uint8_t LOOPER_STATE = 92;
        constexpr uint8_t FX_BYPASS = 93;

        namespace DataTypes {
            constexpr uint8_t BOOL = 0;
            constexpr uint8_t UINT8 = 1;
            constexpr uint8_t FLOAT_LE = 2;
        } // namespace DataTypes

        uint8_t getDataType(uint8_t entityId) {
            switch (entityId) {
                case FRONT_LED: return DataTypes::UINT8;
                case FEEDBACK_ELIM: return DataTypes::UINT8;
                case AMP_PA_MODE: return DataTypes::UINT8;
                case LOCATION_MODE: return DataTypes::UINT8;
                case SELECTED_CHAN: return DataTypes::UINT8;
                case INPUT1_GAIN: return DataTypes::UINT8;
                case INPUT1_VOLUME: return DataTypes::FLOAT_LE;
                case INPUT1_MUTE: return DataTypes::UINT8;
                case INPUT1_CLIP_OL_PRE: return DataTypes::UINT8;
                case INPUT1_CLIP_OL_POST: return DataTypes::UINT8;
                case INPUT1_EFFECT_1_MUTE: return DataTypes::UINT8;
                case INPUT1_EFFECT_1_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT1_EFFECT_2_MUTE: return DataTypes::UINT8;
                case INPUT1_EFFECT_2_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT1_EQ_ENABLE: return DataTypes::UINT8;
                case INPUT1_EQ_LOW_GAIN: return DataTypes::FLOAT_LE;
                case INPUT1_EQ_MID_GAIN: return DataTypes::FLOAT_LE;
                case INPUT1_EQ_HIGH_GAIN: return DataTypes::FLOAT_LE;
                case INPUT1_COMPRESSOR_ENABLE: return DataTypes::UINT8;
                case INPUT1_COMPRESSOR_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT1_EXT_FX_MUTE: return DataTypes::UINT8;
                case INPUT1_EXT_FX_SENDS: return DataTypes::UINT8;
                case INPUT2_GAIN: return DataTypes::UINT8;
                case INPUT2_VOLUME: return DataTypes::FLOAT_LE;
                case INPUT2_MUTE: return DataTypes::UINT8;
                case INPUT2_CLIP_OL_PRE: return DataTypes::UINT8;
                case INPUT2_CLIP_OL_POST: return DataTypes::UINT8;
                case INPUT2_EFFECT_1_MUTE: return DataTypes::UINT8;
                case INPUT2_EFFECT_1_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT2_EFFECT_2_MUTE: return DataTypes::UINT8;
                case INPUT2_EFFECT_2_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT2_EQ_ENABLE: return DataTypes::UINT8;
                case INPUT2_EQ_LOW_GAIN: return DataTypes::FLOAT_LE;
                case INPUT2_EQ_MID_GAIN: return DataTypes::FLOAT_LE;
                case INPUT2_EQ_HIGH_GAIN: return DataTypes::FLOAT_LE;
                case INPUT2_COMPRESSOR_ENABLE: return DataTypes::UINT8;
                case INPUT2_COMPRESSOR_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT2_EXT_FX_MUTE: return DataTypes::UINT8;
                case INPUT2_EXT_FX_SENDS: return DataTypes::UINT8;
                case INPUT3_GAIN: return DataTypes::UINT8;
                case INPUT3_VOLUME: return DataTypes::FLOAT_LE;
                case INPUT3_MUTE: return DataTypes::UINT8;
                case INPUT3_CLIP_OL_PRE: return DataTypes::UINT8;
                case INPUT3_CLIP_OL_POST: return DataTypes::UINT8;
                case INPUT3_EFFECT_1_MUTE: return DataTypes::UINT8;  
                case INPUT3_EFFECT_1_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT3_EFFECT_2_MUTE: return DataTypes::UINT8;
                case INPUT3_EFFECT_2_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT3_EQ_ENABLE: return DataTypes::UINT8;
                case INPUT3_EQ_LOW_GAIN: return DataTypes::FLOAT_LE;
                case INPUT3_EQ_MID_GAIN: return DataTypes::FLOAT_LE;
                case INPUT3_EQ_HIGH_GAIN: return DataTypes::FLOAT_LE;
                case INPUT3_COMPRESSOR_ENABLE: return DataTypes::UINT8;
                case INPUT3_COMPRESSOR_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT3_EXT_FX_MUTE: return DataTypes::UINT8;
                case INPUT3_EXT_FX_SENDS: return DataTypes::UINT8;
                case INPUT4_GAIN: return DataTypes::UINT8;
                case INPUT4_VOLUME: return DataTypes::FLOAT_LE;
                case INPUT4_MUTE: return DataTypes::UINT8;
                case INPUT4_CLIP_OL_PRE: return DataTypes::UINT8;
                case INPUT4_CLIP_OL_POST: return DataTypes::UINT8;
                case INPUT4_EFFECT_1_MUTE: return DataTypes::UINT8;
                case INPUT4_EFFECT_1_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT4_EFFECT_2_MUTE: return DataTypes::UINT8;
                case INPUT4_EFFECT_2_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT4_EQ_ENABLE: return DataTypes::UINT8;
                case INPUT4_EQ_LOW_GAIN: return DataTypes::FLOAT_LE;
                case INPUT4_EQ_MID_GAIN: return DataTypes::FLOAT_LE;
                case INPUT4_EQ_HIGH_GAIN: return DataTypes::FLOAT_LE;
                case INPUT4_COMPRESSOR_ENABLE: return DataTypes::UINT8;
                case INPUT4_COMPRESSOR_AMOUNT: return DataTypes::FLOAT_LE;
                case INPUT4_EXT_FX_MUTE: return DataTypes::UINT8;
                case INPUT4_EXT_FX_SENDS: return DataTypes::UINT8;
                case STEREO_INPUT1_VOLUME: return DataTypes::FLOAT_LE;
                case STEREO_INPUT1_MUTE: return DataTypes::UINT8;
                case STEREO_INPUT1_CLIP_OL_PRE: return DataTypes::UINT8;
                case STEREO_INPUT1_CLIP_OL_POST: return DataTypes::UINT8;
                case STEREO_INPUT1_EQ_ENABLE: return DataTypes::UINT8;
                case STEREO_INPUT1_EQ_LOW_GAIN: return DataTypes::FLOAT_LE;
                case STEREO_INPUT1_EQ_MID_GAIN: return DataTypes::FLOAT_LE;
                case STEREO_INPUT1_EQ_HIGH_GAIN: return DataTypes::FLOAT_LE;
                case EFFECT11_TYPEID: return DataTypes::UINT8;
                case EFFECT12_TYPEID: return DataTypes::UINT8;
                case EFFECT13_TYPEID: return DataTypes::UINT8;
                case EFFECT14_TYPEID: return DataTypes::UINT8;
                case EFFECT21_TYPEID: return DataTypes::UINT8;
                case EFFECT22_TYPEID: return DataTypes::UINT8;
                case MAIN_HEADPHONE_GAIN: return DataTypes::FLOAT_LE;
                case MAIN_MASTER_GAIN: return DataTypes::FLOAT_LE;
                case MAIN_MUTE: return DataTypes::UINT8;
                case MAIN_CLIP_OL: return DataTypes::UINT8;
                case LOOPER_LEVEL: return DataTypes::FLOAT_LE;
                case LOOPER_STATE: return DataTypes::UINT8;
                case FX_BYPASS: return DataTypes::UINT8;
                default: return 0;
            }
        } 

        // there are dB, % and custom
        namespace Units {
            constexpr uint8_t ON_OFF = 0;
            constexpr uint8_t DB = 1;
            constexpr uint8_t PERCENT = 2;
            constexpr uint8_t STATE = 3;
            constexpr uint8_t UNKNOWN = 4;

            #ifdef SHOWBOX_DEBUG
            inline const char* getName(uint8_t unitId) {
                switch (unitId) {
                    case ON_OFF: return "";
                    case DB: return "dB";
                    case PERCENT: return "%";
                    case STATE: return "";
                    case UNKNOWN: return " Unknown unit";
                    default: return " Unknown unit";
                }
            }
            #endif // SHOWBOX_DEBUG
        } // namespace Units

        #ifdef SHOWBOX_DEBUG
        inline uint8_t getUnit(uint8_t entityId) {
            switch (entityId) {
                case FRONT_LED: return Units::ON_OFF;
                case FEEDBACK_ELIM: return Units::STATE;
                case AMP_PA_MODE: return Units::STATE;
                case LOCATION_MODE: return Units::STATE;
                case SELECTED_CHAN: return Units::STATE;
                case INPUT1_GAIN: return Units::DB;
                case INPUT1_VOLUME: return Units::DB;
                case INPUT1_MUTE: return Units::ON_OFF;
                case INPUT1_CLIP_OL_PRE: return Units::ON_OFF;
                case INPUT1_CLIP_OL_POST: return Units::ON_OFF;
                case INPUT1_EFFECT_1_MUTE: return Units::ON_OFF;
                case INPUT1_EFFECT_1_AMOUNT: return Units::DB;
                case INPUT1_EFFECT_2_MUTE: return Units::ON_OFF;
                case INPUT1_EFFECT_2_AMOUNT: return Units::DB;
                case INPUT1_EQ_ENABLE: return Units::ON_OFF;
                case INPUT1_EQ_LOW_GAIN: return Units::DB;
                case INPUT1_EQ_MID_GAIN: return Units::DB;
                case INPUT1_EQ_HIGH_GAIN: return Units::DB;
                case INPUT1_COMPRESSOR_ENABLE: return Units::ON_OFF;
                case INPUT1_COMPRESSOR_AMOUNT: return Units::PERCENT;
                case INPUT1_EXT_FX_MUTE: return Units::ON_OFF;
                case INPUT1_EXT_FX_SENDS: return Units::DB;
                case INPUT2_GAIN: return Units::DB;
                case INPUT2_VOLUME: return Units::DB;
                case INPUT2_MUTE: return Units::ON_OFF;
                case INPUT2_CLIP_OL_PRE: return Units::ON_OFF;
                case INPUT2_CLIP_OL_POST: return Units::ON_OFF;
                case INPUT2_EFFECT_1_MUTE: return Units::ON_OFF;
                case INPUT2_EFFECT_1_AMOUNT: return Units::DB;
                case INPUT2_EFFECT_2_MUTE: return Units::ON_OFF;
                case INPUT2_EFFECT_2_AMOUNT: return Units::DB;
                case INPUT2_EQ_ENABLE: return Units::ON_OFF;
                case INPUT2_EQ_LOW_GAIN: return Units::DB;
                case INPUT2_EQ_MID_GAIN: return Units::DB;
                case INPUT2_EQ_HIGH_GAIN: return Units::DB;
                case INPUT2_COMPRESSOR_ENABLE: return Units::ON_OFF;
                case INPUT2_COMPRESSOR_AMOUNT: return Units::PERCENT;
                case INPUT2_EXT_FX_MUTE: return Units::ON_OFF;
                case INPUT2_EXT_FX_SENDS: return Units::DB;
                case INPUT3_GAIN: return Units::DB;
                case INPUT3_VOLUME: return Units::DB;
                case INPUT3_MUTE: return Units::ON_OFF;
                case INPUT3_CLIP_OL_PRE: return Units::ON_OFF;
                case INPUT3_CLIP_OL_POST: return Units::ON_OFF;
                case INPUT3_EFFECT_1_MUTE: return Units::ON_OFF;
                case INPUT3_EFFECT_1_AMOUNT: return Units::DB;
                case INPUT3_EFFECT_2_MUTE: return Units::ON_OFF;
                case INPUT3_EFFECT_2_AMOUNT: return Units::DB;
                case INPUT3_EQ_ENABLE: return Units::ON_OFF;
                case INPUT3_EQ_LOW_GAIN: return Units::DB;
                case INPUT3_EQ_MID_GAIN: return Units::DB;
                case INPUT3_EQ_HIGH_GAIN: return Units::DB;
                case INPUT3_COMPRESSOR_ENABLE: return Units::ON_OFF;
                case INPUT3_COMPRESSOR_AMOUNT: return Units::PERCENT;
                case INPUT3_EXT_FX_MUTE: return Units::ON_OFF;
                case INPUT3_EXT_FX_SENDS: return Units::DB;
                case INPUT4_GAIN: return Units::DB;
                case INPUT4_VOLUME: return Units::DB;
                case INPUT4_MUTE: return Units::ON_OFF;
                case INPUT4_CLIP_OL_PRE: return Units::ON_OFF;
                case INPUT4_CLIP_OL_POST: return Units::ON_OFF;
                case INPUT4_EFFECT_1_MUTE: return Units::ON_OFF;
                case INPUT4_EFFECT_1_AMOUNT: return Units::DB;
                case INPUT4_EFFECT_2_MUTE: return Units::ON_OFF;
                case INPUT4_EFFECT_2_AMOUNT: return Units::DB;
                case INPUT4_EQ_ENABLE: return Units::ON_OFF;
                case INPUT4_EQ_LOW_GAIN: return Units::DB;
                case INPUT4_EQ_MID_GAIN: return Units::DB;
                case INPUT4_EQ_HIGH_GAIN: return Units::DB;
                case INPUT4_COMPRESSOR_ENABLE: return Units::ON_OFF;
                case INPUT4_COMPRESSOR_AMOUNT: return Units::PERCENT;
                case INPUT4_EXT_FX_MUTE: return Units::ON_OFF;
                case INPUT4_EXT_FX_SENDS: return Units::DB;
                case STEREO_INPUT1_VOLUME: return Units::DB;
                case STEREO_INPUT1_MUTE: return Units::ON_OFF;
                case STEREO_INPUT1_CLIP_OL_PRE: return Units::ON_OFF;
                case STEREO_INPUT1_CLIP_OL_POST: return Units::ON_OFF;
                case STEREO_INPUT1_EQ_ENABLE: return Units::ON_OFF;
                case STEREO_INPUT1_EQ_LOW_GAIN: return Units::DB;
                case STEREO_INPUT1_EQ_MID_GAIN: return Units::DB;
                case STEREO_INPUT1_EQ_HIGH_GAIN: return Units::DB;
                case EFFECT11_TYPEID: return Units::STATE;
                case EFFECT12_TYPEID: return Units::STATE;
                case EFFECT13_TYPEID: return Units::STATE;
                case EFFECT14_TYPEID: return Units::STATE;
                case EFFECT21_TYPEID: return Units::STATE;
                case EFFECT22_TYPEID: return Units::STATE;
                case MAIN_HEADPHONE_GAIN: return Units::DB;
                case MAIN_MASTER_GAIN: return Units::DB;
                case MAIN_MUTE: return Units::ON_OFF;
                case MAIN_CLIP_OL: return Units::ON_OFF;
                case LOOPER_LEVEL: return Units::DB;
                case LOOPER_STATE: return Units::STATE;
                case FX_BYPASS: return Units::ON_OFF;
                default: return Units::UNKNOWN;
            }
        }
        #endif // SHOWBOX_DEBUG

        #ifdef SHOWBOX_DEBUG
        inline const char* getName(uint8_t entityId) {
            switch (entityId) {
                case FRONT_LED: return "FRONT_LED";
                case FEEDBACK_ELIM: return "FEEDBACK_ELIM";
                case AMP_PA_MODE: return "AMP_PA_MODE";
                case LOCATION_MODE: return "LOCATION_MODE";
                case SELECTED_CHAN: return "SELECTED_CHAN";
                case INPUT1_GAIN: return "INPUT1_GAIN";
                case INPUT1_VOLUME: return "INPUT1_VOLUME";
                case INPUT1_MUTE: return "INPUT1_MUTE";
                case INPUT1_CLIP_OL_PRE: return "INPUT1_CLIP_OL_PRE";
                case INPUT1_CLIP_OL_POST: return "INPUT1_CLIP_OL_POST";
                case INPUT1_EFFECT_1_MUTE: return "INPUT1_EFFECT_1_MUTE";
                case INPUT1_EFFECT_1_AMOUNT: return "INPUT1_EFFECT_1_AMOUNT";
                case INPUT1_EFFECT_2_MUTE: return "INPUT1_EFFECT_2_MUTE";
                case INPUT1_EFFECT_2_AMOUNT: return "INPUT1_EFFECT_2_AMOUNT";
                case INPUT1_EQ_ENABLE: return "INPUT1_EQ_ENABLE";
                case INPUT1_EQ_LOW_GAIN: return "INPUT1_EQ_LOW_GAIN";
                case INPUT1_EQ_MID_GAIN: return "INPUT1_EQ_MID_GAIN";
                case INPUT1_EQ_HIGH_GAIN: return "INPUT1_EQ_HIGH_GAIN";
                case INPUT1_COMPRESSOR_ENABLE: return "INPUT1_COMPRESSOR_ENABLE";
                case INPUT1_COMPRESSOR_AMOUNT: return "INPUT1_COMPRESSOR_AMOUNT";
                case INPUT1_EXT_FX_MUTE: return "INPUT1_EXT_FX_MUTE";
                case INPUT1_EXT_FX_SENDS: return "INPUT1_EXT_FX_SENDS";
                case INPUT2_GAIN: return "INPUT2_GAIN";
                case INPUT2_VOLUME: return "INPUT2_VOLUME";
                case INPUT2_MUTE: return "INPUT2_MUTE";
                case INPUT2_CLIP_OL_PRE: return "INPUT2_CLIP_OL_PRE";
                case INPUT2_CLIP_OL_POST: return "INPUT2_CLIP_OL_POST";
                case INPUT2_EFFECT_1_MUTE: return "INPUT2_EFFECT_1_MUTE";
                case INPUT2_EFFECT_1_AMOUNT: return "INPUT2_EFFECT_1_AMOUNT";
                case INPUT2_EFFECT_2_MUTE: return "INPUT2_EFFECT_2_MUTE";
                case INPUT2_EFFECT_2_AMOUNT: return "INPUT2_EFFECT_2_AMOUNT";
                case INPUT2_EQ_ENABLE: return "INPUT2_EQ_ENABLE";
                case INPUT2_EQ_LOW_GAIN: return "INPUT2_EQ_LOW_GAIN";
                case INPUT2_EQ_MID_GAIN: return "INPUT2_EQ_MID_GAIN";
                case INPUT2_EQ_HIGH_GAIN: return "INPUT2_EQ_HIGH_GAIN";
                case INPUT2_COMPRESSOR_ENABLE: return "INPUT2_COMPRESSOR_ENABLE";
                case INPUT2_COMPRESSOR_AMOUNT: return "INPUT2_COMPRESSOR_AMOUNT";
                case INPUT2_EXT_FX_MUTE: return "INPUT2_EXT_FX_MUTE";
                case INPUT2_EXT_FX_SENDS: return "INPUT2_EXT_FX_SENDS";
                case INPUT3_GAIN: return "INPUT3_GAIN";
                case INPUT3_VOLUME: return "INPUT3_VOLUME";
                case INPUT3_MUTE: return "INPUT3_MUTE";
                case INPUT3_CLIP_OL_PRE: return "INPUT3_CLIP_OL_PRE";
                case INPUT3_CLIP_OL_POST: return "INPUT3_CLIP_OL_POST";
                case INPUT3_EFFECT_1_MUTE: return "INPUT3_EFFECT_1_MUTE";
                case INPUT3_EFFECT_1_AMOUNT: return "INPUT3_EFFECT_1_AMOUNT";
                case INPUT3_EFFECT_2_MUTE: return "INPUT3_EFFECT_2_MUTE";
                case INPUT3_EFFECT_2_AMOUNT: return "INPUT3_EFFECT_2_AMOUNT";
                case INPUT3_EQ_ENABLE: return "INPUT3_EQ_ENABLE";
                case INPUT3_EQ_LOW_GAIN: return "INPUT3_EQ_LOW_GAIN";
                case INPUT3_EQ_MID_GAIN: return "INPUT3_EQ_MID_GAIN";
                case INPUT3_EQ_HIGH_GAIN: return "INPUT3_EQ_HIGH_GAIN";
                case INPUT3_COMPRESSOR_ENABLE: return "INPUT3_COMPRESSOR_ENABLE";
                case INPUT3_COMPRESSOR_AMOUNT: return "INPUT3_COMPRESSOR_AMOUNT";
                case INPUT3_EXT_FX_MUTE: return "INPUT3_EXT_FX_MUTE";
                case INPUT3_EXT_FX_SENDS: return "INPUT3_EXT_FX_SENDS";
                case INPUT4_GAIN: return "INPUT4_GAIN";
                case INPUT4_VOLUME: return "INPUT4_VOLUME";
                case INPUT4_MUTE: return "INPUT4_MUTE";
                case INPUT4_CLIP_OL_PRE: return "INPUT4_CLIP_OL_PRE";
                case INPUT4_CLIP_OL_POST: return "INPUT4_CLIP_OL_POST";
                case INPUT4_EFFECT_1_MUTE: return "INPUT4_EFFECT_1_MUTE";
                case INPUT4_EFFECT_1_AMOUNT: return "INPUT4_EFFECT_1_AMOUNT";
                case INPUT4_EFFECT_2_MUTE: return "INPUT4_EFFECT_2_MUTE";
                case INPUT4_EFFECT_2_AMOUNT: return "INPUT4_EFFECT_2_AMOUNT";
                case INPUT4_EQ_ENABLE: return "INPUT4_EQ_ENABLE";
                case INPUT4_EQ_LOW_GAIN: return "INPUT4_EQ_LOW_GAIN";
                case INPUT4_EQ_MID_GAIN: return "INPUT4_EQ_MID_GAIN";
                case INPUT4_EQ_HIGH_GAIN: return "INPUT4_EQ_HIGH_GAIN";
                case INPUT4_COMPRESSOR_ENABLE: return "INPUT4_COMPRESSOR_ENABLE";
                case INPUT4_COMPRESSOR_AMOUNT: return "INPUT4_COMPRESSOR_AMOUNT";
                case INPUT4_EXT_FX_MUTE: return "INPUT4_EXT_FX_MUTE";
                case INPUT4_EXT_FX_SENDS: return "INPUT4_EXT_FX_SENDS";
                case STEREO_INPUT1_VOLUME: return "STEREO_INPUT1_VOLUME";
                case STEREO_INPUT1_MUTE: return "STEREO_INPUT1_MUTE";
                case STEREO_INPUT1_CLIP_OL_PRE: return "STEREO_INPUT1_CLIP_OL_PRE";
                case STEREO_INPUT1_CLIP_OL_POST: return "STEREO_INPUT1_CLIP_OL_POST";
                case STEREO_INPUT1_EQ_ENABLE: return "STEREO_INPUT1_EQ_ENABLE";
                case STEREO_INPUT1_EQ_LOW_GAIN: return "STEREO_INPUT1_EQ_LOW_GAIN";
                case STEREO_INPUT1_EQ_MID_GAIN: return "STEREO_INPUT1_EQ_MID_GAIN";
                case STEREO_INPUT1_EQ_HIGH_GAIN: return "STEREO_INPUT1_EQ_HIGH_GAIN";
                case EFFECT11_TYPEID: return "EFFECT11_TYPEID";
                case EFFECT12_TYPEID: return "EFFECT12_TYPEID";
                case EFFECT13_TYPEID: return "EFFECT13_TYPEID";
                case EFFECT14_TYPEID: return "EFFECT14_TYPEID";
                case EFFECT21_TYPEID: return "EFFECT21_TYPEID";
                case EFFECT22_TYPEID: return "EFFECT22_TYPEID";
                case MAIN_HEADPHONE_GAIN: return "MAIN_HEADPHONE_GAIN";
                case MAIN_MASTER_GAIN: return "MAIN_MASTER_GAIN";
                case MAIN_MUTE: return "MAIN_MUTE";
                case MAIN_CLIP_OL: return "MAIN_CLIP_OL";
                case LOOPER_LEVEL: return "LOOPER_LEVEL";
                case LOOPER_STATE: return "LOOPER_STATE";
                case FX_BYPASS: return "FX_BYPASS";
                default: return "UNKNOWN";
            }
        }
        #endif // SHOWBOX_DEBUG
    } // namespace Entity

} // namespace Showbox

#endif // ENTITY_H