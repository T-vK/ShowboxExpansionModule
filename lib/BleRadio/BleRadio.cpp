#include "BleRadio.h"
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <os/os_mbuf.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr char kMidiService[] = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";

void copyAddress(const ble_addr_t& addr, char* out, size_t size) {
    BLEAddress address(addr);
    String text = address.toString();
    strncpy(out, text.c_str(), size - 1);
    out[size - 1] = '\0';
}

bool addrZero(const ble_addr_t& addr) {
    for (int i = 0; i < 6; i++) {
        if (addr.val[i] != 0) {
            return false;
        }
    }
    return true;
}

void fillIncoming(BleRadio::Incoming& in, ble_gap_conn_desc* desc) {
    memset(&in, 0, sizeof(in));
    in.connId = desc->conn_handle;
    copyAddress(desc->peer_ota_addr, in.otaAddress, sizeof(in.otaAddress));
    if (!addrZero(desc->peer_id_addr)) {
        copyAddress(desc->peer_id_addr, in.idAddress, sizeof(in.idAddress));
    }
    if (in.idAddress[0] != '\0' && strcasecmp(in.idAddress, in.otaAddress) != 0) {
        strncpy(in.address, in.idAddress, sizeof(in.address) - 1);
    } else {
        strncpy(in.address, in.otaAddress, sizeof(in.address) - 1);
    }
}

bool incomingHas(const BleRadio::Incoming& in, const char* address) {
    if (address == nullptr || address[0] == '\0') {
        return false;
    }
    if (strcasecmp(in.address, address) == 0 || strcasecmp(in.otaAddress, address) == 0) {
        return true;
    }
    return in.idAddress[0] != '\0' && strcasecmp(in.idAddress, address) == 0;
}

void addSkipLocked(char skip[][18], int& count, const char* address) {
    if (address == nullptr || address[0] == '\0' || count >= 8) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (strcasecmp(skip[i], address) == 0) {
            return;
        }
    }
    strncpy(skip[count], address, 17);
    skip[count][17] = '\0';
    count++;
}

bool skipHas(const char skip[][18], int count, const BleRadio::Incoming& in) {
    for (int i = 0; i < count; i++) {
        if (incomingHas(in, skip[i])) {
            return true;
        }
    }
    return false;
}

class RadioSecurity : public BLESecurityCallbacks {
public:
    BleRadio* radio = nullptr;
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (radio != nullptr && desc != nullptr) {
            radio->onBonded(desc);
        }
    }
};

RadioSecurity radioSecurity;

void copyPrintableName(const char* text, int len, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return;
    }
    out[0] = '\0';
    if (text == nullptr || len <= 0) {
        return;
    }
    size_t n = 0;
    for (int i = 0; i < len && n + 1 < size; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c > 126) {
            continue;
        }
        if (n == 0 && c == ' ') {
            continue;
        }
        out[n++] = static_cast<char>(c);
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

int onGapNameAttr(uint16_t connHandle, const struct ble_gatt_error* error,
        struct ble_gatt_attr* attr, void* arg) {
    BleRadio* radio = static_cast<BleRadio*>(arg);
    if (radio == nullptr) {
        return 0;
    }
    int status = error != nullptr ? error->status : BLE_HS_EUNKNOWN;
    if (status == BLE_HS_EDONE) {
        radio->finishIncomingName(connHandle, false);
        return 0;
    }
    if (status != 0) {
        radio->finishIncomingName(connHandle, false);
        return 0;
    }
    if (attr == nullptr || attr->om == nullptr) {
        return 0;
    }
    char raw[32];
    int len = OS_MBUF_PKTLEN(attr->om);
    if (len > 31) {
        len = 31;
    }
    if (len <= 0) {
        return 0;
    }
    os_mbuf_copydata(attr->om, 0, len, raw);
    raw[len] = '\0';
    char name[32];
    copyPrintableName(raw, len, name, sizeof(name));
    if (name[0] != '\0') {
        radio->storeIncomingName(connHandle, name);
    }
    return 0;
}
}

