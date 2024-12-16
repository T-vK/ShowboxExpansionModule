// // constants.h
// #ifndef XTOUCH_COMPACT_ADAPTER_CONSTANTS_H
// #define XTOUCH_COMPACT_ADAPTER_CONSTANTS_H

// #include <unordered_map>
// #include "constants.h"

// namespace XtouchCompactAdapter {
//     // enum with simple button names indicating their position likeBUTTON_ROW_0_CHANNEL_0,BUTTON_ROW_0_CHANNEL_1,BUTTON_ROW_0_CHANNEL_2:
//     enum channel_0_note : uint8_t {
//         ENCODER_BUTTON_CHAHNEL_0 = 0,
//         ENCODER_BUTTON_CHANNEL_1 = 1,
//         ENCODER_BUTTON_CHANNEL_2 = 2,
//         ENCODER_BUTTON_CHANNEL_3 = 3,
//         ENCODER_BUTTON_CHANNEL_4 = 4,
//         ENCODER_BUTTON_CHANNEL_5 = 5,
//         ENCODER_BUTTON_CHANNEL_6 = 6,
//         ENCODER_BUTTON_CHANNEL_7 = 7,
//         ENCODER_BUTTON_SIDE_ROW_0_COL_0 = 8,
//         ENCODER_BUTTON_SIDE_ROW_0_COL_1 = 9,
//         ENCODER_BUTTON_SIDE_ROW_1_COL_0 = 10,
//         ENCODER_BUTTON_SIDE_ROW_1_COL_1 = 11,
//         ENCODER_BUTTON_SIDE_ROW_2_COL_0 = 12,
//         ENCODER_BUTTON_SIDE_ROW_2_COL_1 = 13,
//         ENCODER_BUTTON_SIDE_ROW_3_COL_0 = 14,
//         ENCODER_BUTTON_SIDE_ROW_3_COL_1 = 15,
//         BUTTON_ROW_0_CHANNEL_0 = 16,
//         BUTTON_ROW_0_CHANNEL_1 = 17,
//         BUTTON_ROW_0_CHANNEL_2 = 18,
//         BUTTON_ROW_0_CHANNEL_3 = 19,
//         BUTTON_ROW_0_CHANNEL_4 = 20,
//         BUTTON_ROW_0_CHANNEL_5 = 21,
//         BUTTON_ROW_0_CHANNEL_6 = 22,
//         BUTTON_ROW_0_CHANNEL_7 = 23,
//         BUTTON_ROW_1_CHANNEL_0 = 24,
//         BUTTON_ROW_1_CHANNEL_1 = 25,
//         BUTTON_ROW_1_CHANNEL_2 = 26,
//         BUTTON_ROW_1_CHANNEL_3 = 27,
//         BUTTON_ROW_1_CHANNEL_4 = 28,
//         BUTTON_ROW_1_CHANNEL_5 = 29,
//         BUTTON_ROW_1_CHANNEL_6 = 30,
//         BUTTON_ROW_1_CHANNEL_7 = 31,
//         BUTTON_ROW_2_CHANNEL_0 = 32,
//         BUTTON_ROW_2_CHANNEL_1 = 33,
//         BUTTON_ROW_2_CHANNEL_2 = 34,
//         BUTTON_ROW_2_CHANNEL_3 = 35,
//         BUTTON_ROW_2_CHANNEL_4 = 36,
//         BUTTON_ROW_2_CHANNEL_5 = 37,
//         BUTTON_ROW_2_CHANNEL_6 = 38,
//         BUTTON_ROW_2_CHANNEL_7 = 39,
//         BUTTON_ROW_3_CHANNEL_0 = 40,
//         BUTTON_ROW_3_CHANNEL_1 = 41,
//         BUTTON_ROW_3_CHANNEL_2 = 42,
//         BUTTON_ROW_3_CHANNEL_3 = 43,
//         BUTTON_ROW_3_CHANNEL_4 = 44,
//         BUTTON_ROW_3_CHANNEL_5 = 45,
//         BUTTON_ROW_3_CHANNEL_6 = 46,
//         BUTTON_ROW_3_CHANNEL_7 = 47,
//         MAIN_BUTTON = 48,
//         REWIND_BUTTON = 49,
//         FAST_FORWARD_BUTTON = 50,
//         LOOP_BUTTON = 51,
//         RECORD_BUTTON = 52,
//         STOP_BUTTON = 53,
//         PLAY_BUTTON = 54,
//         };
//     };

