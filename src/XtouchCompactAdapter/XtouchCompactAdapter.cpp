#include "XtouchCompactAdapter.h"

XtouchCompactAdapter* XtouchCompactAdapter::instance = nullptr;

// Constructor
XtouchCompactAdapter::XtouchCompactAdapter(MackieShowbox* showbox)
    : showbox(showbox) {
        instance = this;
    }

// Initialization
void XtouchCompactAdapter::begin() {
    midi->action.setCallbackOnControlChange(XtouchCompactAdapter::onControlChange);
    midi->action.setCallbackOnNoteOn(XtouchCompactAdapter::onNoteOn);
    midi->action.setCallbackOnNoteOff(XtouchCompactAdapter::onNoteOff);
    midi->action.setCallbackOnPitchBend(XtouchCompactAdapter::onPitchBend);

    /*
    X-TOUCH COMPACT RX MIDI DATA
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | Function                     | RX CH      | RX Command                  | RX Value                                                                                  |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **Operation Mode Select**    | GLOBAL CH  | CC 127                      | 0 = Standard Mode, MC Mode LED off                                                        |
    |                              |            |                             | 1 = MC Mode, MC Mode LED on                                                               |
    |                              |            |                             | 2–127 = ignored                                                                           |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **Preset Layer Change**      | GLOBAL CH  | Program Change              | ONLY in Standard Mode (see above):                                                        |
    |                              |            |                             | 0 = Layer A, A-button LED on                                                              |
    |                              |            |                             | 1 = Layer B, B-button LED on                                                              |
    |                              |            |                             | 2–127 = ignored                                                                           |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **Fader Movement**           | GLOBAL CH  | CC 1 – CC 9                 | 0–127 = fader position, bottom to top                                                     |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **LED Ring Behavior Change** | GLOBAL CH  | Encoders 1–8: CC 10 – CC 17 | 0 = Single                                                                                |
    |                              |            | Encoders 9–16: CC 18 – CC 25| 1 = Pan                                                                                   |
    |                              |            |                             | 2 = Fan                                                                                   |
    |                              |            |                             | 3 = Spread                                                                                |
    |                              |            |                             | 4 = Trim                                                                                  |
    |                              |            |                             | 5–127 = ignored                                                                           |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **LED Ring Remote Control**  | GLOBAL CH  | Encoders 1–8: CC 26 – CC 33 | 0 = all LEDs off                                                                          |
    |                              |            | Encoders 9–16: CC 34 – CC 41| 1–13 = LEDs 1 (left) – 13 (right) on                                                      |
    |                              |            |                             | 14–26 = LEDs 1 (left) – 13 (right) blinking                                               |
    |                              |            |                             | 27 = all LEDs on                                                                          |
    |                              |            |                             | 28 = all LEDs blinking                                                                    |
    |                              |            |                             | 29–127 = ignored                                                                          |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **Buttons LED Remote Control**| GLOBAL CH | Upper Top Row 1–8:          | Note 0 – Note 7                                                                           |
    |                              |            | Upper Mid Row 9–16:         | Note 8 – Note 15                                                                          |
    |                              |            | Upper Bottom Row 17–24:     | Note 16 – Note 23                                                                         |
    |                              |            | Lower Row 25–33:            | Note 24 – Note 32                                                                         |
    |                              |            | Right Area 34–39:           | Note 33 – Note 38                                                                         |
    |                              |            | Layer A, B LEDs:            | Not assignable; only 1 of 2 LEDs lights if Standard mode selected (see above "Preset Layer Change")|
    |                              |            |                             | Note on/off with Velocity 0: Button LED off                                               |
    |                              |            |                             | Note on with Velocity 1: Button LED on                                                    |
    |                              |            |                             | Note on with Velocity 2: Button LED blinking                                              |
    |                              |            |                             | Note on with Velocity 3–127: ignored                                                      |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    | **Status LEDs Remote Control**| GLOBAL CH | Foot Switch LED: CC 42      | 0–63 = LED off                                                                            |
    |                              |            | Exp. Pedal LED: CC 43       | 64–127 = LED on                                                                           |
    |                              |            | MC Mode LED:                | Not assignable; lights only if MC mode selected (see above "Operation Mode Select")       |
    |                              |            | USB LED:                    | Not assignable; always active if valid connection to computer available                   |
    |                              |            | MIDI I/O LEDs:              | Not assignable; always active while data transfer                                         |
    |------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
    */

    // Set standard operation mode to 0
    setOperationMode(0);

    // Set preset layer to A
    setPresetLayer(0);

    // Set encoder behavior
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t value = (i == 8 || i == 10 || i == 12) ? 4 : 2;
        setLedRingBehavior(i, value);
    }

    // Turn off button leds
    for (uint8_t i = 0; i < 39; i++) {
        setButton(i, false);
    }

    // Set unused Encoders/Faders to 0
    setEncoder(5, 0);
    setEncoder(15, 0);
    setFader(5, 0);
    setFader(6, 0);
    setFader(7, 0);

    // Set snapshot slecetion encoder to 0
    setEncoder(7, 0);

    // // Turn off encoder leds
    // for (uint8_t i = 0; i < 16; i++) {
    //     setEncoder(i, 0);
    // }

    // // Move all faders to the bottom
    // for (uint8_t i = 0; i < 9; i++) {
    //     setFader(i, 0);
    // }

    showbox->postHandlePacketCallback = [](uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction, UARTInterceptor::PacketHandlerResult result) {
        packet_type packetType = static_cast<packet_type>(raw_packet[3]);
        if (packetType == ENTITY) {
            entity_id entityId = static_cast<entity_id>(raw_packet[5]);
            onShowboxEntityChange(entityId);
        } else if (packetType == ALL_ENTITIES) {
            for (uint8_t i = 0; i <= FX_BYPASS; i++) {
                entity_id entityId = static_cast<entity_id>(i);
                onShowboxEntityChange(entityId);
            }            
        } else if (packetType == SD_CARD_EVENT) {
            sd_card_state sdCardState = static_cast<sd_card_state>(raw_packet[5]);
            if (sdCardState == sd_card_state::NOT_DETECTED) {
                instance->setRecordButton(false);
            } else if (sdCardState == sd_card_state::DETECTED) {
                instance->setRecordButton(true);
            } else  if(sdCardState == sd_card_state::RECORDING) {
                instance->setRecordButton(2); // blinking
            }
        } else if (packetType == TUNER_TOGGLE) { // BE EF 01 18 01 00 EF BE - Head Head BodyLength TunerToggle TurnOn TunerChan Checksum
            bool tunerState = static_cast<bool>(raw_packet[4]);
            uint8_t tunerInput = static_cast<bool>(raw_packet[5]);
            instance->setChannelButton(1, tunerInput, tunerState);
        }
    };

    beginTime = millis();
}