void BleRadio::setName(const char* name) {
    if (name != nullptr && name[0] != '\0') {
        gapName = name;
    }
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

void BleRadio::setAdvertiseAllowed(bool allowed) {
    if (allowed && !adsAllowed) {
        restartAdvertising = true;
        lastAdvStartMs = 0;
    }
    adsAllowed = allowed;
}

void BleRadio::apply() {
    if (!BLEDevice::getInitialized()) {
        return;
    }
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    if (advertising == nullptr) {
        return;
    }
    refreshLinkCount();
    bool want = (midiOn || hidOn) && adsAllowed;
    if (!want) {
        if (advertising->isAdvertising()) {
            advertising->stop();
        }
        advertised = false;
        restartAdvertising = false;
        return;
    }
    // With every ACL link in use a fourth connect cannot complete, and
    // retrying start() there crowds Wi-Fi off the radio.
    if (liveLinks >= kMaxIncoming) {
        if (advertising->isAdvertising()) {
            advertising->stop();
        }
        advertised = false;
        restartAdvertising = false;
        return;
    }
    bool active = advertising->isAdvertising();
    bool payloadSame = advertised && midiOn == advertisedMidi && hidOn == advertisedHid;
    if (active && payloadSame && !restartAdvertising) {
        return;
    }
    if (active && payloadSame) {
        restartAdvertising = false;
        return;
    }
    uint32_t now = millis();
    int hosts = 0;
    portENTER_CRITICAL(&mux);
    hosts = incomingCount;
    portEXIT_CRITICAL(&mux);
    if (hosts > 0 && lastIncomingMs != 0 && (int32_t)(now - lastIncomingMs) < 800) {
        return;
    }
    if (!restartAdvertising && lastAdvStartMs != 0 && (int32_t)(now - lastAdvStartMs) < 5000) {
        return;
    }
    // The library default is 20–40 ms, the shortest interval the controller
    // allows. On this chip that duty cycle leaves Wi-Fi no airtime for unicast,
    // so the page never loads. 100–200 ms is still found promptly.
    advertising->setMinInterval(0xA0);
    advertising->setMaxInterval(0x140);
    // Rewriting the payload while an advertisement is already running fills the
    // controller (HCI 0x2008/0x2009 status 0x07) and crowds Wi-Fi off the radio.
    // A scan, a host disconnect, or a failed start leaves the controller quiet
    // while this object still thinks it is advertising — start again from the
    // radio state, not that flag.
    if (active) {
        advertising->stop();
    }

    BLEAdvertisementData adv;
    adv.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    // Apple only shows a name that is in the advert itself, and skips names
    // longer than 15 characters. HID and MIDI together do not fit next to a
    // 128-bit UUID, so the keyboard UUID stays here and MIDI goes in the
    // scan response. One pair still exposes both GATT services.
    adv.setName(gapName);
    if (hidOn) {
        adv.setAppearance(0x03C0);
        adv.setPartialServices(BLEUUID((uint16_t)0x1812));
    }
    BLEAdvertisementData scan;
    if (midiOn) {
        if (hidOn) {
            scan.setPartialServices(BLEUUID(kMidiService));
        } else {
            adv.setPartialServices(BLEUUID(kMidiService));
        }
    }
    static bool pairingReady = false;
    if (!pairingReady) {
        BLESecurity* security = new BLESecurity();
        security->setAuthenticationMode(ESP_LE_AUTH_BOND);
        security->setForceAuthentication(false);
        security->setCapability(ESP_IO_CAP_NONE);
        security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        radioSecurity.radio = this;
        BLEDevice::setSecurityCallbacks(&radioSecurity);
        pairingReady = true;
    }
    bool needScan = midiOn && hidOn;
    advertising->setScanResponse(needScan);
    advertising->setAdvertisementData(adv);
    if (needScan) {
        advertising->setScanResponseData(scan);
    }
    lastAdvStartMs = now;
    bool started = advertising->start();
    if (started) {
        advertised = true;
        advertisedMidi = midiOn;
        advertisedHid = hidOn;
        restartAdvertising = false;
    } else {
        advertised = false;
        restartAdvertising = true;
    }
}

void BleRadio::service() {
    refreshLinkCount();
    uint16_t connId = 0;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (!incoming[i].intervalPending || incoming[i].connId == 0) {
            continue;
        }
        if ((int32_t)(millis() - incoming[i].sinceMs) < 500) {
            continue;
        }
        incoming[i].intervalPending = false;
        connId = incoming[i].connId;
        break;
    }
    portEXIT_CRITICAL(&mux);
    if (connId == 0) {
        return;
    }
    // Linux HID is fine at 30–60 ms. Android HOGP often drops the bond if we
    // push 100–200 ms right after connect. 30–60 ms still leaves Wi-Fi airtime.
    ble_gap_upd_params params = {};
    params.itvl_min = hidOn ? 0x18 : 0x50;
    params.itvl_max = hidOn ? 0x30 : 0xA0;
    params.latency = 0;
    params.supervision_timeout = 400;
    ble_gap_update_params(connId, &params);
}