//     enum channel_0_cc : uint8_t {
//         FADER_TOUCH_CHANNEL_0 = 1,
//         FADER_TOUCH_CHANNEL_1 = 2,
//         FADER_TOUCH_CHANNEL_2 = 3,
//         FADER_TOUCH_CHANNEL_3 = 4,
//         FADER_TOUCH_CHANNEL_4 = 5,
//         FADER_TOUCH_CHANNEL_5 = 6,
//         FADER_TOUCH_CHANNEL_6 = 7,
//         FADER_TOUCH_CHANNEL_7 = 8,
//         FADER_TOUCH_MASTER = 9,
//         ENCODER_ROTAION_CHANNEL_0 = 10,
//         ENCODER_ROTAION_CHANNEL_1 = 11,
//         ENCODER_ROTAION_CHANNEL_2 = 12,
//         ENCODER_ROTAION_CHANNEL_3 = 13,
//         ENCODER_ROTAION_CHANNEL_4 = 14,
//         ENCODER_ROTAION_CHANNEL_5 = 15,
//         ENCODER_ROTAION_CHANNEL_6 = 16,
//         ENCODER_ROTAION_CHANNEL_7 = 17,
//         ENCODER_ROTAION_SIDE_ROW_0_COL_0 = 18,
//         ENCODER_ROTAION_SIDE_ROW_0_COL_1 = 19,
//         ENCODER_ROTAION_SIDE_ROW_1_COL_0 = 20,
//         ENCODER_ROTAION_SIDE_ROW_1_COL_1 = 21,
//         ENCODER_ROTAION_SIDE_ROW_2_COL_0 = 22,
//         ENCODER_ROTAION_SIDE_ROW_2_COL_1 = 23,
//         ENCODER_ROTAION_SIDE_ROW_3_COL_0 = 24,
//         ENCODER_ROTAION_SIDE_ROW_3_COL_1 = 25,
//         FADER_MOVE_CHANNEL_0 = 101,
//         FADER_MOVE_CHANNEL_1 = 102,
//         FADER_MOVE_CHANNEL_2 = 103,
//         FADER_MOVE_CHANNEL_3 = 104,
//         FADER_MOVE_CHANNEL_4 = 105,
//         FADER_MOVE_CHANNEL_5 = 106,
//         FADER_MOVE_CHANNEL_6 = 107,
//         FADER_MOVE_CHANNEL_7 = 108,
//         FADER_MOVE_MASTER = 109,
//     };

