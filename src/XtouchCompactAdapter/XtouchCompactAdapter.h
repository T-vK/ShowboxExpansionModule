#ifndef MACKIE_CONTROL_UNIVERSAL_ADAPTER_H
#define MACKIE_CONTROL_UNIVERSAL_ADAPTER_H

#include <Arduino.h>
#include "MackieShowbox/MackieShowbox.h"
#include "MultiMidi/MultiMidi.h"

class XtouchCompactAdapter {
public:
    // Constructor
    XtouchCompactAdapter(MackieShowbox* showbox);

    // Initialization method
    void begin();

    // Update method, to be called in the loop
    void tick();

    // Set the debug serial
    void setDebugSerial(Print* serial);

    // Set midi object
    void setMidi(MultiMidi* midi);

    // Setters for the X Touch Compact controls

    // Encoder setters
    void setEncoder(uint8_t encoder, uint8_t value); // 0-15
    void setEncoderPercent(uint8_t encoder, float value);
    
    void setChannelEncoder(uint8_t channel, uint8_t value);
    void setChannelEncoderPercent(uint8_t channel, float value);

    // LED ring setters
    void setLedRing(uint8_t encoder, uint8_t value);
    void setLedRingPercent(uint8_t encoder, float value);

    void setChannelLedRing(uint8_t channel, uint8_t value);
    void setChannelLedRingPercent(uint8_t channel, float value);

    // Button Control setters
    void setButton(uint8_t button, bool state);

    void setChannelButton(uint8_t row, uint8_t channel, bool state);

    void setMainButton(bool state);

    void setRewindButton(bool state);

    void setFastForwardButton(bool state);

    void setLoopButton(bool state);

    void setRecordButton(bool state);

    void setStopButton(bool state);

    void setPlayButton(bool state);

    // Button LED setters

    void setButtonLed(uint8_t button, uint8_t value);
    void setButtonLed(uint8_t button, bool value);

    void setChannelButtonLed(uint8_t row, uint8_t channel, uint8_t value);
    void setChannelButtonLed(uint8_t row, uint8_t channel, bool value);

    void setFader(uint8_t fader, uint8_t value);
    void setFaderPercent(uint8_t fader, float value);

    void setOperationMode(uint8_t mode);

    void setPresetLayer(uint8_t layer);

    void setLedRingBehavior(uint8_t encoder, uint8_t behavior);

    void setMainButtonLed(uint8_t value);
    void setMainButtonLed(bool value);

    void setRewindButtonLed(uint8_t value);
    void setRewindButtonLed(bool value);

    void setFastForwardButtonLed(uint8_t value);
    void setFastForwardButtonLed(bool value);

    void setLoopButtonLed(uint8_t value);
    void setLoopButtonLed(bool value);

    void setRecordButtonLed(uint8_t value);
    void setRecordButtonLed(bool value);

    void setStopButtonLed(uint8_t value);
    void setStopButtonLed(bool value);

    void setPlayButtonLed(uint8_t value);
    void setPlayButtonLed(bool value);

    // selected channel is not synched between showbox and xtouch compact so that they can be used independently
    uint8_t midiChannel = 0;
    uint8_t globalChannel = 1; // Has to be set up in the X-Touch Editor - You must select "2" for the global channel
    uint8_t selectedChannel = 0;
    bool fx1Lock = false;
    bool fx2Lock = false;
    bool snapshotLock = false;
    uint8_t selectedSnapshot = -1;
private:
    MackieShowbox* showbox;
    Print* Debug = &Serial;
    MultiMidi* midi = nullptr;
    MidiCallbackAction action;
    unsigned long midiConfigDelay = 2000;
    unsigned long beginTime = 0;
    bool delayedConfigTriggered = false;

    void configureXtouchCompact();

    // Showbox entity change callback
    static void onShowboxEntityChange(entity_id entityId);

    // Utility functions
    float mapNonlinearToLinear(float nonlinearValue);
    float mapLinearToNonlinear(float linearValue);
    
    // Midi callbacks
    static void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    static void onPitchBend(uint8_t channel, uint8_t value);    
    static XtouchCompactAdapter* instance;
};

#endif // MACKIE_CONTROL_UNIVERSAL_ADAPTER_H