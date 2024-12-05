#include "MackieControlUniversalAdapter.h"

MackieControlUniversalAdapter* MackieControlUniversalAdapter::instance = nullptr;

// Constructor
MackieControlUniversalAdapter::MackieControlUniversalAdapter(MackieShowbox* showbox)
    : showbox(showbox) {
        instance = this;
    }

// Initialization
void MackieControlUniversalAdapter::begin() {
    midi->action.setCallbackOnControlChange(MackieControlUniversalAdapter::onControlChange);
    midi->action.setCallbackOnNoteOn(MackieControlUniversalAdapter::onNoteOn);
    midi->action.setCallbackOnNoteOff(MackieControlUniversalAdapter::onNoteOff);
    midi->action.setCallbackOnPitchBend(MackieControlUniversalAdapter::onPitchBend);
}

// Update loop
void MackieControlUniversalAdapter::tick() {
    //midi->tick();
}

// Set the debug serial
void MackieControlUniversalAdapter::setDebugSerial(Print* serial) {
    Debug = serial;
}

// Set midi object
void MackieControlUniversalAdapter::setMidi(MultiMidi* multiMidi) {
    midi = multiMidi;
}

// Callback methods
void MackieControlUniversalAdapter::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("MCU Note On: %d, %d, %d\n", channel, note, velocity);
}

void MackieControlUniversalAdapter::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("MCU Note Off: %d, %d, %d\n", channel, note, velocity);
}

void MackieControlUniversalAdapter::onControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    instance->Debug->printf("MCU Control Change: %d, %d, %d\n", channel, controller, value);
    // instance->controlChange(ccNumber, ccValue, 1);
    
}

void MackieControlUniversalAdapter::onPitchBend(uint8_t channel, uint8_t value) {
    instance->Debug->printf("MCU Pitch Bend: %d, %d\n", channel, value);
}