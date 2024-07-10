#ifndef TWO_BUTTON_LOOPER_H
#define TWO_BUTTON_LOOPER_H

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

class TwoButtonLooper {
public:
    // Constructor
    TwoButtonLooper(uint8_t record_button_pin, uint8_t stop_button_pin, MackieShowbox& showbox);

    // Initialization method
    void begin();

    // Update method, to be called in the loop
    void tick();

private:
    // Static callback functions
    static void recordOverdubPlayCallback(void *instance);
    static void stopCallback(void *instance);
    static void deleteCallback(void *instance);

    // Instance methods that the callbacks will call
    void handleRecordOverdubPlay();
    void handleStop();
    void handleDelete();

    OneButton recordButton;
    OneButton stopButton;
    MackieShowbox& showbox;
};

#endif // TWO_BUTTON_LOOPER_H