void XtouchCompactAdapter::configureXtouchCompact() {
//     // Set standard operation mode to 0
//     setOperationMode(0);

//     // Set preset layer to A
//     setPresetLayer(0);

   // Set encoder behavior
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t value = (i == 8 || i == 10 || i == 12) ? 4 : 2;
        setLedRingBehavior(i, value);
    }

//     // Turn off button leds
//     for (uint8_t i = 0; i < 39; i++) {
//         setButtonLed(i, false);
//     }

    // Turn off encoder leds
    // for (uint8_t i = 0; i < 16; i++) {
    //     setLedRing(i, 0);
    // }

    // Move all faders to the bottom
    // for (uint8_t i = 0; i < 9; i++) {
    //     setFader(i, 0);
    // }
}

void XtouchCompactAdapter::onShowboxEntityChange(entity_id entityId) {
    if (entityId < INPUT1_GAIN || entityId > STEREO_INPUT1_EQ_HIGH_GAIN) {
        if (entityId == FRONT_LED) {
            bool state = instance->showbox->getFrontLed();
            instance->setChannelButton(1, 5, state);
        } else if (entityId == FEEDBACK_ELIM) {
            uint8_t state = instance->showbox->getFeedbackElim();
            instance->setChannelButton(2, 5, state);
        } else if (entityId == AMP_PA_MODE) {
            bool state = instance->showbox->getAmpPaMode();
            instance->setChannelButton(1, 6, !state);
        } else if (entityId == LOCATION_MODE) {
            bool state = instance->showbox->getLocationMode();
            instance->setChannelButton(2, 6, !state);
        } else if (entityId == SELECTED_CHAN) {
            // Will not be mapped to keep them independent
        } else if (entityId == MAIN_HEADPHONE_GAIN) {
            // float gain = instance->showbox->getMainHeadphoneGain(); // main headphone gain goes from -120.0 to 10.0
            // float gainPercent = (gain + 120.0) / 130.0;
            // instance->setChannelEncoderPercent(4, gainPercent);
            // Not mapped 
        } else if (entityId == MAIN_MASTER_GAIN) {
            float gain = instance->showbox->getMainMasterGain(); // main master gain goes from -120.0 to 6.0
            float gainPercent = (gain + 120.0) / 126.0;
            float gainPercentLinear = instance->mapNonlinearToLinear(gainPercent);
            instance->setFaderPercent(8, gainPercentLinear);
        } else if (entityId == MAIN_MUTE) {
            bool mute = instance->showbox->getMainMute();
            instance->setMainButton(mute);
        } else if (entityId == LOOPER_LEVEL) {
            // uint8_t level = instance->showbox->getLooperLevel(); // looper level goes from -120.0 - 10.0
            // float levelPercent = (level + 120.0) / 130.0;
            // instance->setChannelEncoderPercent(9, levelPercent);
            // Not mapped
        } else if (entityId == LOOPER_STATE) {
            looper_state state = instance->showbox->getLooperState();
            if (state == RECORD_INITIAL_LOOP) {
                instance->setLoopButton(true);
                instance->setPlayButton(false);
                instance->setStopButton(false);
            } else if (state == PLAY) {
                instance->setLoopButton(false);
                instance->setPlayButton(true);
                instance->setStopButton(false);
            } else if (state == STOP_PLAYING) {
                instance->setLoopButton(false);
                instance->setPlayButton(false);
                instance->setStopButton(true);
            } else if (state == RECORD_OVERDUB) {
                instance->setLoopButton(true);
                instance->setPlayButton(true);
                instance->setStopButton(false);
            } else if (state == DELETE) {
                instance->setLoopButton(false);
                instance->setPlayButton(false);
                instance->setStopButton(false);
            }
        } else if (entityId == FX_BYPASS) {
            bool state = instance->showbox->getFxBypassState();
            if (state) {
                instance->setLedRing(13, 28);
            } else {
                float value = instance->showbox->getInputExtFxSends(0); // input effect amount goes from 0.0 to 1.0
                instance->setEncoderPercent(13, value);
            }
        } else if (entityId == EFFECT11_TYPEID) {
            if (instance->selectedChannel == 0) {
                uint8_t type = instance->showbox->getInputEffectType(0, EFFECT1); // FX1 goes from 0 to 14
                float typePercent = type / 14.0;
                instance->setEncoderPercent(5, typePercent);
            }
        } else if (entityId == EFFECT12_TYPEID) {
            if (instance->selectedChannel == 1) {
                uint8_t type = instance->showbox->getInputEffectType(1, EFFECT1); // FX1 goes from 0 to 14
                float typePercent = type / 14.0;
                instance->setEncoderPercent(5, typePercent);
            }
        } else if (entityId == EFFECT13_TYPEID) {
            if (instance->selectedChannel == 2) {
                uint8_t type = instance->showbox->getInputEffectType(2, EFFECT1); // FX1 goes from 0 to 14
                float typePercent = type / 14.0;
                instance->setEncoderPercent(5, typePercent);
            }
        } else if (entityId == EFFECT14_TYPEID) {
            if (instance->selectedChannel == 3) {
                uint8_t type = instance->showbox->getInputEffectType(3, EFFECT1); // FX1 goes from 0 to 14
                float typePercent = type / 14.0;
                instance->setEncoderPercent(5, typePercent);
            }
        } else if (entityId == EFFECT21_TYPEID) {
            if (instance->selectedChannel == 0 || instance->selectedChannel == 2) {
                uint8_t type = instance->showbox->getInputEffectType(0, EFFECT2); // FX2 goes from 0 to 17
                float typePercent = type / 17.0;
                instance->setEncoderPercent(6, typePercent);
            }
        } else if (entityId == EFFECT22_TYPEID) {
            if (instance->selectedChannel == 1 || instance->selectedChannel == 3) {
                uint8_t type = instance->showbox->getInputEffectType(1, EFFECT2); // FX2 goes from 0 to 17
                float typePercent = type / 17.0;
                instance->setEncoderPercent(6, typePercent);
            }
        }
        return;
    }

    uint8_t input;
    if (entityId >= INPUT1_GAIN && entityId < INPUT2_GAIN) {
        input = 0;
    } else if (entityId >= INPUT2_GAIN && entityId < INPUT3_GAIN) {
        input = 1;
    } else if (entityId >= INPUT3_GAIN && entityId < INPUT4_GAIN) {
        input = 2;
    } else if (entityId >= INPUT4_GAIN && entityId < STEREO_INPUT1_VOLUME) {
        input = 3;
    } else if (entityId >= STEREO_INPUT1_VOLUME && entityId < EFFECT11_TYPEID) {
        input = 4;
    }
    
    if (entityId == INPUT1_GAIN || entityId == INPUT2_GAIN || entityId == INPUT3_GAIN || entityId == INPUT4_GAIN) {
        float gain = instance->showbox->getInputGain(input); // input gain goes from 0.0 to 60.0
        float gainPercent = gain / 60.0;
        instance->Debug->printf("Gain: %f\n", gainPercent);
        instance->setChannelEncoderPercent(input, gainPercent);
    } else if (entityId == INPUT1_VOLUME || entityId == INPUT2_VOLUME || entityId == INPUT3_VOLUME || entityId == INPUT4_VOLUME || entityId == STEREO_INPUT1_VOLUME) {
        float volume = instance->showbox->getInputVolume(input); // input volume goes from -120.0 to 10.0 (20.0 for STERO_INPUT1_VOLUME)
        float volumePercent = (volume + 120.0) / 130.0;
        if (entityId == STEREO_INPUT1_VOLUME) { // -120.0 to 20.0
            volumePercent = (volume + 120.0) / 140.0;
        }
        float volumePercentLinear = instance->mapNonlinearToLinear(volumePercent);
        instance->Debug->printf("Volume: %f\n", volumePercentLinear);
        instance->setFaderPercent(input, volumePercentLinear);
    } else if (entityId == INPUT1_MUTE || entityId == INPUT2_MUTE || entityId == INPUT3_MUTE || entityId == INPUT4_MUTE || entityId == STEREO_INPUT1_MUTE) {
        bool mute = instance->showbox->getInputMute(input);
        uint8_t muteRow = 3;
        instance->setChannelButton(muteRow, input, mute);
    } else if (entityId == INPUT1_CLIP_OL_PRE || entityId == INPUT2_CLIP_OL_PRE || entityId == INPUT3_CLIP_OL_PRE || entityId == INPUT4_CLIP_OL_PRE || entityId == STEREO_INPUT1_CLIP_OL_PRE) {
        // Not assigned yet: TODO
    } else if (entityId == INPUT1_CLIP_OL_POST || entityId == INPUT2_CLIP_OL_POST || entityId == INPUT3_CLIP_OL_POST || entityId == INPUT4_CLIP_OL_POST || entityId == STEREO_INPUT1_CLIP_OL_POST) {
        // Not assigned yet: TODO
    }
    
    if (instance->selectedChannel == input) {
        if (entityId == INPUT1_EFFECT_1_MUTE || entityId == INPUT2_EFFECT_1_MUTE || entityId == INPUT3_EFFECT_1_MUTE || entityId == INPUT4_EFFECT_1_MUTE) {
            uint8_t fx1TypeEncoder = 5;
            uint8_t fx1AmountEncoder = 9;
            bool mute = instance->showbox->getInputEffectMute(input, EFFECT1);
            if (mute) {
                instance->setLedRing(fx1TypeEncoder, 28);
                instance->setLedRing(fx1AmountEncoder, 28);
            } else {
                float amount = instance->showbox->getInputEffectAmount(input, EFFECT1);
                instance->setEncoderPercent(fx1AmountEncoder, amount);
                uint8_t type = instance->showbox->getInputEffectType(input, EFFECT1);
                float typePercent = type / 14.0;
                instance->setEncoderPercent(fx1TypeEncoder, typePercent);
            }
        } else if (entityId == INPUT1_EFFECT_2_MUTE || entityId == INPUT2_EFFECT_2_MUTE || entityId == INPUT3_EFFECT_2_MUTE || entityId == INPUT4_EFFECT_2_MUTE) {
            uint8_t fx2TypeEncoder = 6;
            uint8_t fx2AmountEncoder = 11;
            bool mute = instance->showbox->getInputEffectMute(input, EFFECT2);
            if (mute) {
                instance->setLedRing(fx2TypeEncoder, 28);
                instance->setLedRing(fx2AmountEncoder, 28);
            } else {
                float amount = instance->showbox->getInputEffectAmount(input, EFFECT2); // input effect amount goes from 0 to 127
                instance->setEncoderPercent(fx2AmountEncoder, amount);
                uint8_t type = instance->showbox->getInputEffectType(input, EFFECT2); // input effect type goes from 0 to 127
                float typePercent = type / 17.0;
                instance->setEncoderPercent(fx2TypeEncoder, typePercent);
            }
        } else if (entityId == INPUT1_EFFECT_1_AMOUNT || entityId == INPUT2_EFFECT_1_AMOUNT || entityId == INPUT3_EFFECT_1_AMOUNT || entityId == INPUT4_EFFECT_1_AMOUNT) {
            float amount = instance->showbox->getInputEffectAmount(input, EFFECT1); // input effect amount goes from 0.0 to 1.0
            instance->setChannelEncoderPercent(9, amount);
        } else if (entityId == INPUT1_EFFECT_2_AMOUNT || entityId == INPUT2_EFFECT_2_AMOUNT || entityId == INPUT3_EFFECT_2_AMOUNT || entityId == INPUT4_EFFECT_2_AMOUNT) {
            float amount = instance->showbox->getInputEffectAmount(input, EFFECT2); // input effect amount goes from 0.0 to 1.0
            instance->setChannelEncoderPercent(11, amount);
        } else if (entityId == INPUT1_EQ_ENABLE || entityId == INPUT2_EQ_ENABLE || entityId == INPUT3_EQ_ENABLE || entityId == INPUT4_EQ_ENABLE || entityId == STEREO_INPUT1_EQ_ENABLE) {
            bool enable = instance->showbox->getInputEqEnable(input);
            uint8_t eqHighEncoder = 8;
            uint8_t eqMidEncoder = 10;
            uint8_t eqLowEncoder = 12;
            if (enable) {
                uint8_t eqLowGain = instance->showbox->getInputEqGain(input, EQ_LOW_BAND); // input eq gain goes from -15.0 to 15.0
                uint8_t eqMidGain = instance->showbox->getInputEqGain(input, EQ_MID_BAND); // input eq gain goes from -15.0 to 15.0
                uint8_t eqHighGain = instance->showbox->getInputEqGain(input, EQ_HIGH_BAND); // input eq gain goes from -15.0 to 15.0
                float eqLowGainPercent = (eqLowGain + 15.0) / 30.0;
                float eqMidGainPercent = (eqMidGain + 15.0) / 30.0;
                float eqHighGainPercent = (eqHighGain + 15.0) / 30.0;
                instance->setEncoderPercent(eqLowEncoder, eqLowGainPercent);
                instance->setEncoderPercent(eqMidEncoder, eqMidGainPercent);
                instance->setEncoderPercent(eqHighEncoder, eqHighGainPercent);
            } else {
                instance->setLedRing(eqLowEncoder, 28);
                instance->setLedRing(eqMidEncoder, 28);
                instance->setLedRing(eqHighEncoder, 28);
            }
        } else if (entityId == INPUT1_EQ_LOW_GAIN || entityId == INPUT2_EQ_LOW_GAIN || entityId == INPUT3_EQ_LOW_GAIN || entityId == INPUT4_EQ_LOW_GAIN || entityId == STEREO_INPUT1_EQ_LOW_GAIN) {
            bool enable = instance->showbox->getInputEqEnable(input);
            if (enable) {
                float gain = instance->showbox->getInputEqGain(input, EQ_LOW_BAND); // input eq gain goes from -15.0 to 15.0
                float gainPercent = (gain + 15.0) / 30.0;
                instance->Debug->printf("Low Gain: %f\n", gainPercent);
                instance->setEncoderPercent(12, gainPercent);
            }
        } else if (entityId == INPUT1_EQ_MID_GAIN || entityId == INPUT2_EQ_MID_GAIN || entityId == INPUT3_EQ_MID_GAIN || entityId == INPUT4_EQ_MID_GAIN || entityId == STEREO_INPUT1_EQ_MID_GAIN) {
            bool enable = instance->showbox->getInputEqEnable(input);
            if (enable) {
                float gain = instance->showbox->getInputEqGain(input, EQ_MID_BAND); // input eq gain goes from -15.0 to 15.0
                float gainPercent = (gain + 15.0) / 30.0;
                instance->Debug->printf("Mid Gain: %f\n", gainPercent);
                instance->setEncoderPercent(10, gainPercent);
            }
        } else if (entityId == INPUT1_EQ_HIGH_GAIN || entityId == INPUT2_EQ_HIGH_GAIN || entityId == INPUT3_EQ_HIGH_GAIN || entityId == INPUT4_EQ_HIGH_GAIN || entityId == STEREO_INPUT1_EQ_HIGH_GAIN) {
            bool enable = instance->showbox->getInputEqEnable(input);
            if (enable) {
                float gain = instance->showbox->getInputEqGain(input, EQ_HIGH_BAND); // input eq gain goes from -15.0 to 15.0
                float gainPercent = (gain + 15.0) / 30.0;
                instance->Debug->printf("High Gain: %f\n", gainPercent);
                instance->setEncoderPercent(8, gainPercent);
            }
        } else if (entityId == INPUT1_EXT_FX_MUTE || entityId == INPUT2_EXT_FX_MUTE || entityId == INPUT3_EXT_FX_MUTE || entityId == INPUT4_EXT_FX_MUTE) {
            bool mute = instance->showbox->getInputExtFxMute(input);
            uint8_t extFxEncoder = 13;
            if (mute) {
                instance->setLedRing(extFxEncoder, 28);
            } else {
                float sends = instance->showbox->getInputExtFxSends(input); // input ext fx sends goes from 0.0 to 1.0
                instance->setEncoderPercent(extFxEncoder, sends);
            }
        } else if (entityId == INPUT1_EXT_FX_SENDS || entityId == INPUT2_EXT_FX_SENDS || entityId == INPUT3_EXT_FX_SENDS || entityId == INPUT4_EXT_FX_SENDS) {
            bool mute = instance->showbox->getInputExtFxMute(input);
            if (!mute) {
                float sends = instance->showbox->getInputExtFxSends(input); // input ext fx sends goes from 0.0 to 1.0
                instance->setEncoderPercent(13, sends);
            }
        } else if (entityId == INPUT1_COMPRESSOR_ENABLE || entityId == INPUT2_COMPRESSOR_ENABLE || entityId == INPUT3_COMPRESSOR_ENABLE || entityId == INPUT4_COMPRESSOR_ENABLE) {
            bool enable = instance->showbox->getInputCompressorEnable(input);
            uint8_t compressorEncoder = 14;
            if (enable) {
                float amount = instance->showbox->getInputCompressorAmount(input); // input compressor amount goes from 0.0 to 1.0
                instance->setEncoderPercent(compressorEncoder, amount);
            } else {
                instance->setLedRing(compressorEncoder, 28);
            }
        } else if (entityId == INPUT1_COMPRESSOR_AMOUNT || entityId == INPUT2_COMPRESSOR_AMOUNT || entityId == INPUT3_COMPRESSOR_AMOUNT || entityId == INPUT4_COMPRESSOR_AMOUNT) {
            bool enable = instance->showbox->getInputCompressorEnable(input);
            if (enable) {
                float amount = instance->showbox->getInputCompressorAmount(input); // input compressor amount goes from 0.0 to 1.0
                instance->setEncoderPercent(14, amount);
            }
        }
    }
}


