#include "BoosterPedal.h"
#define SHOWBOX_DEBUG

// Constructor
BoosterPedal::BoosterPedal(uint8_t button_pin, MackieShowbox& showbox, entity_id entity, float value1, float value2)
    : button(button_pin, true), showbox(showbox), entity(entity), value1(value1), value2(value2) {}

// Initialization
void BoosterPedal::begin() {
    // Attach static callback function with the instance pointer
    button.attachClick(toggleCallback, this);
}

// Update loop
void BoosterPedal::tick() {
    button.tick();
}

// Static callback function
void BoosterPedal::toggleCallback(void *instance) {
    BoosterPedal *pedal = static_cast<BoosterPedal*>(instance);
    pedal->handleToggle();
}

// Instance method
void BoosterPedal::handleToggle() {
    float current_value = showbox.getFloatEntityValue(entity);
    
    if (current_value == value1) {
        showbox.setEntityValue(entity, value2);
    } else {
        showbox.setEntityValue(entity, value1);
    }

    #ifdef SHOWBOX_DEBUG
    Serial.print("Toggled entity ");
    Serial.print(static_cast<int>(entity));
    Serial.print(" to value ");
    Serial.println(showbox.getFloatEntityValue(entity));
    #endif
}
