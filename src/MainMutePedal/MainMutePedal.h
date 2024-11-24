#ifndef MAIN_MUTE_PEDAL_H
#define MAIN_MUTE_PEDAL_H

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

class MainMutePedal {
public:
    // Constructor
    MainMutePedal(uint8_t button_pin, MackieShowbox* showbox);

    // Initialization method
    void begin();

    // Update method, to be called in the loop
    void tick();

    // Set the debug serial
    void setDebugSerial(Print* serial);

private:
    // Static callback function
    static void toggleCallback(void *instance);

    // Instance method that the callback will call
    void handleToggle();

    OneButton button;
    MackieShowbox* showbox;
    entity_id entity = entity_id::MAIN_MUTE;
    bool onValue = Showbox::MainMute::ON;
    bool offValue = Showbox::MainMute::OFF;

    Print* Debug = &Serial;
};

#endif // MAIN_MUTE_PEDAL_H
