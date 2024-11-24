#ifndef SNAPSHOT_LOADER_H
#define SNAPSHOT_LOADER_H

#include <Arduino.h>
#include <OneButton.h>
#include "MackieShowbox/MackieShowbox.h"

class SnapshotLoader {
public:
    // Constructor
    SnapshotLoader(uint8_t snapshot_button_pin1, uint8_t snapshot_button_pin2, MackieShowbox& showbox);

    // Initialization method
    void begin();

    // Update method, to be called in the loop
    void tick();

    // Set the debug serial
    void setDebugSerial(Print* serial);

private:
    // Static callback functions
    static void snapshotOneCallback(void *instance);
    static void snapshotTwoCallback(void *instance);

    // Instance methods that the callbacks will call
    void handleSnapshotOne();
    void handleSnapshotTwo();

    OneButton snapshotButton1;
    OneButton snapshotButton2;
    MackieShowbox& showbox;
    Print* Debug = &Serial;
};

#endif // SNAPSHOT_LOADER_H
