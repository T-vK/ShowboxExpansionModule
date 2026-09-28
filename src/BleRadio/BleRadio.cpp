#include "BleRadio.h"
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <host/ble_gap.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr char kGapName[] = "ShowboxMod";
constexpr char kMidiService[] = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";

void copyAddress(const ble_addr_t& addr, char* out, size_t size) {
    BLEAddress address(addr);
    String text = address.toString();
    strncpy(out, text.c_str(), size - 1);
    out[size - 1] = '\0';
}
}

BleRadio& BleRadio::instance() {
    static BleRadio radio;
    return radio;
}

void BleRadio::setMidiPeripheral(bool enabled) {
    midiOn = enabled;
}

void BleRadio::setHidPeripheral(bool enabled) {
    hidOn = enabled;
}

bool BleRadio::midiPeripheral() const { return midiOn; }
bool BleRadio::hidPeripheral() const { return hidOn; }

void BleRadio::restartWhenIdle() {
    restartAdvertising = true;
}

void BleRadio::apply() {
    if (!BLEDevice::getInitialized()) {
        return;
    }
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    if (advertising == nullptr) {
        return;
    }
    // The library default is 20–40 ms, the shortest interval the controller
    // allows. On this chip that duty cycle leaves Wi-Fi no airtime for unicast,
    // so the page never loads. 100–200 ms is still found promptly.
    advertising->setMinInterval(0xA0);
    advertising->setMaxInterval(0x140);
    if (!midiOn && !hidOn) {
        if (advertised) {
            advertising->stop();
            advertised = false;
        }
        return;
    }
    // Rewriting the payload while an advertisement is already running fills the
    // controller (HCI 0x2008/0x2009 status 0x07) and crowds Wi-Fi off the radio.
    if (advertised && midiOn == advertisedMidi && hidOn == advertisedHid) {
        if (restartAdvertising) {
            restartAdvertising = false;
            advertising->start();
        }
        return;
    }
    if (advertised) {
        advertising->stop();
        advertised = false;
    }

    BLEAdvertisementData adv;
    adv.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    if (hidOn) {
        adv.setAppearance(0x03C0);
        adv.setPartialServices(BLEUUID((uint16_t)0x1812));
    }
    if (midiOn) {
        adv.setPartialServices(BLEUUID(kMidiService));
    }
    static bool pairingReady = false;
    if (!pairingReady) {
        BLESecurity* security = new BLESecurity();
        security->setAuthenticationMode(ESP_LE_AUTH_BOND);
        security->setCapability(ESP_IO_CAP_NONE);
        security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        pairingReady = true;
    }
    BLEAdvertisementData scan;
    scan.setName(kGapName);
    advertising->setScanResponse(true);
    advertising->setAdvertisementData(adv);
    advertising->setScanResponseData(scan);
    advertising->start();
    advertised = true;
    advertisedMidi = midiOn;
    advertisedHid = hidOn;
}

void BleRadio::service() {
    uint16_t handle = 0;
    portENTER_CRITICAL(&mux);
    if (!pendingInterval) {
        portEXIT_CRITICAL(&mux);
        return;
    }
    pendingInterval = false;
    handle = pendingHandle;
    portEXIT_CRITICAL(&mux);
    if (server == nullptr) {
        return;
    }
    // 50–100 ms. A phone that connects as host otherwise picks ~7.5 ms and
    // unicast Wi-Fi stalls the same way a short peripheral link does.
    server->updateConnParams(handle, 0x28, 0x50, 0, 400);
}

void BleRadio::attach(BLEServer* bleServer) {
    server = bleServer;
    if (server != nullptr) {
        server->setCallbacks(this);
    }
}

int BleRadio::copyIncoming(Incoming* out, int max) const {
    if (out == nullptr || max <= 0) {
        return 0;
    }
    portENTER_CRITICAL(&mux);
    int n = incomingCount < max ? incomingCount : max;
    for (int i = 0; i < n; i++) {
        out[i] = incoming[i];
    }
    portEXIT_CRITICAL(&mux);
    return n;
}

bool BleRadio::disconnectIncoming(const char* address) {
    if (address == nullptr) {
        return false;
    }
    uint16_t connId = 0;
    bool found = false;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (strcasecmp(incoming[i].address, address) == 0) {
            connId = incoming[i].connId;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&mux);
    if (!found) {
        return false;
    }
    if (server != nullptr) {
        server->disconnect(connId);
    }
    return true;
}

void BleRadio::onConnect(BLEServer* /*server*/, ble_gap_conn_desc* desc) {
    if (desc == nullptr) {
        return;
    }
    char address[18];
    copyAddress(desc->peer_ota_addr, address, sizeof(address));
    portENTER_CRITICAL(&mux);
    pendingHandle = desc->conn_handle;
    pendingInterval = true;
    for (int i = 0; i < incomingCount; i++) {
        if (strcasecmp(incoming[i].address, address) == 0) {
            incoming[i].connId = desc->conn_handle;
            portEXIT_CRITICAL(&mux);
            return;
        }
    }
    if (incomingCount < kMaxIncoming) {
        incoming[incomingCount].connId = desc->conn_handle;
        strncpy(incoming[incomingCount].address, address, sizeof(incoming[incomingCount].address) - 1);
        incoming[incomingCount].address[sizeof(incoming[incomingCount].address) - 1] = '\0';
        incomingCount++;
    }
    portEXIT_CRITICAL(&mux);
}

void BleRadio::onDisconnect(BLEServer* /*server*/, ble_gap_conn_desc* desc) {
    restartAdvertising = true;
    if (desc == nullptr) {
        return;
    }
    char address[18];
    copyAddress(desc->peer_ota_addr, address, sizeof(address));
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId == desc->conn_handle || strcasecmp(incoming[i].address, address) == 0) {
            for (int j = i; j < incomingCount - 1; j++) {
                incoming[j] = incoming[j + 1];
            }
            incomingCount--;
            break;
        }
    }
    portEXIT_CRITICAL(&mux);
}