void BleRadio::queryIncomingNames() {
    if (hidOn) {
        return;
    }
    uint16_t connId = 0;
    bool found = false;
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].name[0] != '\0' || incoming[i].namePending || incoming[i].nameTries >= 1
            || incoming[i].connId == 0) {
            continue;
        }
        if ((int32_t)(now - incoming[i].sinceMs) < 2500) {
            continue;
        }
        if (!incoming[i].bonded && (int32_t)(now - incoming[i].sinceMs) < 5000) {
            continue;
        }
        incoming[i].namePending = true;
        incoming[i].nameTries = 1;
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].address);
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].otaAddress);
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].idAddress);
        connId = incoming[i].connId;
        found = true;
        break;
    }
    portEXIT_CRITICAL(&mux);
    if (!found) {
        return;
    }
    struct ble_gap_conn_desc desc = {};
    if (ble_gap_conn_find(connId, &desc) != 0) {
        finishIncomingName(connId, false);
        return;
    }
    ble_uuid16_t uuid = BLE_UUID16_INIT(0x2A00);
    int rc = ble_gattc_read_by_uuid(connId, 1, 0xFFFF, &uuid.u, onGapNameAttr, this);
    if (rc == 0) {
        return;
    }
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId != connId) {
            continue;
        }
        incoming[i].namePending = false;
        if (rc == BLE_HS_EBUSY || rc == BLE_HS_EAGAIN) {
            incoming[i].nameTries = 0;
        }
        break;
    }
    portEXIT_CRITICAL(&mux);
}

void BleRadio::preferIncomingName(const char* address, const char* name) {
    if (address == nullptr || address[0] == '\0' || name == nullptr || name[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (!incomingHas(incoming[i], address)) {
            continue;
        }
        if (incoming[i].name[0] == '\0') {
            strncpy(incoming[i].name, name, sizeof(incoming[i].name) - 1);
            incoming[i].name[sizeof(incoming[i].name) - 1] = '\0';
        }
        incoming[i].namePending = false;
        incoming[i].nameTries = 1;
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].address);
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].otaAddress);
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].idAddress);
        break;
    }
    portEXIT_CRITICAL(&mux);
}

void BleRadio::storeIncomingName(uint16_t connId, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId != connId) {
            continue;
        }
        strncpy(incoming[i].name, name, sizeof(incoming[i].name) - 1);
        incoming[i].name[sizeof(incoming[i].name) - 1] = '\0';
        incoming[i].namePending = false;
        incoming[i].nameTries = 1;
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].address);
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].otaAddress);
        addSkipLocked(nameSkip, nameSkipCount, incoming[i].idAddress);
        break;
    }
    portEXIT_CRITICAL(&mux);
}

void BleRadio::finishIncomingName(uint16_t connId, bool keepPending) {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId != connId) {
            continue;
        }
        if (!keepPending) {
            incoming[i].namePending = false;
        }
        break;
    }
    portEXIT_CRITICAL(&mux);
}

void BleRadio::attach(BLEServer* bleServer) {
    server = bleServer;
    if (server != nullptr) {
        server->setCallbacks(this);
        // Start advertising from apply() when Connections allows it. Auto
        // restart after a disconnect would keep the radio busy while another
        // host is still linked.
        server->advertiseOnDisconnect(false);
    }
}

void BleRadio::setIncomingHandler(void (*handler)(void* ctx, const char* address, uint16_t connId, bool connected), void* ctx) {
    incomingHandler = handler;
    incomingHandlerCtx = ctx;
}

void BleRadio::refreshLinkCount() {
    int n = 0;
    ble_gap_conn_desc desc;
    for (uint16_t handle = 0; handle < 32; handle++) {
        if (ble_gap_conn_find(handle, &desc) == 0) {
            n++;
        }
    }
    liveLinks = n;
}

int BleRadio::gapLinkCount() const {
    return liveLinks;
}

