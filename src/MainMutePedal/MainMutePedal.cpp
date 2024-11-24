#include "MainMutePedal.h"
#define SHOWBOX_DEBUG

// Constructor
MainMutePedal::MainMutePedal(uint8_t button_pin, MackieShowbox* showbox)
    : button(button_pin, true), showbox(showbox) {}

// Initialization
void MainMutePedal::begin() {
    // Attach static callback function with the instance pointer
    button.attachClick(toggleCallback, this);
}

// Update loop
void MainMutePedal::tick() {
    button.tick();
}

// Static callback function
void MainMutePedal::toggleCallback(void *instance) {
    MainMutePedal *pedal = static_cast<MainMutePedal*>(instance);
    pedal->handleToggle();
}

// Instance method
void MainMutePedal::handleToggle() {
    bool current_value = showbox->getBoolEntityValue(entity);
    
    if (current_value == onValue) {
        showbox->setEntityValue(entity, offValue);
    } else {
        showbox->setEntityValue(entity, onValue);
    }

    //#ifdef SHOWBOX_DEBUG
    Debug->print("Toggled entity ");
    Debug->print(static_cast<int>(entity));
    Debug->print(" to value ");
    Debug->println(showbox->getBoolEntityValue(entity));
    //#endif
}

// Set the debug serial
void MainMutePedal::setDebugSerial(Print* serial) {
    Debug = serial;
}