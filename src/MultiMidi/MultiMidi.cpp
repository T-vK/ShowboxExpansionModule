#include "MultiMidi.h"

// Constructor
MultiMidi::MultiMidi()
    : bleServer(nullptr),
      appleMidiServer(nullptr),
      serialStreamIn(nullptr),
      serialStreamOut(nullptr),
    //   bluetoothStreamIn(nullptr),
    //   bluetoothStreamOut(nullptr),
      Debug(&Serial),
      bleMidiEnabled(false),
    //   bleSerialMidiEnabled(false),
      hardwareMidiEnabled(false),
      appleMidiEnabled(false),
      bluetoothName("BleMidi"),
      rxPin(-1),
      txPin(-1),
      appleMidiPort(5004) {}

// Destructor
MultiMidi::~MultiMidi() {
    if (bleServer) delete bleServer;
    if (appleMidiServer) delete appleMidiServer;
    if (serialStreamIn) delete serialStreamIn;
    if (serialStreamOut) delete serialStreamOut;
    // if (bluetoothStreamIn) delete bluetoothStreamIn;
    // if (bluetoothStreamOut) delete bluetoothStreamOut;
}

// Enable BLE MIDI
void MultiMidi::enableBleMidi(const char *name) {
    bluetoothName = name; // todo: separate ble midi and ble serial names
    Debug->println("Bluetooth MIDI enabled.");
}

// Enable Serial Bluetooth MIDI
// void MultiMidi::enableBleSerialMidi() {
//     bleSerialMidiEnabled = true;
//     Debug->println("Serial Bluetooth MIDI enabled.");
// }

// Enable Hardware Serial MIDI
void MultiMidi::enableHardwareMidi(int rx, int tx) {
    hardwareMidiEnabled = true;
    rxPin = rx;
    txPin = tx;
    HardwareMidiSerial = new SoftwareSerial(rxPin, txPin);
    Debug->printf("Hardware Serial MIDI enabled on RX: %d, TX: %d\n", rxPin, txPin);
}
void MultiMidi::enableHardwareMidi(SoftwareSerial* serial) {
    hardwareMidiEnabled = true;
    HardwareMidiSerial = serial;
    Debug->println("Hardware Serial MIDI enabled on custom serial.");
}

// Enable AppleMIDI
void MultiMidi::enableAppleMidi(uint16_t port) {
    appleMidiEnabled = true;
    appleMidiPort = port;
    Debug->printf("AppleMIDI enabled on port: %d\n", port);
}

// Set Debug Output
void MultiMidi::setDebugSerial(Print *serial) {
    Debug = serial;
}

// Begin MIDI
void MultiMidi::begin(MidiCallbackAction &midiAction) {
    action = midiAction;

    // Initialize BLE MIDI if enabled
    if (bleMidiEnabled && !bleServer) {
        bleServer = new MidiBleServer(bluetoothName, &action);
        bleServer->begin();
        Debug->println("Bluetooth MIDI initialized.");
    }

    // Initialize Serial Bluetooth MIDI if enabled
    // if (bleSerialMidiEnabled && !bluetoothStreamIn && !bluetoothStreamOut) {
    //     bluetoothStreamIn = new MidiStreamIn(SerialBT, action);
    //     bluetoothStreamOut = new MidiStreamOut(SerialBT);
    //     SerialBT.begin(bluetoothName);
    //     Debug->println("Serial Bluetooth MIDI initialized.");
    // }

    // Initialize AppleMIDI if enabled
    if (appleMidiEnabled && !appleMidiServer) {
        appleMidiServer = new AppleMidiServer(&action);
        appleMidiServer->begin(appleMidiPort);
        Debug->println("AppleMIDI initialized.");
    }

    // Initialize Hardware Serial MIDI if enabled
    if (hardwareMidiEnabled && !serialStreamIn && !serialStreamOut) {
        HardwareMidiSerial->begin(31250);
        serialStreamIn = new MidiStreamIn(*HardwareMidiSerial, action);
        serialStreamOut = new MidiStreamOut(*HardwareMidiSerial);
        Debug->println("Hardware Serial MIDI initialized.");
    }

    Debug->println("MultiMidi setup complete.");
}

// Process incoming MIDI messages for all active interfaces
void MultiMidi::loop() {
    if (serialStreamIn) {
        serialStreamIn->loop();
    }
    if (appleMidiServer) {
        appleMidiServer->loop();
    }
    // if (bluetoothStreamIn) {
    //     bluetoothStreamIn->loop();
    // }
    // if (bleServer) {
    //    // Happens automatically
    // }
}

// writeData override
void MultiMidi::writeData(MidiMessage *msg, int len){
    if (serialStreamOut) { 
        serialStreamOut->write(msg, len);
    }
    // if (bluetoothStreamOut) {
    //     bluetoothStreamOut->writeData(msg, len);
    // }
    if (bleServer) {
        bleServer->write(msg, len);
    }
    if (appleMidiServer) {
        appleMidiServer->write(msg, len);
    }
}