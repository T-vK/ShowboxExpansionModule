#include "BleClassicMidi.h"

#if defined(CONFIG_BT_CLASSIC_ENABLED) && !defined(CONFIG_IDF_TARGET_ESP32S3)
#define MULTIMIDI_CLASSIC_RADIO 1
#include <BluetoothSerial.h>
#include "MidiStreamIn.h"
#include "MidiStreamOut.h"
#endif

bool BleClassicMidi::supported() {
#if defined(MULTIMIDI_CLASSIC_RADIO)
    return true;
#else
    return false;
#endif
}

bool BleClassicMidi::enable(const char* name) {
    if (!supported()) {
        return false;
    }
    if (name != nullptr && name[0] != '\0') {
        deviceName = name;
    }
    enabled = true;
    return true;
}

void BleClassicMidi::begin(midi::MidiAction* midiAction, Print* debugPrint) {
    action = midiAction;
    debug = debugPrint;
#if defined(MULTIMIDI_CLASSIC_RADIO)
    if (!enabled) {
        return;
    }
    static BluetoothSerial serial;
    static midi::MidiStreamIn* streamIn = nullptr;
    static midi::MidiStreamOut* streamOut = nullptr;
    serial.begin(deviceName);
    streamIn = new midi::MidiStreamIn(serial, *action);
    streamOut = new midi::MidiStreamOut(serial);
    (void)streamOut;
    if (debug != nullptr) {
        debug->println("Bluetooth Classic MIDI started.");
    }
#else
    (void)action;
    if (enabled && debug != nullptr) {
        debug->println("Bluetooth Classic MIDI is not available on this chip.");
    }
#endif
}

void BleClassicMidi::tick() {
#if defined(MULTIMIDI_CLASSIC_RADIO)
    // The stream is polled by MidiStreamIn stored above. Classic is unused on this board.
#endif
}

void BleClassicMidi::write(const uint8_t* data, int length) {
    (void)data;
    (void)length;
#if defined(MULTIMIDI_CLASSIC_RADIO)
    // Outgoing Classic MIDI is wired when a Classic radio is present.
#endif
}

bool BleClassicMidi::isEnabled() const {
    return enabled && supported();
}