// Update loop
void XtouchCompactAdapter::tick() {
    //midi->tick();
    if (!delayedConfigTriggered && millis() > beginTime+midiConfigDelay) {
        configureXtouchCompact();
        delayedConfigTriggered = true;
    }
}

// Set the debug serial
void XtouchCompactAdapter::setDebugSerial(Print* serial) {
    Debug = serial;
}

// Set midi object
void XtouchCompactAdapter::setMidi(MultiMidi* multiMidi) {
    midi = multiMidi;
    midi->setDefaultSendingChannel(midiChannel);
}

// Setters for the X Touch Compact controls

// Encoder setters
void XtouchCompactAdapter::setEncoder(uint8_t encoder, uint8_t value) { // 0-127
    uint8_t cc = 10 + encoder;
    Debug->printf("Setting encoder %d to %d\n", encoder, value);
    midi->controlChange(cc, value);
}

void XtouchCompactAdapter::setEncoderPercent(uint8_t encoder, float value) {
    uint8_t ccValue = value * 127;
    setEncoder(encoder, ccValue);
}

void XtouchCompactAdapter::setChannelEncoder(uint8_t channel, uint8_t value) {
    setEncoder(channel, value);
}

void XtouchCompactAdapter::setChannelEncoderPercent(uint8_t channel, float value) {
    setEncoderPercent(channel, value);
}


