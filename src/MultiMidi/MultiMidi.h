#ifndef MULTIMIDI_H
#define MULTIMIDI_H

#include "MidiCommon.h"
#include "Midi.h"
#include "BleMidi.h"
#include "EthernetMidi.h"
#include "MultiMidi/BleClassicMidi.h"
#include "MultiMidi/BleMidiHostPolicy.h"
#include "UsbMidi.h"
#include "MidiPorts.h"
//#include <BluetoothSerial.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <SoftwareSerial.h>

class BLEServer;

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

    // Bluetooth Classic SPP MIDI. Returns false on ESP32-S3, which has no Classic radio.
    bool enableBleClassicMidi(const char* name = "BleMidi");
    void setHostPolicy(BleMidiHostPolicy* policy);
    void setRadio(class BleRadio* radio);
    void setEthernetPins(uint8_t sclk, uint8_t miso, uint8_t mosi, uint8_t cs, uint8_t reset);

    // Set MIDI Action
    // void setMidiAction(MidiCallbackAction midiAction);

    // Begin MIDI with a given MidiCallbackAction
    void begin();
    void finishBle();
    void allowOutgoing(bool enabled);

    // Saved transport switches. USB and Ethernet cannot be turned on.
    void loadSettings();
    String midiStatusJson();
    String bleDevicesJson();
    bool setMidiOption(const String& name, bool enabled, String& error);
    void requestBleScan();
    bool connectBle(const String& address, String& error);
    void disconnectBle();
    void setAutoConnect(bool enabled, bool persist);
    bool autoConnect() const { return autoConnectEnabled; }
    bool dinIsEnabled() const { return dinEnabled; }
    bool wifiIsEnabled() const { return wifiEnabled; }
    bool bleHostEnabled() const { return bleMidi.central(); }
    bool blePeripheralEnabled() const { return bleMidi.advertising(); }
    bool bleScanning() const { return bleMidi.isScanning(); }
    bool hostConnected() const { return bleMidi.isConnected(); }
    const char* hostAddress() const { return bleMidi.connectedAddress(); }
    const char* hostName() const { return bleMidi.connectedName(); }
    int copyBleScan(BleMidi::SeenDevice* out, int max) const { return bleMidi.copyDevices(out, max); }
    int copyOutgoing(BleMidi::Outgoing* out, int max) const { return bleMidi.copyOutgoing(out, max); }
    bool addressConnected(const char* address) const { return bleMidi.addressConnected(address); }
    int outgoingCount() const { return bleMidi.connectedCount(); }
    void setMaxOutgoing(int n) { bleMidi.setMaxOutgoing(n); }
    int hostRssi() const { return bleMidi.connectedRssi(); }
    bool hostRssiKnown() const { return bleMidi.connectedRssiKnown(); }
    int hostBattery() const { return bleMidi.connectedBattery(); }
    void connectHost(const char* address);
    void holdHost(const char* address);
    void dropHostLink();
    void dropHost(const char* address);
    void releaseHost(const char* address);
    bool ethernetIsEnabled() const { return ethernetEnabled; }
    bool usbDeviceIsEnabled() const { return usbDeviceEnabled; }
    void rememberUsbDevice(bool enabled) { usbDeviceEnabled = enabled; }
    const char* ethernetNote() const { return ethernet.status(); }
    BLEServer* bleGattServer() const;

    // Process incoming MIDI messages for all active interfaces
    void tick();

    // Write MIDI data to enabled interfaces. ports==0 sends to every live output.
    void writeData(MidiMessage *msg, int len);
    void writeData(MidiMessage *msg, int len, uint16_t ports);
    void writeRaw(uint8_t status, uint8_t d1, uint8_t d2, uint16_t ports = 0);
    void writeBytes(const uint8_t* data, size_t n, uint16_t ports = 0);

    // Bumps when a MIDI output appears (BLE pedal, BLE phone, DIN, AppleMIDI, USB).
    uint32_t outputGeneration() const { return outGeneration; }

    // Set the debug output
    void setDebugSerial(Print *serial);

    // Callback methods
    MidiCallbackAction action;

private:
    MidiBleServer *bleServer;
    BleMidi bleMidi;
    AppleMidiServer *appleMidiServer;
    MidiStreamIn *serialStreamIn;
    MidiStreamOut *serialStreamOut;
    //MidiStreamIn *bluetoothStreamIn;
    //MidiStreamOut *bluetoothStreamOut;
    //BluetoothSerial SerialBT;
    SoftwareSerial *HardwareMidiSerial;
    Print *Debug = &Serial;

    bool bleMidiEnabled;
    // bool bleSerialMidiEnabled;
    bool hardwareMidiEnabled;
    bool appleMidiEnabled;
    bool dinEnabled = false;
    bool wifiEnabled = true;
    bool bleAdvertise = true;
    bool bleCentral = true;
    bool autoConnectEnabled = true;
    bool ethernetEnabled = true;
    bool usbDeviceEnabled = false;
    bool appleMidiStarted = false;
    BleClassicMidi classic;
    EthernetMidi ethernet;
    char bleTargetAddress[18] = "";

    void saveSettings();
    void applyBleSettings();
    void applyDin();
    void tryStartAppleMidi();
    void watchOutputLinks();
    void sendAppleMidi(MidiMessage* msg, int len);

    uint32_t outGeneration = 0;
    uint32_t outSignature = 0;

    const char *bluetoothName;
    int rxPin, txPin;
    uint16_t appleMidiPort;

    // Callback methods
    static void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    static void onControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    static void onPitchBend(uint8_t channel, uint8_t value);
    
    static MultiMidi* instance;
};

#endif // MULTIMIDI_H