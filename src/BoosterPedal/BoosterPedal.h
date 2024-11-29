#ifndef BOOSTER_PEDAL_H
#define BOOSTER_PEDAL_H

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

class BoosterPedal {
public:
    // Constructor
    BoosterPedal(uint8_t button_pin, MackieShowbox* showbox, entity_id entity);

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
    entity_id entity;
    float value1 = Showbox::InputGain::POSITION_9;
    float value2 = Showbox::InputGain::POSITION_14;

    Print* Debug = &Serial;
};

#endif // BOOSTER_PEDAL_H