/**
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
| **LED Ring Remote Control**  | GLOBAL CH  | Encoders 1–8: CC 26 – CC 33 | 0 = all LEDs off                                                                          |
|                              |            | Encoders 9–16: CC 34 – CC 41| 1–13 = LEDs 1 (left) – 13 (right) on                                                      |
|                              |            |                             | 14–26 = LEDs 1 (left) – 13 (right) blinking                                               |
|                              |            |                             | 27 = all LEDs on                                                                          |
|                              |            |                             | 28 = all LEDs blinking                                                                    |
|                              |            |                             | 29–127 = ignored                                                                          |
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
*/
void XtouchCompactAdapter::setLedRing(uint8_t encoder, uint8_t value) { // 0-15
    uint8_t cc = 26 + encoder;
    midi->controlChange(cc, value, globalChannel);
}

void XtouchCompactAdapter::setLedRingPercent(uint8_t encoder, float value) {
    uint8_t ccValue = value * 27;
    setLedRing(encoder, ccValue);
}

void XtouchCompactAdapter::setChannelLedRing(uint8_t channel, uint8_t value) {
    uint8_t encoder = channel;
    setLedRing(encoder, value);
}

void XtouchCompactAdapter::setChannelLedRingPercent(uint8_t channel, float value) {
    uint8_t encoder = channel;
    setLedRingPercent(encoder, value);
}


// Button Control

void XtouchCompactAdapter::setButton(uint8_t button, bool state) {
    uint8_t note = button+16;
    if (state) {
        midi->noteOn(note, 127);
    } else {
        midi->noteOff(note, 0);
    }
}

