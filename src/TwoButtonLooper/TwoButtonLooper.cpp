#include "TwoButtonLooper.h"
#define SHOWBOX_DEBUG 1

// Constructor
TwoButtonLooper::TwoButtonLooper(uint8_t record_button_pin, uint8_t stop_button_pin, MackieShowbox* showbox)
    : recordButton(record_button_pin, true), stopButton(stop_button_pin, true), showbox(showbox) {}

// Initialization
void TwoButtonLooper::begin() {
    // Attach static callback functions with the instance pointer
    recordButton.attachClick(recordOverdubPlayCallback, this);
    stopButton.attachClick(stopCallback, this);
    stopButton.attachLongPressStart(deleteCallback, this);
}

// Update loop
void TwoButtonLooper::tick() {
    recordButton.tick();
    stopButton.tick();
}

// Static callback functions
void TwoButtonLooper::recordOverdubPlayCallback(void *instance) {
    TwoButtonLooper *looper = static_cast<TwoButtonLooper*>(instance);
    looper->handleRecordOverdubPlay();
}

void TwoButtonLooper::stopCallback(void *instance) {
    TwoButtonLooper *looper = static_cast<TwoButtonLooper*>(instance);
    looper->handleStop();
}

void TwoButtonLooper::deleteCallback(void *instance) {
    TwoButtonLooper *looper = static_cast<TwoButtonLooper*>(instance);
    looper->handleDelete();
}

// Instance methods
void TwoButtonLooper::handleRecordOverdubPlay() {
    showbox->sendLooperButtonAction(looper_button_action::DOWN);
    showbox->sendLooperButtonAction(looper_button_action::UP);

    //#ifdef SHOWBOX_DEBUG
    uint8_t currentState = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);
    if (currentState == (uint8_t)DELETE || currentState == (uint8_t)STOP_PLAYING) {
        Debug->println("Recording initial loop");
    } else if (currentState == (uint8_t)RECORD_INITIAL_LOOP) {
        Debug->println("Recording overdub");
    } else if (currentState == (uint8_t)RECORD_OVERDUB) {
        Debug->println("Play");
    }
    //#endif
}

void TwoButtonLooper::handleStop() {
    uint8_t currentState = showbox->getUint8EntityValue(entity_id::LOOPER_STATE);

    if (currentState == (uint8_t)PLAY || currentState == (uint8_t)RECORD_INITIAL_LOOP || currentState == (uint8_t)RECORD_OVERDUB) {
        showbox->sendLooperButtonAction(looper_button_action::DOUBLE_PRESS);
        //#ifdef SHOWBOX_DEBUG
        Debug->println("Stop playing");
        //#endif
    }
}

void TwoButtonLooper::handleDelete() {
    showbox->sendLooperButtonAction(looper_button_action::LONG_PRESS);
    //#ifdef SHOWBOX_DEBUG
    Debug->println("Set LOOPER_STATE to DELETE");
    //#endif
}

void TwoButtonLooper::setDebugSerial(Print* serial) {
    Debug = serial;
}