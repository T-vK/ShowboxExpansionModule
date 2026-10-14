#include "BleRadio.h"
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <os/os_mbuf.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr char kMidiService[] = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";

// Android HOGP stays at ATT MTU 23 unless we initiate the exchange.
// At 23 bytes the 53-byte Pointer map arrives as first+last blob only
// (31-byte cached DSCP, EV=1 SYN-only). 256 fits kb+consumer+mouse.
void requestAttMtu(uint16_t connId) {
    ble_att_set_preferred_mtu(256);
    ble_gattc_exchange_mtu(connId, nullptr, nullptr);
}

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

namespace {
void stopGapAdv(BLEAdvertising* advertising) {
    if (!ble_gap_adv_active()) {
        return;
    }
    if (advertising != nullptr) {
        advertising->stop();
    }
    ble_gap_adv_stop();
    uint32_t t0 = millis();
    while (ble_gap_adv_active() && (int32_t)(millis() - t0) < 80) {
        delay(1);
    }
}
}

void BleRadio::restartWhenIdle() {
    restartAdvertising = true;
    lastAdvStartMs = 0;
}

void BleRadio::stopNow() {
    stopGapAdv(BLEDevice::getAdvertising());
    advertised = false;
    advIntervalApplied = false;
    restartAdvertising = false;
}

void BleRadio::resumeExisting() {
    adsAllowed = true;
    restartAdvertising = false;
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    if (advertising == nullptr) {
        return;
    }
    advertising->setMinInterval(BleRadio::kAdvItvlMin);
    advertising->setMaxInterval(BleRadio::kAdvItvlMax);
    advertising->setScanResponse(false);
    if (!ble_gap_adv_active()) {
        advertising->start();
    }
    advertised = ble_gap_adv_active();
    advertisedMidi = midiOn;
    advertisedHid = hidOn;
    advIntervalApplied = advertised;
}

void BleRadio::setAdvertiseAllowed(bool allowed) {
    if (allowed && !adsAllowed) {
        restartAdvertising = true;
        lastAdvStartMs = 0;
    }
    adsAllowed = allowed;
}

void BleRadio::setPedalShare(bool share) {
    pedalShare = share;
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
    uint16_t advMin = BleRadio::kAdvItvlMin;
    uint16_t advMax = BleRadio::kAdvItvlMax;
    if (pedalShare && adsAllowed) {
        // 100 ms ads plus AIRSTEP used to kill ARP. 2560 ms was too
        // slow for the 8 s host scan. 400 ms stays visible and leaves
        // Wi-Fi gaps next to the 200 ms pedal.
        advMin = 0x280;
        advMax = 0x320;
    }
    advertising->setMinInterval(advMin);
    advertising->setMaxInterval(advMax);
    if (!want || liveLinks >= kMaxIncoming) {
        stopGapAdv(advertising);
        advertised = false;
        advIntervalApplied = false;
        restartAdvertising = false;
        return;
    }
    bool active = ble_gap_adv_active();
    bool payloadSame = advertised && midiOn == advertisedMidi && hidOn == advertisedHid;
    if (active && payloadSame && advIntervalApplied && !restartAdvertising) {
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
    if (!restartAdvertising && active && lastAdvStartMs != 0 && (int32_t)(now - lastAdvStartMs) < 5000) {
        return;
    }
    if (active) {
        stopGapAdv(advertising);
        if (ble_gap_adv_active()) {
            restartAdvertising = true;
            advIntervalApplied = false;
            return;
        }
    }
    advertising->setMinInterval(advMin);
    advertising->setMaxInterval(advMax);

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
    // Scan-response RX after every adv eats Wi-Fi when a 200 ms pedal is live.
    // The name stays in the advert, so phones and the 8s host scan still find us.
    bool needScan = midiOn && hidOn && liveLinks == 0 && !pedalShare;
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
        advIntervalApplied = true;
        restartAdvertising = false;
    } else {
        advertised = false;
        advIntervalApplied = false;
        restartAdvertising = false;
        lastAdvStartMs = now;
    }
}

void BleRadio::service() {
    int hosts = 0;
    portENTER_CRITICAL(&mux);
    hosts = incomingCount;
    portEXIT_CRITICAL(&mux);
    if (pedalShare && hosts == 0) {
        return;
    }
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
        static uint32_t lastCapMs = 0;
        uint32_t now = millis();
        if (lastCapMs != 0 && (int32_t)(now - lastCapMs) < 2000) {
            return;
        }
        lastCapMs = now;
        ble_gap_conn_desc desc = {};
        for (uint16_t handle = 0; handle < 32; handle++) {
            if (ble_gap_conn_find(handle, &desc) != 0) {
                continue;
            }
            bool incoming = desc.role != BLE_GAP_ROLE_MASTER;
            // Outgoing (AIRSTEP) is capped in BleMidi. Recapping a pedal that
            // will not go slower than 200 ms is a permanent radio storm.
            if (!incoming) {
                continue;
            }
            if (incoming && hidOn) {
                continue;
            }
            if (ceCapped & (1u << handle)) {
                continue;
            }
            lastCapMs = millis();
            ceCapped |= (1u << handle);
            ble_gap_upd_params slow = {};
            slow.itvl_min = BleRadio::kWifiSafeItvl;
            slow.itvl_max = BleRadio::kSlowItvlMax;
            slow.latency = 0;
            slow.supervision_timeout = 800;
            slow.min_ce_len = 0;
            slow.max_ce_len = BleRadio::kMaxCeLen;
            ble_gap_update_params(handle, &slow);
            break;
        }
        return;
    }
    // Linux HID is fine at 30–60 ms. Android HOGP often drops the bond if we
    // push 100–200 ms right after connect. 30–60 ms still leaves Wi-Fi airtime.
    ble_gap_upd_params params = {};
    params.itvl_min = hidOn ? 0x18 : 0x50;
    params.itvl_max = hidOn ? 0x30 : 0xA0;
    params.latency = 0;
    params.supervision_timeout = 400;
    params.min_ce_len = 0;
    params.max_ce_len = BleRadio::kMaxCeLen;
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
    uint16_t minInterval = 0;
    uint32_t liveMask = 0;
    ble_gap_conn_desc desc;
    for (uint16_t handle = 0; handle < 32; handle++) {
        if (ble_gap_conn_find(handle, &desc) == 0) {
            n++;
            liveMask |= (1u << handle);
            if (minInterval == 0 || desc.conn_itvl < minInterval) {
                minInterval = desc.conn_itvl;
            }
        }
    }
    liveLinks = n;
    minItvl = minInterval;
    ceCapped &= liveMask;
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
    // Exchange MTU only after the link is encrypted. Doing it here races
    // BlueZ SMP and laptop-pair returns "no pair result" with Paired: no.
    if (!desc->sec_state.encrypted) {
        ble_gap_security_initiate(desc->conn_handle);
    } else {
        requestAttMtu(connId);
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
    requestAttMtu(connId);
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