//     enum channel_1_note : uint8_t {
//         BUTTON_LED_ROW_0_CHANNEL_0 = 0,
//         BUTTON_LED_ROW_0_CHANNEL_1 = 1,
//         BUTTON_LED_ROW_0_CHANNEL_2 = 2,
//         BUTTON_LED_ROW_0_CHANNEL_3 = 3,
//         BUTTON_LED_ROW_0_CHANNEL_4 = 4,
//         BUTTON_LED_ROW_0_CHANNEL_5 = 5,
//         BUTTON_LED_ROW_0_CHANNEL_6 = 6,
//         BUTTON_LED_ROW_0_CHANNEL_7 = 7,
//         BUTTON_LED_ROW_1_CHANNEL_0 = 8,
//         BUTTON_LED_ROW_1_CHANNEL_1 = 9,
//         BUTTON_LED_ROW_1_CHANNEL_2 = 10,
//         BUTTON_LED_ROW_1_CHANNEL_3 = 11,
//         BUTTON_LED_ROW_1_CHANNEL_4 = 12,
//         BUTTON_LED_ROW_1_CHANNEL_5 = 13,
//         BUTTON_LED_ROW_1_CHANNEL_6 = 14,
//         BUTTON_LED_ROW_1_CHANNEL_7 = 15,
//         BUTTON_LED_ROW_2_CHANNEL_0 = 16,
//         BUTTON_LED_ROW_2_CHANNEL_1 = 17,
//         BUTTON_LED_ROW_2_CHANNEL_2 = 18,
//         BUTTON_LED_ROW_2_CHANNEL_3 = 19,
//         BUTTON_LED_ROW_2_CHANNEL_4 = 20,
//         BUTTON_LED_ROW_2_CHANNEL_5 = 21,
//         BUTTON_LED_ROW_2_CHANNEL_6 = 22,
//         BUTTON_LED_ROW_2_CHANNEL_7 = 23,
//         BUTTON_LED_ROW_3_CHANNEL_0 = 24,
//         BUTTON_LED_ROW_3_CHANNEL_1 = 25,
//         BUTTON_LED_ROW_3_CHANNEL_2 = 26,
//         BUTTON_LED_ROW_3_CHANNEL_3 = 27,
//         BUTTON_LED_ROW_3_CHANNEL_4 = 28,
//         BUTTON_LED_ROW_3_CHANNEL_5 = 29,
//         BUTTON_LED_ROW_3_CHANNEL_6 = 30,
//         BUTTON_LED_ROW_3_CHANNEL_7 = 31,
//         MAIN_BUTTON_LED = 32,
//         REWIND_BUTTON_LED = 33,
//         FAST_FORWARD_BUTTON_LED = 34,
//         LOOP_BUTTON_LED = 35,
//         RECORD_BUTTON_LED = 36,
//         STOP_BUTTON_LED = 37,
//         PLAY_BUTTON_LED = 38,
//     };

//     enum channel_1_cc : uint8_t {
//         FADER_MOVE_CHANNEL_0 = 1,
//         FADER_MOVE_CHANNEL_1 = 2,
//         FADER_MOVE_CHANNEL_2 = 3,
//         FADER_MOVE_CHANNEL_3 = 4,
//         FADER_MOVE_CHANNEL_4 = 5,
//         FADER_MOVE_CHANNEL_5 = 6,
//         FADER_MOVE_CHANNEL_6 = 7,
//         FADER_MOVE_CHANNEL_7 = 8,
//         FADER_MOVE_MASTER = 9,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_0 = 26,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_1 = 27,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_2 = 28,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_3 = 29,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_4 = 30,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_5 = 31,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_6 = 32,
//         LED_RING_BEHAVIOR_ENCODER_CHANNEL_7 = 33,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_0_COL_0 = 34,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_0_COL_1 = 35,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_1_COL_0 = 36,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_1_COL_1 = 37,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_2_COL_0 = 38,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_2_COL_1 = 39,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_3_COL_0 = 40,
//         LED_RING_BEHAVIOR_ENCODER_SIDE_ROW_3_COL_1 = 41,
//         FOOT_SWITCH_LED = 42,
//         EXP_PEDAL_LED = 43,
//         OPERATION_MODE_SELECT = 127
//     };

//     enum operation_mode : uint8_t {
//         STANDARD = 0,
//         MC = 1
//     };

//     enum preset_layer_select : uint8_t {
//         A = 0,
//         B = 1
//     };

//     enum led_ring_behavior : uint8_t {
//         SINGLE = 0,
//         PAN = 1,
//         FAN = 2,
//         SPREAD = 3,
//         TRIM = 4
//     };