void XtouchCompactAdapter::setChannelButton(uint8_t row, uint8_t channel, bool state) {
    uint8_t button = row * 8 + channel;
    setButton(button, state);
}

void XtouchCompactAdapter::setMainButton(bool state) {
    setButton(48-16, state);
}

void XtouchCompactAdapter::setRewindButton(bool state) {
    setButton(49-16, state);
}

void XtouchCompactAdapter::setFastForwardButton(bool state) {
    setButton(50-16, state);
}

void XtouchCompactAdapter::setLoopButton(bool state) {
    setButton(51-16, state);
}

void XtouchCompactAdapter::setRecordButton(bool state) {
    setButton(52-16, state);
}

void XtouchCompactAdapter::setStopButton(bool state) {
    setButton(53-16, state);
}

void XtouchCompactAdapter::setPlayButton(bool state) {
    setButton(54-16, state);
}


/**
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
| **Buttons LED Remote Control**| GLOBAL CH | Upper Top Row 1–8:          | Note 0 – Note 7                                                                           |
|                              |            | Upper Mid Row 9–16:         | Note 8 – Note 15                                                                          |
|                              |            | Upper Bottom Row 17–24:     | Note 16 – Note 23                                                                         |
|                              |            | Lower Row 25–33:            | Note 24 – Note 32                                                                         |
|                              |            | Right Area 34–39:           | Note 33 – Note 38                                                                         |
|                              |            | Layer A, B LEDs:            | Not assignable; only 1 of 2 LEDs lights if Standard mode selected (see above "Preset Layer Change")|
|                              |            |                             | Note on/off with Velocity 0: Button LED off                                               |
|                              |            |                             | Note on with Velocity 1: Button LED on                                                    |
|                              |            |                             | Note on with Velocity 2: Button LED blinking                                              |
|                              |            |                             | Note on with Velocity 3–127: ignored                                                      |
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
*/

void XtouchCompactAdapter::setButtonLed(uint8_t button, uint8_t value) { // 0-38
    midi->noteOn(button, value, globalChannel);
}

void XtouchCompactAdapter::setButtonLed(uint8_t button, bool state) {
    uint8_t value = state ? 1 : 0;
    setButtonLed(button, value);
}

void XtouchCompactAdapter::setChannelButtonLed(uint8_t row, uint8_t channel, uint8_t value) { // Set buttons 1-33 (4 rows) (rows 0,1,2,3) 
    uint8_t button = row * 8 + channel;
    setButtonLed(button, value);
}

void XtouchCompactAdapter::setChannelButtonLed(uint8_t row, uint8_t channel, bool state) {
    uint8_t value = state ? 1 : 0;
    setChannelButtonLed(row, channel, value);
}

void XtouchCompactAdapter::setMainButtonLed(uint8_t value) {
    setButtonLed(32, value);
}

void XtouchCompactAdapter::setMainButtonLed(bool state) {
    uint8_t value = state ? 1 : 0;
    setMainButtonLed(value);
}

void XtouchCompactAdapter::setRewindButtonLed(uint8_t value) {
    setButtonLed(34, value);
}

void XtouchCompactAdapter::setRewindButtonLed(bool state) {
    uint8_t value = state ? 1 : 0;
    setRewindButtonLed(value);
}

void XtouchCompactAdapter::setFastForwardButtonLed(uint8_t value) {
    setButtonLed(35, value);
}

void XtouchCompactAdapter::setLoopButtonLed(uint8_t value) {
    setButtonLed(36, value);
}

void XtouchCompactAdapter::setLoopButtonLed(bool state) {
    uint8_t value = state ? 1 : 0;
    setLoopButtonLed(value);
}

void XtouchCompactAdapter::setRecordButtonLed(uint8_t value) {
    setButtonLed(37, value);
}

void XtouchCompactAdapter::setRecordButtonLed(bool state) {
    uint8_t value = state ? 1 : 0;
    setRecordButtonLed(value);
}

void XtouchCompactAdapter::setStopButtonLed(uint8_t value) {
    setButtonLed(38, value);
}

void XtouchCompactAdapter::setStopButtonLed(bool state) {
    uint8_t value = state ? 1 : 0;
    setStopButtonLed(value);
}

void XtouchCompactAdapter::setPlayButtonLed(uint8_t value) {
    setButtonLed(0, value);
}

void XtouchCompactAdapter::setPlayButtonLed(bool state) {
    uint8_t value = state ? 1 : 0;
    setPlayButtonLed(value);
}

/**
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
| **Fader Movement**           | GLOBAL CH  | CC 1 – CC 9                 | 0–127 = fader position, bottom to top                                                     |
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
*/
void XtouchCompactAdapter::setFader(uint8_t fader, uint8_t value) { // index 0-8
    Debug->printf("Setting fader %d to %d\n", fader, value);
    midi->controlChange(fader+1, value); // Doesn't need global channel, but can be used alternatively, doesn't make a difference
}

void XtouchCompactAdapter::setFaderPercent(uint8_t fader, float value) {
    uint8_t ccValue = value * 127;
    setFader(fader, ccValue);
}

/**
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
| **Operation Mode Select**    | GLOBAL CH  | CC 127                      | 0 = Standard Mode, MC Mode LED off                                                        |
|                              |            |                             | 1 = MC Mode, MC Mode LED on                                                               |
|                              |            |                             | 2–127 = ignored                                                                           |
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
*/
void XtouchCompactAdapter::setOperationMode(uint8_t mode) {
    midi->controlChange(127, mode, globalChannel);
}

/**
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
| **Preset Layer Change**      | GLOBAL CH  | Program Change              | ONLY in Standard Mode (see above):                                                        |
|                              |            |                             | 0 = Layer A, A-button LED on                                                              |
|                              |            |                             | 1 = Layer B, B-button LED on                                                              |
|                              |            |                             | 2–127 = ignored                                                                           |
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
*/ 
void XtouchCompactAdapter::setPresetLayer(uint8_t layer) {
    midi->programChange(layer, globalChannel);
}

/**
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
| **LED Ring Behavior Change** | GLOBAL CH  | Encoders 1–8: CC 10 – CC 17 | 0 = Single                                                                                |
|                              |            | Encoders 9–16: CC 18 – CC 25| 1 = Pan                                                                                   |
|                              |            |                             | 2 = Fan                                                                                   |
|                              |            |                             | 3 = Spread                                                                                |
|                              |            |                             | 4 = Trim                                                                                  |
|                              |            |                             | 5–127 = ignored                                                                           |
|------------------------------|------------|-----------------------------|-------------------------------------------------------------------------------------------|
*/
void XtouchCompactAdapter::setLedRingBehavior(uint8_t encoder, uint8_t behavior) {
    uint8_t cc = 10 + encoder;
    midi->controlChange(cc, behavior, globalChannel);
}


