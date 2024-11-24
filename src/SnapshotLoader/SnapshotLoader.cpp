#include "SnapshotLoader.h"

// Constructor
SnapshotLoader::SnapshotLoader(uint8_t snapshot_button_pin1, uint8_t snapshot_button_pin2, MackieShowbox& showbox)
    : snapshotButton1(snapshot_button_pin1, true), snapshotButton2(snapshot_button_pin2, true), showbox(showbox) {}

// Initialization
void SnapshotLoader::begin() {
    // Attach static callback functions with the instance pointer
    snapshotButton1.attachClick(snapshotOneCallback, this);
    snapshotButton2.attachClick(snapshotTwoCallback, this);
}

// Update loop
void SnapshotLoader::tick() {
    snapshotButton1.tick();
    snapshotButton2.tick();
}

// Static callback functions
void SnapshotLoader::snapshotOneCallback(void *instance) {
    SnapshotLoader *loader = static_cast<SnapshotLoader*>(instance);
    loader->handleSnapshotOne();
}

void SnapshotLoader::snapshotTwoCallback(void *instance) {
    SnapshotLoader *loader = static_cast<SnapshotLoader*>(instance);
    loader->handleSnapshotTwo();
}

// Instance methods
void SnapshotLoader::handleSnapshotOne() {
    showbox.snapshotAction(snapshot_action::RECALL, snapshot_slot::SNAPSHOT_1);
    //#ifdef SHOWBOX_DEBUG
    Debug->println("Recall snapshot SNAPSHOT_1");
    //#endif
}

void SnapshotLoader::handleSnapshotTwo() {
    showbox.snapshotAction(snapshot_action::RECALL, snapshot_slot::SNAPSHOT_2);
    //#ifdef SHOWBOX_DEBUG
    Debug->println("Recall snapshot SNAPSHOT_2");
    //#endif
}

// Set the debug serial
void SnapshotLoader::setDebugSerial(Print* serial) {
    Debug = serial;
}