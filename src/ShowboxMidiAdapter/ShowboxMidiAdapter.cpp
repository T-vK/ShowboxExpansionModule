#include "ShowboxMidiAdapter.h"

ShowboxMidiAdapter* ShowboxMidiAdapter::instance = nullptr;

// Constructor
ShowboxMidiAdapter::ShowboxMidiAdapter(MackieShowbox* showbox)
    : showbox(showbox) {
        instance = this;
    }

// Initialization
void ShowboxMidiAdapter::begin(uint8_t rxPin, uint8_t txPin) {
    midi.setDebugSerial(Debug);
    midi.enableHardwareMidi(rxPin, txPin);
    midi.enableAppleMidi(5004);
    midi.enableBleMidi("ShowboxMidi");
    // midi.enableBleSerialMidi();
    action = MidiCallbackAction();
    action.setCallbackOnControlChange(ShowboxMidiAdapter::onControlChange);
    action.setCallbackOnNoteOn(ShowboxMidiAdapter::onNoteOn);
    action.setCallbackOnNoteOff(ShowboxMidiAdapter::onNoteOff);
    action.setCallbackOnPitchBend(ShowboxMidiAdapter::onPitchBend);
    midi.begin(action);
}

// Update loop
void ShowboxMidiAdapter::tick() {
    midi.loop();
}

// Set the debug serial
void ShowboxMidiAdapter::setDebugSerial(Print* serial) {
    Debug = serial;
}

// Callback methods
void ShowboxMidiAdapter::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("Note On: %d, %d, %d\n", channel, note, velocity);
}

void ShowboxMidiAdapter::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("Note Off: %d, %d, %d\n", channel, note, velocity);
}

void ShowboxMidiAdapter::onControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    instance->Debug->printf("Control Change: %d, %d, %d\n", channel, controller, value);
}

void ShowboxMidiAdapter::onPitchBend(uint8_t channel, uint8_t value) {
    instance->Debug->printf("Pitch Bend: %d, %d\n", channel, value);
}