// Callback methods
void XtouchCompactAdapter::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("X-Touch Note Off Event - Channel: %d - Note: %d - Velocity: %d\n", channel, note, velocity);
    if (channel != instance->midiChannel) {
        return;
    }
    if (note == 8 || note == 10 || note == 12) { // enable/disable EQ
        bool eqEnabled = instance->showbox->getInputEqEnable(instance->selectedChannel);
        instance->Debug->printf("Toggle EQ for channel %d: %s\n", instance->selectedChannel, eqEnabled ? "Disable" : "Enable");
        instance->showbox->setInputEqEnable(instance->selectedChannel, !eqEnabled);
    } else if (note == 9) {//  enable/disable FX1
        bool fx1Enabled = instance->showbox->getInputEffectMute(instance->selectedChannel, effect_channel::EFFECT1);
        instance->Debug->printf("Toggle FX1 for channel %d: %s\n", instance->selectedChannel, fx1Enabled ? "Disable" : "Enable");
        instance->showbox->setInputEffectMute(instance->selectedChannel, effect_channel::EFFECT1, !fx1Enabled);
    } else if (note == 11) { // enable/disable FX2
        bool fx2Enabled = instance->showbox->getInputEffectMute(instance->selectedChannel, effect_channel::EFFECT2);
        instance->Debug->printf("Toggle FX2 for channel %d: %s\n", instance->selectedChannel, fx2Enabled ? "Disable" : "Enable");
        instance->showbox->setInputEffectMute(instance->selectedChannel, effect_channel::EFFECT2, !fx2Enabled);
    } else if (note == 13) { // enable/disable Ext FX
        bool extFxEnabled = instance->showbox->getInputExtFxMute(instance->selectedChannel);
        instance->Debug->printf("Toggle Ext FX for channel %d: %s\n", instance->selectedChannel, extFxEnabled ? "Disable" : "Enable");
         instance->showbox->setInputExtFxMute(instance->selectedChannel, !extFxEnabled);

    } else if (note == 14) { // enable/disable Compressor
        bool compressorEnabled = instance->showbox->getInputCompressorEnable(instance->selectedChannel);
        instance->Debug->printf("Toggle Compressor for channel %d: %s\n", instance->selectedChannel, compressorEnabled ? "Disable" : "Enable");
        instance->showbox->setInputCompressorEnable(instance->selectedChannel, !compressorEnabled);
    } else if (note >= 24 && note <= 27) {
        tuner_chan chan = static_cast<tuner_chan>(note - 24);
        instance->Debug->printf("Turn on tuner for channel %d\n", chan);
        // instance->showbox->tunerAction(tuner_action::TURN_ON, chan);  // TODO: Needs a getter for tuner state
    } else if (note >= 32 && note <= 36) { // select channel buttons
        instance->selectedChannel = note - 32;
        instance->Debug->printf("Select channel %d\n", instance->selectedChannel);
        // turn off all selected chanel buttons
        instance->setChannelButton(2, 0, false);
        instance->setChannelButton(2, 1, false);
        instance->setChannelButton(2, 2, false);
        instance->setChannelButton(2, 3, false);
        instance->setChannelButton(2, 4, false);
        // turn on selected channel button
        instance->setChannelButton(2, instance->selectedChannel, true);
    } else if (note >= 40 && note <= 44) { // mute/unmute inputs
        uint8_t input = note - 40;
        bool mute = instance->showbox->getInputMute(input);
        instance->Debug->printf("Toggle mute for input %d: %s\n", input, mute ? "Unmute" : "Mute");
        instance->showbox->setInputMute(input, !mute);
        instance->setChannelButton(3, input, !mute);
    } else if (note == 48) {
        bool masterMute = instance->showbox->getMainMute();
        instance->Debug->printf("Toggle master mute: %s\n", masterMute ? "Unmute" : "Mute");
        instance->showbox->setMainMute(!masterMute);
        instance->setMainButton(!masterMute);
    } else if (note == 29) { // Front LED
        instance->Debug->printf("Toggle front LED\n");
        bool state = instance->showbox->getFrontLed();
        instance->showbox->setFrontLed(!state);
        instance->setChannelButton(2, 5, !state);
    } else if (note == 30) { // AMP_PA_MODE
        instance->Debug->printf("Toggle PA mode\n");
        bool state = instance->showbox->getAmpPaMode();
        instance->showbox->setAmpPaMode(!state);
        instance->setChannelButton(2, 6, !state);
    } else if (note == 37) { // Feedback elimination
        instance->Debug->printf("Toggle feedback elimination\n");
        bool state = instance->showbox->getFeedbackElim();
        instance->showbox->setFeedbackElim(!state);
        instance->setChannelButton(3, 5, !state);
    } else if (note == 38) { //  Outdoor mode (location mdoe)
        instance->Debug->printf("Toggle outdoor mode\n");
        bool state = instance->showbox->getLocationMode();
        instance->showbox->setLocationMode(!state);
        instance->setChannelButton(3, 6, !state);
    } else if (note == 23) { // Snapshot lock
        instance->snapshotLock = !instance->snapshotLock;
        instance->setChannelButton(0, 7, instance->snapshotLock);
        instance->Debug->printf("Toggle snapshot lock: %s\n", instance->snapshotLock ? "Lock" : "Unlock");
        instance->selectedSnapshot = -1;
        instance->setEncoder(7, 0);
    } else if (note == 31) { // Load snapshot
        if (instance->snapshotLock) {
            instance->Debug->printf("Snapshot lock is enabled, can't load snapshot\n");
            return;
        }
        if (instance->selectedSnapshot != -1) {
            instance->Debug->printf("Recall snapshot %d\n", instance->selectedSnapshot);
            instance->showbox->snapshotAction(snapshot_action::RECALL, static_cast<snapshot_slot>(instance->selectedSnapshot));
            instance->selectedSnapshot = -1;
            instance->setEncoder(7, 0);
        }
    } else if (note == 39) { // Save snapshot
        if (instance->snapshotLock) {
            instance->Debug->printf("Snapshot lock is enabled, can't save snapshot\n");
            return;
        }
        if (instance->selectedSnapshot != -1) {
            instance->Debug->printf("Save snapshot %d\n", instance->selectedSnapshot);
            instance->showbox->snapshotAction(snapshot_action::SAVE, static_cast<snapshot_slot>(instance->selectedSnapshot));
            instance->selectedSnapshot = -1;
            instance->setEncoder(7, 0);
        }
    } else if (note == 51) { // looper record
        looper_state state = instance->showbox->getLooperState();
        instance->Debug->printf("Looper record: current state %d\n", state);
        if (state == looper_state::DELETE) { // looper is empty, start recording first layer
            instance->Debug->printf("Start recording first layer\n");
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
        } else if (state == looper_state::RECORD_INITIAL_LOOP) { // first layer is getting recorded, don't do anything
            return;
        } else if (state == looper_state::STOP_PLAYING) { // playback is paused, start playing and record overdub
            instance->Debug->printf("Start playing and record overdub\n");
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
        } else if (state == looper_state::PLAY) { // looper is playing, record overdub
            instance->Debug->printf("Record overdub\n");
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
        } else if (state == looper_state::RECORD_OVERDUB) { // looper is recording overdub, don't do anything
            return;
        }
    } else if (note == 52) {
        instance->Debug->printf("Toggle SD card record\n");
        instance->showbox->toggleSdCardRecord();  // TODO: Needs a getter for SD card record state
    } else if (note == 53) { // looper stop/clear
        looper_state state = instance->showbox->getLooperState();
        instance->Debug->printf("Looper stop/clear: current state %d\n", state);
        if (state == looper_state::DELETE) { // looper is already empty, don't do anything
            return;
        } else if (state == looper_state::RECORD_INITIAL_LOOP) { // first layer is getting recorded, stop recording and stop playing
            instance->Debug->printf("Stop recording and stop playing\n");
            instance->showbox->setLooperLevel(looper_button_action::DOUBLE_PRESS);
        } else if (state == looper_state::STOP_PLAYING) { // playback is paused, clear looper
            instance->Debug->printf("Clear looper\n");
            instance->showbox->setLooperLevel(looper_button_action::LONG_PRESS);
        } else if (state == looper_state::PLAY) { // looper is playing, stop playing
            instance->Debug->printf("Stop playing\n");
            instance->showbox->setLooperLevel(looper_button_action::DOUBLE_PRESS);
        } else if (state == looper_state::RECORD_OVERDUB) { // looper is recording overdub, stop recording/playing
            instance->Debug->printf("Stop recording/playing\n");
            instance->showbox->setLooperLevel(looper_button_action::DOUBLE_PRESS);
            return;
        }
    } else if (note == 54) { // looper play
        looper_state state = instance->showbox->getLooperState();
        instance->Debug->printf("Looper play: current state %d\n", state);
        if (state == looper_state::DELETE) { // looper is empty, don't do anything
            return;
        } else if (state == looper_state::RECORD_INITIAL_LOOP) { // first layer is getting recorded, stop recording and play
            instance->Debug->printf("Stop recording and play\n");
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
        } else if (state == looper_state::STOP_PLAYING) { // playback is paused, start playing
            instance->Debug->printf("Start playing\n");
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
        } else if (state == looper_state::PLAY) { // looper is playing, dont do anything
            return;
        } else if (state == looper_state::RECORD_OVERDUB) { // looper is recording overdub, stop recording and start playing
            instance->Debug->printf("Stop recording and start playing\n");
            instance->showbox->setLooperLevel(looper_button_action::DOWN);
            instance->showbox->setLooperLevel(looper_button_action::UP);
        }
    } else if (note == 21) {
        instance->fx1Lock = !instance->fx1Lock;
        instance->setChannelButton(0, 5, instance->fx1Lock);
        instance->Debug->printf("Toggle FX1 Lock: %s\n", instance->fx1Lock ? "On" : "Off");
    } else if (note == 22) { // Button 8
        instance->fx2Lock = !instance->fx2Lock;
        instance->setChannelButton(0, 6, instance->fx2Lock);
        instance->Debug->printf("Toggle FX2 Select Mode: %s\n", instance->fx2Lock ? "On" : "Off");
    }
}