bool BleRadio::hasIncoming() const {
    portENTER_CRITICAL(&mux);
    bool live = incomingCount > 0;
    portEXIT_CRITICAL(&mux);
    return live;
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
        if (incomingHas(incoming[i], address)) {
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
    lastIncomingMs = millis();
    if (desc == nullptr) {
        return;
    }
    Incoming slot;
    fillIncoming(slot, desc);
    slot.sinceMs = millis();
    slot.intervalPending = true;
    portENTER_CRITICAL(&mux);
    if (skipHas(nameSkip, nameSkipCount, slot)) {
        slot.nameTries = 1;
    }
    int found = -1;
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId == desc->conn_handle || incomingHas(incoming[i], slot.address)
            || incomingHas(incoming[i], slot.otaAddress)) {
            Incoming keep = incoming[i];
            incoming[i] = slot;
            if (keep.connId == desc->conn_handle) {
                incoming[i].sinceMs = keep.sinceMs;
                incoming[i].intervalPending = keep.intervalPending;
                incoming[i].bonded = keep.bonded;
                if (keep.name[0] != '\0') {
                    strncpy(incoming[i].name, keep.name, sizeof(incoming[i].name) - 1);
                    incoming[i].namePending = keep.namePending;
                    incoming[i].nameTries = keep.nameTries;
                }
            }
            found = i;
            break;
        }
    }
    if (found < 0 && incomingCount < kMaxIncoming) {
        incoming[incomingCount] = slot;
        incomingCount++;
        found = incomingCount - 1;
    }
    char notify[18];
    notify[0] = '\0';
    uint16_t connId = desc->conn_handle;
    if (found >= 0) {
        strncpy(notify, incoming[found].address, sizeof(notify) - 1);
        notify[sizeof(notify) - 1] = '\0';
    }
    portEXIT_CRITICAL(&mux);
    if (incomingHandler != nullptr && notify[0] != '\0') {
        incomingHandler(incomingHandlerCtx, notify, connId, true);
    }
    if (!adsAllowed) {
        advertised = false;
        restartAdvertising = false;
    }
    if (!desc->sec_state.encrypted) {
        ble_gap_security_initiate(desc->conn_handle);
    }
}

void BleRadio::onBonded(ble_gap_conn_desc* desc) {
    if (desc == nullptr) {
        return;
    }
    Incoming slot;
    fillIncoming(slot, desc);
    char notify[18];
    notify[0] = '\0';
    uint16_t connId = desc->conn_handle;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId == desc->conn_handle || incomingHas(incoming[i], slot.otaAddress)) {
            Incoming keep = incoming[i];
            incoming[i] = slot;
            incoming[i].sinceMs = keep.sinceMs != 0 ? keep.sinceMs : millis();
            incoming[i].intervalPending = keep.intervalPending;
            incoming[i].bonded = true;
            strncpy(incoming[i].name, keep.name, sizeof(incoming[i].name) - 1);
            incoming[i].name[sizeof(incoming[i].name) - 1] = '\0';
            incoming[i].namePending = keep.namePending;
            incoming[i].nameTries = keep.nameTries;
            strncpy(notify, incoming[i].address, sizeof(notify) - 1);
            notify[sizeof(notify) - 1] = '\0';
            break;
        }
    }
    portEXIT_CRITICAL(&mux);
    if (incomingHandler != nullptr && notify[0] != '\0') {
        incomingHandler(incomingHandlerCtx, notify, connId, true);
    }
}

void BleRadio::onDisconnect(BLEServer* /*server*/, ble_gap_conn_desc* desc) {
    restartAdvertising = true;
    lastAdvStartMs = 0;
    if (desc == nullptr) {
        return;
    }
    Incoming slot;
    fillIncoming(slot, desc);
    char notify[18];
    strncpy(notify, slot.address, sizeof(notify) - 1);
    notify[sizeof(notify) - 1] = '\0';
    uint16_t connId = desc->conn_handle;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < incomingCount; i++) {
        if (incoming[i].connId == desc->conn_handle || incomingHas(incoming[i], slot.address)
            || incomingHas(incoming[i], slot.otaAddress)) {
            strncpy(notify, incoming[i].address, sizeof(notify) - 1);
            notify[sizeof(notify) - 1] = '\0';
            connId = incoming[i].connId;
            for (int j = i; j < incomingCount - 1; j++) {
                incoming[j] = incoming[j + 1];
            }
            incomingCount--;
            break;
        }
    }
    portEXIT_CRITICAL(&mux);
    if (incomingHandler != nullptr) {
        incomingHandler(incomingHandlerCtx, notify, connId, false);
    }
}