//     enum led_ring_value : uint8_t {
//         ALL_LEDS_OFF = 0,
//         LED_1_ON = 1,
//         LED_2_ON = 2,
//         LED_3_ON = 3,
//         LED_4_ON = 4,
//         LED_5_ON = 5,
//         LED_6_ON = 6,
//         LED_7_ON = 7,
//         LED_8_ON = 8,
//         LED_9_ON = 9,
//         LED_10_ON = 10,
//         LED_11_ON = 11,
//         LED_12_ON = 12,
//         LED_13_ON = 13,
//         LED_1_BLINK = 14,
//         LED_2_BLINK = 15,
//         LED_3_BLINK = 16,
//         LED_4_BLINK = 17,
//         LED_5_BLINK = 18,
//         LED_6_BLINK = 19,
//         LED_7_BLINK = 20,
//         LED_8_BLINK = 21,
//         LED_9_BLINK = 22,
//         LED_10_BLINK = 23,
//         LED_11_BLINK = 24,
//         LED_12_BLINK = 25,
//         LED_13_BLINK = 26,
//         ALL_LEDS_ON = 27,
//         ALL_LEDS_BLINK = 28
//     };

//     enum button_led_value : uint8_t {
//         BUTTON_LED_OFF = 0,
//         BUTTON_LED_ON = 1,
//         BUTTON_LED_BLINK = 2
//     };

//     // For foot switch, 0-13 = LED off, 64-127 = LED on:
//     enum foot_switch_led : uint8_t {
//         LED_OFF = 0,
//         LED_ON = 127
//     };

//     // For Expression Pedal: LED only on while data transfer (value change)

//     // X-Touch Compact MIDI note mappings

//     const std::unordered_map<int, int> midiNoteToEntityId = { // -1 = not mapped | -2 = Special case/no entity
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_0, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_1, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_2, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_3, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_4, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_5, -2}, // Lock FX 1
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_6, -2}, // Lock FX 2
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_0_CHANNEL_7, -2}, // Lock Snapshots
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_0, -2}, // Tuner Channel 0
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_1, -2}, // Tuner Channel 1
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_2, -2}, // Tuner Channel 2
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_3, -2}, // Tuner Channel 3
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_4, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_5, FRONT_LED},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_6, AMP_PA_MODE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_7, -2}, // Load Snapshot
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_0, -2}, // Select Channel 0
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_1, -2}, // Select Channel 1
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_2, -2}, // Select Channel 2
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_3, -2}, // Select Channel 3
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_4, -2}, // Select Channel 4
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_5, FEEDBACK_ELIM},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_6, LOCATION_MODE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_7, -2}, // Save Snapshot
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_0, INPUT1_MUTE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_1, INPUT2_MUTE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_2, INPUT3_MUTE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_3, INPUT4_MUTE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_4, STEREO_INPUT1_MUTE},
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_5, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_6, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::BUTTON_ROW_3_CHANNEL_7, -1}, // Unused
//         {XtouchCompactAdapter::channel_0_note::MAIN_BUTTON, MAIN_MUTE},
//         {XtouchCompactAdapter::channel_0_note::ENCODER_BUTTON_SIDE_ROW_0_COL_1, INPUT1_EFFECT_1_MUTE}
//         {XtouchCompactAdapter::channel_0_note::ENCODER_BUTTON_SIDE_ROW_1_COL_1, INPUT2_EFFECT_1_MUTE}

        
//     };

//     const std::unordered_map<int, int> entityIdToMidiNote = {
//         {FRONT_LED, XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_5},
//         {FEEDBACK_ELIM, XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_5},
//         {AMP_PA_MODE, XtouchCompactAdapter::channel_0_note::BUTTON_ROW_1_CHANNEL_6},
//         {LOCATION_MODE, XtouchCompactAdapter::channel_0_note::BUTTON_ROW_2_CHANNEL_6},
//     };


// #endif // XTOUCH_COMPACT_ADAPTER_CONSTANTS_H