void XtouchCompactAdapter::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("X-Touch Note On Event - Channel: %d - Note: %d - Velocity: %d\n", channel, note, velocity);
    if (channel != instance->midiChannel) {
        return;
    }
}

void XtouchCompactAdapter::onControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    instance->Debug->printf("X-Touch Control Change Event - Channel: %d - Controller: %d - Value: %d\n", channel, controller, value);
    if (channel != instance->midiChannel) {
        return;
    }
    // instance->controlChange(ccNumber, ccValue, 1);
    if (controller >= 1 && controller <= 5) { // input volume faders
        float percentInputVolumeLinear = value / 127.0;
        float percentInputVolume = instance->mapLinearToNonlinear(percentInputVolumeLinear);
        // value needs to be converted from int 0-127 to float -120.0-10.0
        float inputVolume = percentInputVolume * 130.0 - 120.0;
        instance->Debug->printf("Set input volume for fader %d: %f\n", controller - 1, inputVolume);
        instance->showbox->setInputVolume(controller - 1, inputVolume);
    } else if (controller == 9) { // master gain fader
        float percentMasterGainLinear = value / 127.0;
        float percentMasterGain = instance->mapLinearToNonlinear(percentMasterGainLinear);
        // value needs to be converted from int 0-127 to float -120.0-6.0
        float masterGain = percentMasterGain * 126.0 - 120.0;
        instance->Debug->printf("Set master gain: %f\n", masterGain);
        instance->showbox->setMainMasterGain(masterGain);
    } else if (controller >= 10 && controller <= 13) { // input gain encoders
        // value needs to be converted from int 0-127 to float 0.0-60.0
        float inputGain = (value / 127.0) * 60.0;
        instance->Debug->printf("Set input gain for encoder %d: %f\n", controller - 10, inputGain);
        instance->showbox->setInputGain(controller - 10, inputGain);
    } else if (controller == 15) { // cycle through available effects for FX1 (0-14)
        if (!instance->fx1Lock) {
            float valuePercent = value / 127.0;
            uint8_t selectedEffect = valuePercent * 14.0;
            instance->Debug->printf("Select FX1 effect: %d\n", selectedEffect);
            instance->showbox->setInputEffectType(instance->selectedChannel, effect_channel::EFFECT1, selectedEffect);
        } else {
            instance->Debug->printf("FX1 Lock Mode is locked\n");
            uint8_t currentEffect = instance->showbox->getInputEffectType(instance->selectedChannel, effect_channel::EFFECT1);
            uint8_t currentEffectPercent = currentEffect / 14.0;
            instance->setChannelEncoderPercent(5, currentEffectPercent);
        }
    } else if (controller == 16) { // cycle through available effects for FX2 (0-17)
        if (!instance->fx2Lock) {
            float valuePercent = value / 127.0;
            uint8_t selectedEffect = valuePercent * 17.0;
            instance->Debug->printf("Select FX2 effect: %d\n", selectedEffect);
            instance->showbox->setInputEffectType(instance->selectedChannel, effect_channel::EFFECT2, selectedEffect);
        } else {
            instance->Debug->printf("FX2 Select Mode is locked\n");
            uint8_t currentEffect = instance->showbox->getInputEffectType(instance->selectedChannel, effect_channel::EFFECT2);
            uint8_t currentEffectPercent = currentEffect / 17.0;
            instance->setChannelEncoderPercent(6, currentEffectPercent);
        }
    } else if (controller == 17) { // snapshot slot selection (0-4)
        instance->Debug->printf("Select snapshot: %d\n", value);
        if (!instance->snapshotLock) {
            if (value == 0) {
                instance->selectedSnapshot = 0;
            } else if (value >= 1 && value <= 5) {
                instance->selectedSnapshot = value -1;
            } else {
                instance->selectedSnapshot = 4;
            }
            instance->setChannelEncoder(7, instance->selectedSnapshot+1);
            instance->setChannelLedRing(7, instance->selectedSnapshot+1);
        } else {
            instance->selectedSnapshot = -1;
            instance->setChannelEncoder(7, 0);
            instance->setChannelLedRing(7, 0);
        }
    } else if (controller == 18) { // low eq gain (-15.0 - 15.0)
        float lowEqGain = (value / 127.0) * 30.0 - 15.0;
        instance->Debug->printf("Set low EQ gain: %f\n", lowEqGain);
        instance->showbox->setInputEqGain(instance->selectedChannel, eq_band::EQ_LOW_BAND, lowEqGain);
    } else if (controller == 19) { // FX1 amount (0.0 - 1.0)
        float fx1Amount = value / 127.0;
        instance->Debug->printf("Set FX1 amount: %f\n", fx1Amount);
        instance->showbox->setInputEffectAmount(instance->selectedChannel, effect_channel::EFFECT1, fx1Amount);
    } else if (controller == 20) { // mid eq gain (-15.0 - 15.0)
        float midEqGain = (value / 127.0) * 30.0 - 15.0;
        instance->Debug->printf("Set mid EQ gain: %f\n", midEqGain);
        instance->showbox->setInputEqGain(instance->selectedChannel, eq_band::EQ_MID_BAND, midEqGain);
    } else if (controller == 21) { // FX2 amount (0.0 - 1.0)
        float fx2Amount = value / 127.0;
        instance->Debug->printf("Set FX2 amount: %f\n", fx2Amount);
        instance->showbox->setInputEffectAmount(instance->selectedChannel, effect_channel::EFFECT2, fx2Amount);
    } else if (controller == 22) { // high eq gain (-15.0 - 15.0)
        float highEqGain = (value / 127.0) * 30.0 - 15.0;
        instance->Debug->printf("Set high EQ gain: %f\n", highEqGain);
        instance->showbox->setInputEqGain(instance->selectedChannel, eq_band::EQ_HIGH_BAND, highEqGain);
    } else if (controller == 23) { // ext fx send (0.0 - 1.0)
        float extFxSend = value / 127.0;
        instance->Debug->printf("Set ext FX send: %f\n", extFxSend);
        instance->showbox->setInputExtFxSends(instance->selectedChannel, extFxSend);
    } else if (controller == 24) { // compressor amount (0.0 - 1.0)
        float compressorAmount = value / 127.0;
        instance->Debug->printf("Set compressor amount: %f\n", compressorAmount);
        instance->showbox->setInputCompressorAmount(instance->selectedChannel, compressorAmount);
    }
}

