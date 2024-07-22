#ifndef BOOSTER_PEDAL_H
#define BOOSTER_PEDAL_H

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

class BoosterPedal {
public:
    // Constructor
    BoosterPedal(uint8_t button_pin, MackieShowbox& showbox, entity_id entity, float value1, float value2);

    // Initialization method
    void begin();

    // Update method, to be called in the loop
    void tick();

private:
    // Static callback function
    static void toggleCallback(void *instance);

    // Instance method that the callback will call
    void handleToggle();

    OneButton button;
    MackieShowbox& showbox;
    entity_id entity;
    float value1;
    float value2;
};

#endif // BOOSTER_PEDAL_H
