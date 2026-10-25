#include "MultiMidi.h"
#include "../System/Diag.h"
#include "UsbMidiHost.h"
#include "MultiMidi/BleMidiDecode.h"
#include "MultiMidi/FullMidiParser.h"
#include "Actions/ActionRules.h"
#include "DebugLog.h"
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ETH.h>
#include <ESPmDNS.h>
#include <AsyncUDP.h>
#include "WiredNet/WiredNet.h"
#include <esp_heap_caps.h>

namespace {
// WiFiUDP uses BSD sockets. Two extra sockets here used to exhaust lwIP
// netconns so HTTP/telnet timed out while ping still worked. AsyncUDP talks
// to a raw UDP pcb and leaves those sockets for TCP.
struct AppleMidiPkt {
    uint8_t ip[4];
    uint16_t port;
    uint16_t len;
    uint8_t isData;
    uint8_t data[512];
};

static constexpr int kAppleMidiQ = 8;
AppleMidiPkt* appleQ = nullptr;
volatile uint8_t appleQHead = 0;
volatile uint8_t appleQTail = 0;
portMUX_TYPE appleQMux = portMUX_INITIALIZER_UNLOCKED;
AsyncUDP appleControl;
AsyncUDP appleData;
uint16_t appleControlPort = 5004;
FullMidiParser appleParser;
volatile bool appleParsing = false;
volatile uint8_t appleInData = 0;
const char* inboundSourceName = "ext";

void enqueueApplePkt(AsyncUDPPacket& packet, uint8_t isData) {
    if (appleQ == nullptr) {
        return;
    }
    size_t n = packet.length();
    if (n == 0 || packet.data() == nullptr) {
        return;
    }
    if (n > sizeof(AppleMidiPkt::data)) {
        n = sizeof(AppleMidiPkt::data);
    }
    portENTER_CRITICAL(&appleQMux);
    uint8_t next = static_cast<uint8_t>((appleQHead + 1) % kAppleMidiQ);
    if (next == appleQTail) {
        portEXIT_CRITICAL(&appleQMux);
        return;
    }
    AppleMidiPkt& item = appleQ[appleQHead];
    IPAddress ip = packet.remoteIP();
    item.ip[0] = ip[0];
    item.ip[1] = ip[1];
    item.ip[2] = ip[2];
    item.ip[3] = ip[3];
    item.port = packet.remotePort();
    item.len = static_cast<uint16_t>(n);
    item.isData = isData;
    memcpy(item.data, packet.data(), n);
    appleQHead = next;
    portEXIT_CRITICAL(&appleQMux);
}

void drainAppleMidi() {
    if (appleQ == nullptr) {
        return;
    }
    for (;;) {
        AppleMidiPkt item;
        portENTER_CRITICAL(&appleQMux);
        if (appleQTail == appleQHead) {
            portEXIT_CRITICAL(&appleQMux);
            return;
        }
        item = appleQ[appleQTail];
        appleQTail = static_cast<uint8_t>((appleQTail + 1) % kAppleMidiQ);
        portEXIT_CRITICAL(&appleQMux);
        appleInData = item.isData;
        appleParsing = true;
        applemidi_parse_udp_datagram(item.ip, item.port, item.data, item.len, item.isData);
        appleParsing = false;
    }
}

int32_t appleMidiSend(uint8_t* ip_addr, uint16_t port, uint8_t* tx_data, size_t tx_len) {
    if (ip_addr == nullptr || tx_data == nullptr || tx_len == 0) {
        return 0;
    }
    IPAddress dest(ip_addr[0], ip_addr[1], ip_addr[2], ip_addr[3]);
    // Session replies must leave from the pcb that received the packet.
    // Application MIDI (outside parse) is RTP and uses the data pcb.
    AsyncUDP& udp = (appleParsing ? appleInData : 1) ? appleData : appleControl;
    return static_cast<int32_t>(udp.writeTo(tx_data, tx_len, dest, port));
}

void appleMidiReceived(uint8_t /*port*/, uint32_t /*timestamp*/, uint8_t midi_status,
    uint8_t* remaining_message, size_t len, size_t /*continued_sysex_pos*/) {
    uint8_t message[17];
    message[0] = midi_status;
    size_t n = len < 16 ? len : 16;
    if (remaining_message != nullptr && n > 0) {
        memcpy(message + 1, remaining_message, n);
    }
    appleParser.parse(message, static_cast<uint8_t>(n + 1));
}

bool listenAppleMidi(uint16_t port, midi::MidiAction* midiAction) {
    if (appleQ == nullptr) {
        appleQ = static_cast<AppleMidiPkt*>(heap_caps_malloc(sizeof(AppleMidiPkt) * kAppleMidiQ, MALLOC_CAP_SPIRAM));
        if (appleQ == nullptr) {
            appleQ = static_cast<AppleMidiPkt*>(heap_caps_malloc(sizeof(AppleMidiPkt) * kAppleMidiQ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (appleQ == nullptr) {
            return false;
        }
    }
    appleControlPort = port;
    appleQHead = 0;
    appleQTail = 0;
    appleParser.begin(midiAction, -1);
    if (!appleControl.listen(port)) {
        return false;
    }
    if (!appleData.listen(static_cast<uint16_t>(port + 1))) {
        appleControl.close();
        return false;
    }
    appleControl.onPacket([](AsyncUDPPacket& packet) { enqueueApplePkt(packet, 0); });
    appleData.onPacket([](AsyncUDPPacket& packet) { enqueueApplePkt(packet, 1); });
    applemidi_init(reinterpret_cast<void*>(appleMidiReceived), reinterpret_cast<void*>(appleMidiSend));
    return true;
}

class ExposedMidiBleServer : public midi::MidiBleServer {
public:
    using midi::MidiBleServer::MidiBleServer;
    BLEServer* server() const { return pServer; }

    void writeMidi(midi::MidiMessage* pMsg, int len) {
        if (pCharacteristic == nullptr || pMsg == nullptr || len <= 0) {
            return;
        }
        midi::MidiMessage packet = *pMsg;
        updateTimestamp(&packet);
        size_t n = (len >= 2) ? sizeof(midi::MidiMessage) : sizeof(midi::MidiMessage) - 1;
        pCharacteristic->setValue(reinterpret_cast<uint8_t*>(&packet), n);
        pCharacteristic->notify();
    }

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
            pCharacteristic->setCallbacks(new BleMidiDecode(pMidiAction));
        }
        pCharacteristic->addDescriptor(new BLE2902());
        pCharacteristic->setBroadcastProperty(true);
        service->start();
    }
};
}

const char* midiInboundSource() {
    return inboundSourceName != nullptr ? inboundSourceName : "ext";
}

void midiInboundSetSource(const char* name) {
    inboundSourceName = (name != nullptr && name[0] != '\0') ? name : "ext";
}

const char* midiKindFromStatus(uint8_t status) {    if (status >= 0xF0) {
        switch (status) {
            case 0xF0: return "sysex";
            case 0xF1: return "timeCode";
            case 0xF2: return "songPosition";
            case 0xF3: return "songSelect";
            case 0xF6: return "tuneRequest";
            case 0xF8: return "clock";
            case 0xFA: return "start";
            case 0xFB: return "continue";
            case 0xFC: return "stop";
            case 0xFE: return "activeSense";
            case 0xFF: return "reset";
            default: return "system";
        }
    }
    switch (status & 0xF0) {
        case 0x80: return "noteOff";
        case 0x90: return "noteOn";
        case 0xA0: return "polyAftertouch";
        case 0xB0: return "cc";
        case 0xC0: return "pc";
        case 0xD0: return "aftertouch";
        case 0xE0: return "pitchBend";
        default: return "raw";
    }
}

void midiInboundDispatch(midi::MidiAction* action, uint8_t status, uint8_t p1, uint8_t p2) {
    uint8_t kind = status >> 4;
    uint8_t channel = status & 0x0F;
    if (action != nullptr) {
        if (kind == 0xB) {
            action->onControlChange(channel, p1, p2);
            return;
        }
        if (kind == 0x9) {
            if (p2 == 0) {
                action->onNoteOff(channel, p1, p2);
            } else {
                action->onNoteOn(channel, p1, p2);
            }
            return;
        }
        if (kind == 0x8) {
            action->onNoteOff(channel, p1, p2);
            return;
        }
        if (kind == 0xE) {
            action->onPitchBend(channel, p1);
            return;
        }
    }
    actionRulesOnMidi(midiKindFromStatus(status), channel, p1, p2);
}

void MultiMidi::testDispatch(uint8_t status, uint8_t p1, uint8_t p2) {
    if (instance == nullptr) {
        return;
    }
    midiInboundSetSource("test");
    midiInboundDispatch(&instance->action, status, p1, p2);
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
    Debug->printf("Hardware Serial MIDI pins RX: %d, TX: %d (starts only if DIN is on)\n", rxPin, txPin);
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
    dinEnabled = prefs.getBool("din", false);
    wifiEnabled = prefs.getBool("wifi", true);
    bleAdvertise = prefs.getBool("bleadv", true);
    bleCentral = prefs.getBool("blecent", true);
    autoConnectEnabled = prefs.getBool("auto", true);
    ethernetEnabled = prefs.getBool("eth", true);
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

void MultiMidi::applyDin() {
    if (!hardwareMidiEnabled) {
        return;
    }
    if (dinEnabled) {
        if (HardwareMidiSerial == nullptr) {
            HardwareMidiSerial = new SoftwareSerial(rxPin, txPin);
        }
        HardwareMidiSerial->enableRxGPIOPullUp(true);
        if (serialStreamIn == nullptr || serialStreamOut == nullptr) {
            HardwareMidiSerial->begin(31250);
            serialStreamIn = new MidiStreamIn(*HardwareMidiSerial, action);
            serialStreamOut = new MidiStreamOut(*HardwareMidiSerial);
            Debug->println("DIN SoftwareSerial started.");
        } else {
            HardwareMidiSerial->enableRx(true);
        }
        return;
    }
    if (HardwareMidiSerial != nullptr) {
        HardwareMidiSerial->enableRx(false);
        HardwareMidiSerial->end();
        Debug->println("DIN SoftwareSerial stopped.");
    }
}

void MultiMidi::applyBleSettings() {
    bleMidi.setAutoConnect(autoConnectEnabled);
    bleMidi.setAdvertising(bleAdvertise);
    bleMidi.setCentral(bleCentral);
}

void MultiMidi::setHostPolicy(BleMidiHostPolicy* policy) {
    bleMidi.setPolicy(policy);
}

void MultiMidi::setRadio(BleRadio* radio) {
    bleMidi.setRadio(radio);
}

void MultiMidi::setEthernetPins(uint8_t sclk, uint8_t miso, uint8_t mosi, uint8_t cs, uint8_t reset) {
    ethernet.setPins(sclk, miso, mosi, cs, reset);
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
    bleMidi.setCentral(true);
    bleMidi.connectAddress(address);
}

void MultiMidi::holdHost(const char* address) {
    bleMidi.holdAddress(address);
}

void MultiMidi::releaseHost(const char* address) {
    bleMidi.releaseAddress(address);
}

void MultiMidi::dropHostLink() {
    bleMidi.dropLink();
}

void MultiMidi::dropHost(const char* address) {
    bleMidi.dropAddress(address);
}

BLEServer* MultiMidi::bleGattServer() const {
    if (bleServer == nullptr) {
        return nullptr;
    }
    return static_cast<ExposedMidiBleServer*>(bleServer)->server();
}

String MultiMidi::midiStatusJson() {
    bool wifiReady = wifiEnabled && appleMidiStarted && WiFi.status() == WL_CONNECTED;
    bool ethReady = ethernetEnabled && appleMidiStarted && ETH.hasIP();
    String json = "{";
    json += "\"din\":{\"enabled\":" + String(jsonBool(dinEnabled)) + ",\"available\":true,\"label\":\"5-pin MIDI\"},";
    json += "\"wifi\":{\"enabled\":" + String(jsonBool(wifiEnabled)) + ",\"available\":true,\"ready\":" + String(jsonBool(wifiReady)) + ",\"label\":\"Wi-Fi Network MIDI\",\"port\":5004},";
    json += "\"ethernet\":{\"enabled\":" + String(jsonBool(ethernetEnabled)) + ",\"available\":true,\"ready\":" + String(jsonBool(ethReady)) + ",\"label\":\"Ethernet MIDI\",\"note\":\"" + jsonEscape(ethernet.status()) + "\"},";
    json += "\"usbDevice\":{\"enabled\":false,\"available\":false,\"label\":\"USB device MIDI\",\"reason\":\"USB MIDI would replace the serial port used to debug and flash.\"},";
    json += "\"usbHost\":{\"enabled\":false,\"available\":false,\"label\":\"USB host MIDI\",\"reason\":\"USB host MIDI is not implemented.\"},";
    json += "\"bleAdvertise\":{\"enabled\":" + String(jsonBool(bleMidi.advertising())) + ",\"available\":true,\"name\":\"" + jsonEscape(bluetoothName) + "\"},";
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
        applyDin();
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

void MultiMidi::allowOutgoing(bool enabled) {
    bleMidi.allowOutgoing(enabled);
}

void MultiMidi::disconnectBle() {
    bleCentral = false;
    saveSettings();
    bleMidi.disconnect();
}

// Begin MIDI
void MultiMidi::begin() {
    loadSettings();
    {
        // Older builds began SoftwareSerial at boot even with no DIN gear.
        // That bit-bang ISR wedges mixer↔base. One-time: DIN stays off until
        // the user turns it on.
        Preferences prefs;
        if (prefs.begin("midi", false)) {
            if (!prefs.getBool("dinlazy", false)) {
                dinEnabled = false;
                prefs.putBool("din", false);
                prefs.putBool("dinlazy", true);
            }
            prefs.end();
        }
    }
    action.setCallbackOnControlChange(MultiMidi::onControlChange);
    action.setCallbackOnNoteOn(MultiMidi::onNoteOn);
    action.setCallbackOnNoteOff(MultiMidi::onNoteOff);
    action.setCallbackOnPitchBend(MultiMidi::onPitchBend);

    classic.begin(&action, Debug);
    ethernet.setDebug(Debug);
    ethernet.setEnabled(ethernetEnabled);

    // Bind AppleMIDI before BLE, with AsyncUDP so HTTP keeps its TCP sockets.
    tryStartAppleMidi();

    // Initialize BLE MIDI if enabled
    if (bleMidiEnabled && !bleServer) {
        bleServer = new ExposedMidiBleServer(bluetoothName, &action);
        static_cast<ExposedMidiBleServer*>(bleServer)->beginServices();
    }

    applyDin();
    
    Debug->println("MultiMidi setup complete.");
}

void MultiMidi::tryStartAppleMidi() {
    if (!appleMidiEnabled || appleMidiStarted) {
        return;
    }
    if (!lanHasIp()) {
        return;
    }
    appleMidiStarted = listenAppleMidi(appleMidiPort, &action);
    debugLog("applemidi begin=%d port=%u heap=%u largest=%u",
        appleMidiStarted ? 1 : 0,
        (unsigned)appleMidiPort,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (appleMidiStarted && Debug != nullptr) {
        Debug->println("AppleMIDI initialized.");
    }
}

// Process incoming MIDI messages for all active interfaces
void MultiMidi::tick() {
    if (dinEnabled && serialStreamIn) {
        midiInboundSetSource("din");
        serialStreamIn->loop();
    }
    if (appleMidiEnabled) {
        if (!appleMidiStarted) {
            tryStartAppleMidi();
        }
        if (appleMidiStarted) {
            midiInboundSetSource("apple");
            drainAppleMidi();
        }
    }
    if (bleMidiEnabled) {
        midiInboundSetSource("ble");
        bleMidi.tick();
    }
    if (ethernetEnabled) {
        midiInboundSetSource("eth");
        ethernet.tick(&action);
    } else {
        ethernet.tick(nullptr);
    }
    if (UsbMidi::running()) {
        midiInboundSetSource("usb");
        UsbMidi::tick(&action);
    }
    if (UsbMidiHost::running()) {
        midiInboundSetSource("usbhost");
        UsbMidiHost::tick(&action);
    }
    midiInboundSetSource("ext");
    watchOutputLinks();
    // if (bluetoothStreamIn) {
    //     bluetoothStreamIn->loop();
    // }
    // if (bleServer) {
    //    // Happens automatically
    // }
}

void MultiMidi::watchOutputLinks() {
    uint32_t bleCentral = static_cast<uint32_t>(bleMidi.connectedCount());
    uint32_t blePeriph = (bleServer != nullptr && bleServer->getConnectionStatus() == midi::Connected) ? 1u : 0u;
    uint32_t apple = 0;
    if (appleMidiStarted) {
        for (uint8_t port = 1; port < APPLEMIDI_MAX_PEERS; port++) {
            applemidi_peer_t* peer = applemidi_peer_get_info(port);
            if (peer != nullptr && peer->ssrc != 0) {
                apple++;
            }
        }
    }
    uint32_t din = (dinEnabled && serialStreamOut != nullptr) ? 1u : 0u;
    uint32_t usbHost = UsbMidiHost::connected() ? 1u : 0u;
    uint32_t usbDev = UsbMidi::mounted() ? 1u : 0u;
    uint32_t sig = bleCentral
        | (blePeriph << 4)
        | (apple << 8)
        | (din << 12)
        | (usbHost << 13)
        | (usbDev << 14);
    if (sig == outSignature) {
        return;
    }
    uint32_t old = outSignature;
    outSignature = sig;
    bool gained = bleCentral > (old & 0xFu)
        || blePeriph > ((old >> 4) & 0xFu)
        || apple > ((old >> 8) & 0xFu)
        || din > ((old >> 12) & 0x1u)
        || usbHost > ((old >> 13) & 0x1u)
        || usbDev > ((old >> 14) & 0x1u);
    if (gained) {
        outGeneration++;
    }
}

void MultiMidi::sendAppleMidi(MidiMessage* msg, int len) {
    if (msg == nullptr || len <= 0 || !appleMidiStarted) {
        return;
    }
    uint8_t bytes[3] = {msg->status, msg->arg1, msg->arg2};
    size_t n = (len >= 2) ? 3 : (len >= 1 ? 2 : 1);
    for (uint8_t port = 1; port < APPLEMIDI_MAX_PEERS; port++) {
        applemidi_peer_t* peer = applemidi_peer_get_info(port);
        if (peer == nullptr || peer->ssrc == 0) {
            continue;
        }
        applemidi_send_message(port, bytes, n);
        applemidi_outbuffer_flush(port);
    }
}

// writeData override
void MultiMidi::writeData(MidiMessage *msg, int len){
    writeData(msg, len, kMidiPortAll);
}

void MultiMidi::writeData(MidiMessage *msg, int len, uint16_t ports){
    if (msg == nullptr || len <= 0) {
        return;
    }
    diagNoteMidiOut((size_t)len + 1);
    if (midiPortWanted(ports, kMidiPortDin) && dinEnabled && serialStreamOut) {
        serialStreamOut->write(msg, len);
    }
    if (midiPortWanted(ports, kMidiPortBle) && bleServer) {
        static_cast<ExposedMidiBleServer*>(bleServer)->writeMidi(msg, len);
    }
    if (midiPortWanted(ports, kMidiPortBle) && bleMidiEnabled) {
        bleMidi.write(msg, len);
    }
    if (midiPortWanted(ports, kMidiPortApple)) {
        sendAppleMidi(msg, len);
    }
    if (midiPortWanted(ports, kMidiPortEth) && ethernetEnabled) {
        ethernet.write(msg, len);
    }
    if (midiPortWanted(ports, kMidiPortUsb) && UsbMidi::running()) {
        UsbMidi::write(msg, len);
    }
    if (midiPortWanted(ports, kMidiPortUsbHost) && UsbMidiHost::running()) {
        UsbMidiHost::write(msg, len);
    }
}

void MultiMidi::writeRaw(uint8_t status, uint8_t d1, uint8_t d2, uint16_t ports) {
    MidiMessage msg = {};
    msg.status = status;
    msg.arg1 = d1;
    msg.arg2 = d2;
    int len = 2;
    uint8_t kind = status & 0xF0;
    if (kind == 0xC0 || kind == 0xD0 || status >= 0xF8) {
        len = 1;
        if (status >= 0xF8 && status != 0xF1 && status != 0xF3) {
            len = 0;
        }
    }
    writeData(&msg, len == 0 ? 1 : len, ports);
}

void MultiMidi::writeBytes(const uint8_t* data, size_t n, uint16_t ports) {
    if (data == nullptr || n == 0) {
        return;
    }
    size_t i = 0;
    while (i < n) {
        MidiMessage msg = {};
        msg.status = data[i];
        size_t left = n - i;
        int len = 1;
        if (left >= 2) {
            msg.arg1 = data[i + 1];
            len = 1;
        }
        if (left >= 3) {
            msg.arg2 = data[i + 2];
            len = 2;
        }
        if (left == 1) {
            uint8_t status = data[i];
            if (status >= 0xF8 && status != 0xF1 && status != 0xF3) {
                writeRaw(status, 0, 0, ports);
                i += 1;
                continue;
            }
            writeData(&msg, 1, ports);
            break;
        }
        writeData(&msg, len, ports);
        i += 1 + (size_t)len;
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