void XtouchCompactAdapter::onPitchBend(uint8_t channel, uint8_t value) {
    instance->Debug->printf("X-Touch Pitch Bend Event - Channel: %d - Value: %d\n", channel, value);
    if (channel != instance->midiChannel) {
        return;
    }
}

float XtouchCompactAdapter::mapNonlinearToLinear(float nonlinearValue) {
  // Steepness factor, tweak this for better approximation
  const float n = 4.2;

  // Apply the power function
  return pow(nonlinearValue, n);
}

float XtouchCompactAdapter::mapLinearToNonlinear(float linearValue) {
  // Steepness factor, tweak this for better approximation
  const float n = 4.2;

  // Apply the power function
  return pow(linearValue, 1.0 / n);
}

/*
// debug midi tx
sendmidi dev "X-TOUCH COMPACT MIDI 1" on 0 1  # off
sendmidi dev "X-TOUCH COMPACT MIDI 1" on 0 2  # on
sendmidi dev "X-TOUCH COMPACT MIDI 1" on 0 3  # blink

// debug midi rx
aconnect -l 
aseqdump -p 24:0

X-TOUCH COMPACT MIDI MAP
PRESET LAYER A “Mixer Control” (MIDI Channel =1)

Notes:
Every push function (encoders + buttons) is a NOTE
command. The encoder rotation, fader movement, fader
touch, and foot control functions have CC commands.

Midi Notes:
0: Encoder push - unused
1: Encoder push - unused
2: Encoder push - unused
3: Encoder push - unused
4: Encoder push - unused
5: Encoder push - unused
6: Encoder push - unused
7: Encoder push - unused
8: Encoder push - Low EQ gain (for selected channel)
9: Encoder push - FX 1 amount (for selected channel)
10: Encoder push - Mid EQ gain (for selected channel)
11: Encoder push - FX 2 amount (for selected channel)
12: Encoder push - High EQ gain (for selected channel)
13: Encoder push - Ext FX send (for selected channel)
14: Encoder push - Compressor amount (for selected channel)
15: Encoder push - Unused
16: Button - Select Snapshot 1
17: Button - Select Snapshot 2
18: Button - Select Snapshot 3
19: Button - Select Snapshot 4
20: Button - Select Snapshot 5
21: Button - Select Snapshot Default
22: Button - Enter/Exit effect type change mode for FX 1
23: Button - Enter/Exit effect type change mode for FX 2
24: Button - Toggle tuner for channel 1
25: Button - Toggle tuner for channel 2
26: Button - Toggle tuner for channel 3
27: Button - Toggle tuner for channel 4
28: Button - Unused
29: Button - Unused
30: Button - Unused
31: Button - Unused
32: Button - Select channel 1
33: Button - Select channel 2
34: Button - Select channel 3
35: Button - Select channel 4
36: Button - Select channel 5 (stereo)
37: Unused
38: Unused
39: Unused
40: Button - Mute channel 1
41: Button - Mute channel 2
42: Button - Mute channel 3
43: Button - Mute channel 4
44: Button - Mute channel 5 (stereo)
45: Unused
46: Unused
47: Unused
48: Button - Master Mute
49: Button - Load snapshot mode
50: Button - Save snapshot mode
51: Button - Looper record
52: Button - SD record
53: Button - Looper stop/Clear
54: Button - Looper play

CC Commands:
1-5: Channel Fader Movement
6-8: Unused faders
9: Master Fader Movement
10-13: Encoder rotation - Channel Gain
14: Encoder rotation - Unused
15: Encoder rotation - Unused
16: Select FX 1 effect type for selected channel
17: Select FX 2 effect type for selected channel
18: Encoder rotation - Low EQ gain (for selected channel)
19: Encoder rotation - FX 1 amount (for selected channel)
20: Encoder rotation - Mid EQ gain (for selected channel)
21: Encoder rotation - FX 2 amount (for selected channel)
22: Encoder rotation - High EQ gain (for selected channel)
23: Encoder rotation - Ext FX send (for selected channel)
24: Encoder rotation - Compressor amount (for selected channel)
25: Encoder rotation - Unused
26: Expression Pedal (no function assigned)
27: Foot Switch (no function assigned)
101-108: Channel Fader Touch (no function assigned)
109: Master Fader Touch (no function assigned)
*/