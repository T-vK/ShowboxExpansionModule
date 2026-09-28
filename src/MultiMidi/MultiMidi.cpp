#include "MultiMidi.h"
#include "UsbMidiHost.h"
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <Preferences.h>
#include <WiFi.h>

namespace {
class ExposedMidiBleServer : public midi::MidiBleServer {
public:
    using midi::MidiBleServer::MidiBleServer;
    BLEServer* server() const { return pServer; }

    // Same startup as MidiBleServer::begin, without advertising. Advertising
    // starts the GATT database, so HID has to be added before that.
    void beginServices() {
        BLEDevice::init(name);
        pServer = BLEDevice::createServer();
        pServer->setCallbacks(new midi::MidiBleServerCallback(&connectionStatus));
        BLEService* service = pServer->createService(BLEUUID(MIDI_SERVICE_UUID));
        pCharacteristic = service->createCharacteristic(
            BLEUUID(MIDI_CHARACTERISTIC_UUID),
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR |
                BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE);
        if (pMidiAction != nullptr) {
            if (pEventHandler == nullptr) {
                pEventHandler = new midi::MidiBleParser(pMidiAction, receivingChannel);
            }
            pCharacteristic->setCallbacks(pEventHandler);
        }
        pCharacteristic->addDescriptor(new BLE2902());
        pCharacteristic->setBroadcastProperty(true);
        service->start();
    }
};
}

MultiMidi* MultiMidi::instance = nullptr;

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
      appleMidiPort(5004) {
        instance = this;
      }

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
    bleMidiEnabled = true;
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

// Set MIDI ActionwriteData
// void MultiMidi::setMidiAction(MidiCallbackAction midiAction) {
//     action = &midiAction;
// }

namespace {
String jsonEscape(const char* text) {
    String out;
    if (text == nullptr) {
        return out;
    }
    for (const char* p = text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
        }
        if (*p >= 32) {
            out += *p;
        }
    }
    return out;
}

const char* jsonBool(bool value) {
    return value ? "true" : "false";
}
}

void MultiMidi::loadSettings() {
    Preferences prefs;
    if (!prefs.begin("midi", true)) {
        return;
    }
    dinEnabled = prefs.getBool("din", true);
    wifiEnabled = prefs.getBool("wifi", true);
    bleAdvertise = prefs.getBool("bleadv", true);
    bleCentral = prefs.getBool("blecent", true);
    autoConnectEnabled = prefs.getBool("auto", true);
    ethernetEnabled = prefs.getBool("eth", false);
    usbDeviceEnabled = prefs.getBool("usbdev", false);
    String address = prefs.getString("bleaddr", "");
    strncpy(bleTargetAddress, address.c_str(), sizeof(bleTargetAddress) - 1);
    bleTargetAddress[sizeof(bleTargetAddress) - 1] = '\0';
    prefs.end();
}

void MultiMidi::saveSettings() {
    Preferences prefs;
    if (!prefs.begin("midi", false)) {
        Debug->println("Failed to save MIDI settings");
        return;
    }
    prefs.putBool("din", dinEnabled);
    prefs.putBool("wifi", wifiEnabled);
    prefs.putBool("bleadv", bleAdvertise);
    prefs.putBool("blecent", bleCentral);
    prefs.putBool("auto", autoConnectEnabled);
    prefs.putBool("eth", ethernetEnabled);
    prefs.putBool("usbdev", usbDeviceEnabled);
    prefs.putString("bleaddr", bleTargetAddress);
    prefs.end();
}

void MultiMidi::applyBleSettings() {
    bleMidi.setAutoConnect(autoConnectEnabled);
    bleMidi.setAdvertising(bleAdvertise);
    bleMidi.setCentral(bleCentral);
}

void MultiMidi::setHostPolicy(BleMidiHostPolicy* policy) {
    bleMidi.setPolicy(policy);
}

bool MultiMidi::enableBleClassicMidi(const char* name) {
    if (!classic.enable(name)) {
        Debug->println("Bluetooth Classic MIDI is not available on this chip.");
        return false;
    }
    Debug->println("Bluetooth Classic MIDI enabled.");
    return true;
}

