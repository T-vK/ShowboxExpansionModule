#ifndef MULTIMIDI_H
#define MULTIMIDI_H

#include "MidiCommon.h"
#include "Midi.h"
//#include <BluetoothSerial.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <SoftwareSerial.h>

class MultiMidi : public MidiCommon {
public:
    // Constructor
    MultiMidi();

    // Destructor
    ~MultiMidi();

    // Enable Bluetooth MIDI (call before begin)
    void enableBleMidi(const char *bluetoothName = "BleMidi");

    // Enable Serial Bluetooth MIDI (call before begin)
    //void enableBleSerialMidi();

    // Enable Hardware Serial MIDI (call before begin)
    void enableHardwareMidi(int rxPin, int txPin);
    void enableHardwareMidi(SoftwareSerial* serial);

    // Enable AppleMIDI (call before begin)
    void enableAppleMidi(uint16_t port = 5004);

    // Begin MIDI with a given MidiCallbackAction
    void begin(MidiCallbackAction &midiAction);

    // Process incoming MIDI messages for all active interfaces
    void loop();

    // Write MIDI data to all active interfaces
    void writeData(MidiMessage *msg, int len);

    // Set the debug output
    void setDebugSerial(Print *serial);

private:
    MidiCallbackAction action;
    MidiBleServer *bleServer;
    AppleMidiServer *appleMidiServer;
    MidiStreamIn *serialStreamIn;
    MidiStreamOut *serialStreamOut;
    //MidiStreamIn *bluetoothStreamIn;
    //MidiStreamOut *bluetoothStreamOut;
    //BluetoothSerial SerialBT;
    SoftwareSerial* HardwareMidiSerial;
    Print *Debug = &Serial;

    bool bleMidiEnabled;
    // bool bleSerialMidiEnabled;
    bool hardwareMidiEnabled;
    bool appleMidiEnabled;

    const char *bluetoothName;
    int rxPin, txPin;
    uint16_t appleMidiPort;
};

#endif // MULTIMIDI_H