void MultiMidi::setAutoConnect(bool enabled, bool persist) {
    autoConnectEnabled = enabled;
    bleMidi.setAutoConnect(enabled);
    if (persist) {
        saveSettings();
    }
}

void MultiMidi::connectHost(const char* address) {
    bleCentral = true;
    saveSettings();
    bleMidi.connectAddress(address);
}

void MultiMidi::holdHost(const char* address) {
    bleMidi.holdAddress(address);
}

void MultiMidi::dropHostLink() {
    bleMidi.dropLink();
}

BLEServer* MultiMidi::bleGattServer() const {
    if (bleServer == nullptr) {
        return nullptr;
    }
    return static_cast<ExposedMidiBleServer*>(bleServer)->server();
}

String MultiMidi::midiStatusJson() {
    bool wifiReady = wifiEnabled && appleMidiStarted && WiFi.status() == WL_CONNECTED;
    String json = "{";
    json += "\"din\":{\"enabled\":" + String(jsonBool(dinEnabled)) + ",\"available\":true,\"label\":\"5-pin MIDI\"},";
    json += "\"wifi\":{\"enabled\":" + String(jsonBool(wifiEnabled)) + ",\"available\":true,\"ready\":" + String(jsonBool(wifiReady)) + ",\"label\":\"Wi-Fi Network MIDI\",\"port\":5004},";
    json += "\"ethernet\":{\"enabled\":false,\"available\":false,\"label\":\"Ethernet MIDI\",\"reason\":\"The W5500 is not started.\"},";
    json += "\"usbDevice\":{\"enabled\":false,\"available\":false,\"label\":\"USB device MIDI\",\"reason\":\"USB MIDI would replace the serial port used to debug and flash.\"},";
    json += "\"usbHost\":{\"enabled\":false,\"available\":false,\"label\":\"USB host MIDI\",\"reason\":\"USB host MIDI is not implemented.\"},";
    json += "\"bleAdvertise\":{\"enabled\":" + String(jsonBool(bleMidi.advertising())) + ",\"available\":true,\"name\":\"ShowboxMod\"},";
    json += "\"bleCentral\":{\"enabled\":" + String(jsonBool(bleMidi.central())) + ",\"available\":true,\"connected\":" + String(jsonBool(bleMidi.isConnected()));
    json += ",\"scanning\":" + String(jsonBool(bleMidi.isScanning()));
    json += ",\"peerName\":\"" + jsonEscape(bleMidi.connectedName()) + "\"";
    json += ",\"peerAddress\":\"" + jsonEscape(bleMidi.connectedAddress()) + "\"";
    json += ",\"target\":\"" + jsonEscape(bleTargetAddress) + "\"}}";
    return json;
}

String MultiMidi::bleDevicesJson() {
    BleMidi::SeenDevice devices[BleMidi::kMaxDevices];
    int count = bleMidi.copyDevices(devices, BleMidi::kMaxDevices);
    String json = "{\"scanning\":" + String(jsonBool(bleMidi.isScanning())) + ",\"devices\":[";
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            json += ",";
        }
        json += "{\"name\":\"" + jsonEscape(devices[i].name) + "\"";
        json += ",\"address\":\"" + jsonEscape(devices[i].address) + "\"";
        json += ",\"rssi\":" + String(devices[i].rssi);
        json += ",\"midi\":" + String(jsonBool(devices[i].midi)) + "}";
    }
    json += "]}";
    return json;
}

bool MultiMidi::setMidiOption(const String& name, bool enabled, String& error) {
    if (name == "usb" || name == "usbHost") {
        error = "USB host and USB MIDI device share the one USB port, so they cannot run together. USB host MIDI is not started.";
        return false;
    }
    if (name == "din") {
        dinEnabled = enabled;
    } else if (name == "wifi") {
        wifiEnabled = enabled;
    } else if (name == "bleAdvertise") {
        bleAdvertise = enabled;
        bleMidi.setAdvertising(enabled);
    } else if (name == "bleCentral") {
        bleCentral = enabled;
        bleMidi.setCentral(enabled);
    } else if (name == "ethernet") {
        ethernetEnabled = enabled;
        ethernet.setEnabled(enabled);
    } else if (name == "usbDevice") {
        usbDeviceEnabled = enabled;
    } else {
        error = "Unknown MIDI setting.";
        return false;
    }
    saveSettings();
    return true;
}

void MultiMidi::requestBleScan() {
    bleMidi.requestScan();
}

bool MultiMidi::connectBle(const String& address, String& error) {
    if (address.length() < 11) {
        error = "Missing Bluetooth address.";
        return false;
    }
    connectHost(address.c_str());
    return true;
}

void MultiMidi::finishBle() {
    if (!bleMidiEnabled || bleServer == nullptr) {
        return;
    }
    applyBleSettings();
    bleMidi.begin(&action, Debug);
    Debug->println("Bluetooth MIDI ready.");
}

void MultiMidi::disconnectBle() {
    bleCentral = false;
    saveSettings();
    bleMidi.disconnect();
}

// Begin MIDI
void MultiMidi::begin() {
    loadSettings();
    // if (action == nullptr) { 
        // action = new MidiCallbackAction();
        // action->setCallbackOnControlChange(MultiMidi::onControlChange);
        // action->setCallbackOnNoteOn(MultiMidi::onNoteOn);
        // action->setCallbackOnNoteOff(MultiMidi::onNoteOff);
        // action->setCallbackOnPitchBend(MultiMidi::onPitchBend);
    // }
    action.setCallbackOnControlChange(MultiMidi::onControlChange);
    action.setCallbackOnNoteOn(MultiMidi::onNoteOn);
    action.setCallbackOnNoteOff(MultiMidi::onNoteOff);
    action.setCallbackOnPitchBend(MultiMidi::onPitchBend);

    // Initialize BLE MIDI if enabled
    if (bleMidiEnabled && !bleServer) {
        bleServer = new ExposedMidiBleServer(bluetoothName, &action);
        static_cast<ExposedMidiBleServer*>(bleServer)->beginServices();
    }
    classic.begin(&action, Debug);
    ethernet.setDebug(Debug);
    ethernet.setEnabled(ethernetEnabled);

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
        if (wifiEnabled && WiFi.status() == WL_CONNECTED) {
            appleMidiStarted = appleMidiServer->begin(appleMidiPort);
        }
        Debug->println(appleMidiStarted ? "AppleMIDI initialized." : "AppleMIDI waiting for Wi-Fi.");
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
void MultiMidi::tick() {
    if (dinEnabled && serialStreamIn) {
        serialStreamIn->loop();
    }
    if (wifiEnabled && appleMidiServer) {
        if (!appleMidiStarted && WiFi.status() == WL_CONNECTED) {
            appleMidiStarted = appleMidiServer->begin(appleMidiPort);
        }
        if (appleMidiStarted) {
            appleMidiServer->loop();
        }
    }
    if (bleMidiEnabled) {
        bleMidi.tick();
    }
    ethernet.tick(ethernetEnabled ? &action : nullptr);
    if (UsbMidi::running()) {
        UsbMidi::tick(&action);
    }
    if (UsbMidiHost::running()) {
        UsbMidiHost::tick(&action);
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
    if (dinEnabled && serialStreamOut) {
        serialStreamOut->write(msg, len);
    }
    // if (bluetoothStreamOut) {
    //     bluetoothStreamOut->writeData(msg, len);
    // }
    if (bleServer) {
        bleServer->write(msg, len);
    }
    if (wifiEnabled && appleMidiStarted && appleMidiServer) {
        appleMidiServer->write(msg, len);
    }
    if (ethernetEnabled) {
        ethernet.write(msg, len);
    }
    if (UsbMidi::running()) {
        UsbMidi::write(msg, len);
    }
    if (UsbMidiHost::running()) {
        UsbMidiHost::write(msg, len);
    }
}

// Callback methods
void MultiMidi::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("MIDI Note On: %d, %d, %d\n", channel, note, velocity);
}

void MultiMidi::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    instance->Debug->printf("MIDI Note Off: %d, %d, %d\n", channel, note, velocity);
}

void MultiMidi::onControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    instance->Debug->printf("MIDI Control Change: %d, %d, %d\n", channel, controller, value);
}

void MultiMidi::onPitchBend(uint8_t channel, uint8_t value) {
    instance->Debug->printf("MIDI Pitch Bend: %d, %d\n", channel, value);
}