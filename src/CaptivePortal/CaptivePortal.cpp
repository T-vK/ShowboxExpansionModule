#include "CaptivePortal.h"

#include "DebugLog.h"
#include "System/LanKeep.h"
#include <UltimateBaseFirmware.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_coexist.h>
#include <ArduinoJson.h>
#include <Preferences.h>

namespace {
constexpr const char* kApSsid = "ShowboxMod";
constexpr const char* kApPass = "showboxmod";
constexpr uint32_t kFallbackApMs = 8000;
}

void CaptivePortal::loadPrefs() {
    Preferences prefs;
    if (!prefs.begin("radio", true)) {
        return;
    }
    apWanted = prefs.getBool("hotspot", true);
    // Default true: SoftAP + STA together. Older firmwares defaulted false and
    // boot-join persisted apWanted=false, so migrate once to the product default.
    apAlways = prefs.getBool("apalways", true);
    apAuto = prefs.getBool("apauto", true);
    autoConnect = prefs.getBool("stauto", true);
    const bool coexMigrated = prefs.getBool("ap_coex1", false);
    String packed = prefs.getString("nets", "");
    prefs.end();
    rememberedN = 0;
    int start = 0;
    while (start < (int)packed.length() && rememberedN < 8) {
        int nl = packed.indexOf('\n', start);
        if (nl < 0) {
            nl = packed.length();
        }
        String row = packed.substring(start, nl);
        start = nl + 1;
        int tab = row.indexOf('\t');
        String ssid = tab < 0 ? row : row.substring(0, tab);
        String pass = tab < 0 ? "" : row.substring(tab + 1);
        ssid.trim();
        if (ssid.length() == 0) {
            continue;
        }
        remembered[rememberedN].ssid = ssid;
        remembered[rememberedN].pass = pass;
        rememberedN++;
    }
    // After nets are loaded — flushWanted must not rewrite an empty list.
    if (!coexMigrated) {
        apAlways = true;
        apWanted = true;
        persistWanted();
    }
}

void CaptivePortal::begin(AsyncWebServer* /*server*/, UltimateBaseFirmware* firmwareIn) {
    firmware = firmwareIn;
    loadPrefs();
    WiFi.setAutoReconnect(autoConnect);
    // Kick STA from remembered nets so reboot restores LAN. SoftAP is held
    // off only while joinInFlight (tick) — do not persist apWanted=false or
    // Hotspot+STA never comes back after a successful boot join.
    debugLog("portal begin radio=%d auto=%d nets=%u apWanted=%d apAlways=%d",
        radioOn() ? 1 : 0, autoConnect ? 1 : 0, (unsigned)rememberedN,
        apWanted ? 1 : 0, apAlways ? 1 : 0);
    if (radioOn() && autoConnect && rememberedN > 0 && remembered[0].ssid.length() > 0) {
        pendingStaSsid = remembered[0].ssid;
        pendingStaPass = remembered[0].pass;
        pendingStaJoin = true;
        pendingStaJoinAtMs = millis() + 800;
        lanNoteStaJoin(90000);
        staJoinDeadlineMs = millis() + 90000;
        debugLog("sta boot join queued ssid=%s passLen=%u", pendingStaSsid.c_str(),
            (unsigned)pendingStaPass.length());
        return;
    }
    ensureAp();
}

bool CaptivePortal::radioOn() const {
    return firmware != nullptr && firmware->wifiRadioEnabled();
}

bool CaptivePortal::staUp() const {
    return WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
}

bool CaptivePortal::portalOpen() const {
    return firmware != nullptr && firmware->setupPortalOpen();
}

bool CaptivePortal::fromAccessPoint(IPAddress local) const {
    IPAddress ap = WiFi.softAPIP();
    return ap[0] != 0 && local[0] == ap[0] && local[1] == ap[1] && local[2] == ap[2];
}

bool CaptivePortal::apShouldRun() const {
    if (!radioOn() || portalOpen()) {
        return false;
    }
    // Hotspot+STA is the product default (apAlways). SoftAP+STA+BLE can
    // starve STA HTTP if softAP fails into an empty APSTA — that is handled
    // in ensureAp (revert + apGaveUpThisSta) and lanTickHttpHealth (drop
    // SoftAP on probe fail). Do not blanket-ban SoftAP while BLE is live:
    // L1 hotspot / L2 hotspot-sta-coex need all three up together.
    if (staUp() && !(apAlways && apWanted)) {
        return false;
    }
    // JTAG-era: STA blip + SoftAP retry every 1s while a pedal was up left
    // status=254. After a failed softAP this association, wait for STA drop
    // or an explicit prefs change before trying again.
    if (staUp() && apGaveUpThisSta) {
        return false;
    }
    if (lanBleLive() && !apAlways && !apWanted) {
        return false;
    }
    if (apWanted) {
        return true;
    }
    return apAuto && millis() > kFallbackApMs;
}

void CaptivePortal::persistWanted() {
    apWantedDirty = true;
}

void CaptivePortal::flushWanted() {
    if (!apWantedDirty) {
        return;
    }
    apWantedDirty = false;
    Preferences prefs;
    if (!prefs.begin("radio", false)) {
        return;
    }
    prefs.putBool("hotspot", apWanted);
    prefs.putBool("apalways", apAlways);
    prefs.putBool("apauto", apAuto);
    prefs.putBool("stauto", autoConnect);
    prefs.putBool("ap_coex1", true);
    String packed;
    for (uint8_t i = 0; i < rememberedN; i++) {
        packed += remembered[i].ssid;
        packed += '\t';
        packed += remembered[i].pass;
        packed += '\n';
    }
    size_t wrote = prefs.putString("nets", packed);
    debugLog("radio prefs flush ap=%d always=%d nets=%u packed=%u wrote=%u",
        apWanted ? 1 : 0, apAlways ? 1 : 0, (unsigned)rememberedN,
        (unsigned)packed.length(), (unsigned)wrote);
    prefs.end();
}

void CaptivePortal::adoptFallbackAp() {
    if (!apAuto || apWanted || staUp() || portalOpen() || !radioOn() || millis() < kFallbackApMs) {
        return;
    }
    apWanted = true;
    persistWanted();
    debugLog("ap fallback on, sta down");
}

bool CaptivePortal::setWantedAp(bool on) {
    // Only force the hotspot to stay up when there is no remembered STA to
    // join. After NVS wipe + POST /wifi, refusing ap=false while STA was
    // still associating left AP+STA on a wedged join (reconnect thrash,
    // status=6 forever).
    if (!on && radioOn() && !staUp() && !apAlways && rememberedN == 0) {
        on = true;
    }
    apWanted = on;
    apGaveUpThisSta = false;
    apFailStreak = 0;
    // Explicit API/UI toggle must not sit behind boot settle / LanKeep backoff.
    lastApTryMs = 0;
    apForceTry = on;
    persistWanted();
    if (!on) {
        stopAp();
    }
    // SoftAP only from tick()/main loop — softAP() in the AsyncTCP HTTP
    // callback wedges STA (POST /wifi TimeoutError) and starves the loop.
    return true;
}

bool CaptivePortal::setAlwaysAp(bool on) {
    apAlways = on;
    apGaveUpThisSta = false;
    apFailStreak = 0;
    lastApTryMs = 0;
    apForceTry = on && apWanted;
    persistWanted();
    if (!(on && apWanted) && !apShouldRun() && !portalOpen()) {
        stopAp();
    }
    return true;
}

bool CaptivePortal::setAutoAp(bool on) {
    apAuto = on;
    persistWanted();
    return true;
}

bool CaptivePortal::setAutoConnect(bool on) {
    autoConnect = on;
    WiFi.setAutoReconnect(on);
    persistWanted();
    return true;
}

void CaptivePortal::rememberNet(const String& ssid, const String& password) {
    if (ssid.length() == 0) {
        return;
    }
    int found = -1;
    for (uint8_t i = 0; i < rememberedN; i++) {
        if (remembered[i].ssid == ssid) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        if (rememberedN < 8) {
            found = rememberedN++;
        } else {
            for (uint8_t i = 0; i < 7; i++) {
                remembered[i] = remembered[i + 1];
            }
            found = 7;
            rememberedN = 8;
        }
        remembered[found].pass = password;
    } else if (password.length() > 0) {
        remembered[found].pass = password;
    }
    remembered[found].ssid = ssid;
    persistWanted();
}

String CaptivePortal::passwordFor(const String& ssid) const {
    for (uint8_t i = 0; i < rememberedN; i++) {
        if (remembered[i].ssid == ssid) {
            return remembered[i].pass;
        }
    }
    return "";
}

bool CaptivePortal::forgetSta(const String& ssid) {
    uint8_t w = 0;
    for (uint8_t i = 0; i < rememberedN; i++) {
        if (remembered[i].ssid == ssid) {
            continue;
        }
        remembered[w++] = remembered[i];
    }
    if (w == rememberedN) {
        return false;
    }
    rememberedN = w;
    persistWanted();
    return true;
}

bool CaptivePortal::apIfaceLive(uint8_t expectChannel) const {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        return false;
    }
    if (WiFi.softAPSSID() != kApSsid) {
        return false;
    }
    // Beacon can advertise ShowboxMod a beat before softAPIP is bound —
    // treat SSID+mode as live only when the AP address is up so captive
    // clients have a gateway. Callers that see SSID-without-IP should
    // softAPConfig rather than softAPdisconnect (that fight left L1 dark).
    if (WiFi.softAPIP()[0] == 0) {
        return false;
    }
    if (expectChannel != 0) {
        wifi_config_t cfg = {};
        if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK && cfg.ap.channel != 0
            && cfg.ap.channel != expectChannel) {
            return false;
        }
    }
    return true;
}

bool CaptivePortal::apSsidOnAir() const {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        return false;
    }
    return WiFi.softAPSSID() == kApSsid;
}

void CaptivePortal::bindApAddress() {
    if (WiFi.softAPIP()[0] != 0) {
        return;
    }
    // Safe after softAP() has claimed ShowboxMod — pre-softAP softAPConfig
    // rebinds netifs and wedges STA HTTP under APSTA.
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0));
}

void CaptivePortal::ensureAp() {
    if (!staUp()) {
        apGaveUpThisSta = false;
        apFailStreak = 0;
    }
    if (!apShouldRun()) {
        if (!portalOpen()) {
            stopAp();
        } else {
            stopDns();
        }
        return;
    }
    // Never drop to AP-only when STA credentials or auto-reconnect exist.
    // That is what froze status=254 after a BLE-era STA blip.
    const bool keepSta = autoConnect || rememberedN > 0 || WiFi.SSID().length() > 0 || staUp();
    uint8_t channel = 1;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&channel, &second);
    if (channel == 0) {
        channel = 1;
    }
    // Beacon-only SoftAP: ShowboxMod on air but softAPIP still 0.0.0.0.
    // Binding the address heals L1 (air scan saw SSID, API reported ap=false)
    // without softAPdisconnect thrash that wedges STA HTTP.
    if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
        debugLog("ap ssid on air without IP — bindApAddress");
        bindApAddress();
        delay(40);
    }
    // Ghost SoftAP: softAPIP set / apUp latched but mode is STA, SSID drifted,
    // or SoftAP channel lagged a STA channel change — air scan finds nothing.
    if (apUp && !apIfaceLive(channel) && !apSsidOnAir()) {
        debugLog("ap ghost (mode/ssid/ch) — clearing latch for softAP restart");
        apUp = false;
        stopDns();
        lanNoteApActive(false);
    }
    if (apIfaceLive(channel)) {
        apUp = true;
        apFailStreak = 0;
        apGaveUpThisSta = false;
        apForceTry = false;
    } else if (!apUp || WiFi.softAPIP()[0] == 0) {
        // Automatic SoftAP while STA is up waits out the boot settle window so
        // L1 uart/mdns/telnet are not racing softAP() coex wedges. Explicit
        // API/UI (apForceTry) skips the wall — hotspot POST still brings it up.
        // Keep apForceTry set across BLE-yield deferrals until SoftAP is live.
        if (!apForceTry && staUp() && apFailStreak == 0 && lastApTryMs == 0
            && millis() < kApBootSettleMs) {
            return;
        }
        // Hotspot+STA: keep retries short. Long 120s/300s backoff after a
        // coex miss made L1 hotspot time out while SoftAP was still dark.
        // Explicit API force: retry every 2s so L1's 35s wait sees SoftAP.
        const uint32_t backoff = apForceTry ? 2000u
            : ((!apAlways && staUp() && apFailStreak > 0)
                ? (apFailStreak >= 2 ? 300000u : 120000u)
                : (apAlways && apFailStreak > 0 ? (apFailStreak >= 3 ? 45000u : kApRetryMs) : kApRetryMs));
        if (lastApTryMs != 0 && (int32_t)(millis() - lastApTryMs) < (int32_t)backoff) {
            return;
        }
        lastApTryMs = millis();
        // Never WiFi.disconnect() while STA is up — that drops the home AP
        // every time SoftAP starts (Hotspot+STA default). Only bounce the
        // radio when there is no LAN association to protect.
        if (!staUp()) {
            WiFi.disconnect(false, false);
            delay(30);
        }
        // Do NOT WiFi.mode(APSTA) before softAP succeeds. An empty AP
        // interface on APSTA kills STA HTTP (SYN-ACK yes, bodies no).
        // softAP() itself promotes STA→APSTA when association is live.
        if (!keepSta) {
            wifi_mode_t mode = WIFI_MODE_NULL;
            esp_wifi_get_mode(&mode);
            if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
                WiFi.mode(WIFI_AP);
            }
        }
        // softAP() under a live BLE host starves STA HTTP. Yield the pedal
        // then continue in this tick — deferring forever left L1 hotspot dark
        // when hostConnected() stayed true after dropHostLink().
        if (lanYieldBleForSoftAp()) {
            delay(30);
        }
        // Already advertising ShowboxMod — do not softAP() again; bind IP.
        if (apSsidOnAir()) {
            bindApAddress();
            // Short poll only — long delay loops starve AsyncTCP / L1 HTTP.
            for (uint8_t i = 0; i < 5 && WiFi.softAPIP()[0] == 0; i++) {
                delay(20);
            }
            debugLog("ap reclaim ssid ip=%s", WiFi.softAPIP().toString().c_str());
        } else {
            const bool began = WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
            debugLog("ap begin=%d ip=%s ch=%u wanted=%d always=%d fail=%u ssid=%s", began ? 1 : 0,
                WiFi.softAPIP().toString().c_str(), (unsigned)channel, apWanted ? 1 : 0,
                apAlways ? 1 : 0, (unsigned)apFailStreak, WiFi.softAPSSID().c_str());
            for (uint8_t i = 0; i < 5 && (!apSsidOnAir() || WiFi.softAPIP()[0] == 0); i++) {
                delay(20);
            }
            if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
                bindApAddress();
                delay(20);
            }
            if (began && !apIfaceLive(channel) && !apSsidOnAir()) {
                debugLog("ap begin partial (ssid=%s) — reassert", WiFi.softAPSSID().c_str());
                WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
                delay(20);
                if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
                    bindApAddress();
                }
            }
        }
        if (!apIfaceLive(channel)) {
            apUp = false;
            if (apFailStreak < 250) {
                apFailStreak++;
            }
            // Only tear down when the ShowboxMod beacon is NOT on air —
            // disconnecting a beacon-only SoftAP is what left apIp=0 while
            // nmcli still saw ShowboxMod and STA HTTP wedged on retry.
            if (staUp() && !apSsidOnAir()) {
                WiFi.softAPdisconnect(false);
            }
            if (apAlways) {
                debugLog("ap begin failed — STA kept, SoftAP retry later (always)");
            } else if (staUp()) {
                apGaveUpThisSta = true;
                debugLog("ap begin failed — SoftAP abandoned this assoc");
            }
            lanNoteApActive(false);
            stopDns();
            return;
        }
        apUp = true;
        apFailStreak = 0;
        apGaveUpThisSta = false;
        apForceTry = false;
        // Prefer WiFi airtime right after softAP — otherwise the next STA
        // HTTP (L1 reaction-engine) times out while BLE coex catches up.
        lanPreferWifiAirtime();
    }
    // A bare mode switch can leave the default ESP_XXXX beacon up if softAP
    // lost its config; re-assert the hotspot identity.
    if (WiFi.softAPSSID() != kApSsid) {
        debugLog("ap ssid drift -> %s, reasserting", WiFi.softAPSSID().c_str());
        if (!WiFi.softAP(kApSsid, kApPass, channel, 0, 2) || WiFi.softAPIP()[0] == 0) {
            apUp = false;
            return;
        }
    }
    lanNoteApActive(true);
    startDns();
}

void CaptivePortal::stopAp() {
    stopDns();
    lanNoteApActive(false);
    if (!apUp && WiFi.softAPIP()[0] == 0) {
        return;
    }
    WiFi.softAPdisconnect(false);
    apUp = false;
    if (radioOn()) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
            WiFi.mode(WIFI_STA);
        }
    }
    debugLog("ap stop");
}

void CaptivePortal::dropApForSta(uint8_t kind) {
    if (WiFi.softAPIP()[0] == 0 && !apUp) {
        return;
    }
    stopAp();
    // Health-driven drops must not inherit a prior softAP() fail latch —
    // otherwise Hotspot+STA stays dark for the whole association.
    apGaveUpThisSta = false;
    apFailStreak = 0;
    if (kind == 2) {
        lanNoteApYieldForStaTx();
        // SoftAP back in ~2.5s so hotspot-air stays usable.
        lastApTryMs = millis() - kApRetryMs + 2500;
        debugLog("ap: yielded for STA TX — SoftAP retry soon");
    } else {
        lanNoteApForcedDown();
        lastApTryMs = millis();
        debugLog("ap: forced down for STA — SoftAP retry after backoff");
    }
}

void CaptivePortal::startDns() {
    if (dnsUp || WiFi.softAPIP()[0] == 0) {
        return;
    }
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dnsUp = dns.start(53, "*", WiFi.softAPIP());
}

void CaptivePortal::stopDns() {
    if (!dnsUp) {
        return;
    }
    dns.stop();
    dnsUp = false;
}

void CaptivePortal::tick() {
    flushWanted();
    runPendingStaJoin();
    // API set apForceTry — raise SoftAP every loop, not only on the 1 Hz
    // radio tick, so L1's 35s hotspot wait sees retries under BLE coex.
    if (apForceTry && apShouldRun()) {
        ensureAp();
    }
    if (const uint8_t apDown = lanTakeApForcedDown()) {
        // LanKeep softAPdisconnect + STA (HTTP wedge or SPA yield). Clear the
        // latch so ensureAp calls softAP() again after backoff.
        apUp = false;
        stopDns();
        if (apDown == 2) {
            // SPA yield: SoftAP back in ~2.5s so hotspot-air stays usable.
            lastApTryMs = millis() - kApRetryMs + 2500;
            debugLog("ap: yielded for STA TX — SoftAP retry soon");
        } else {
            lastApTryMs = millis();
            debugLog("ap: forced down by LanKeep — SoftAP retry after backoff");
        }
    }
    // SoftAP was blocked while BLE was live; when the pedal drops, allow one
    // more SoftAP attempt without requiring a STA reassociate.
    {
        static bool bleWasLive = false;
        const bool ble = lanBleLive();
        if (bleWasLive && !ble) {
            apGaveUpThisSta = false;
            apFailStreak = 0;
            lastApTryMs = 0;
            debugLog("ap: BLE idle — SoftAP may retry");
        }
        bleWasLive = ble;
    }
    if ((int32_t)(millis() - lastModeMs) >= 1000) {
        lastModeMs = millis();
        // Don't bring SoftAP back while a deferred STA join is queued —
        // ensureAp() disconnects STA mid-handshake.
        if (staUp()) {
            staJoinDeadlineMs = 0;
        }
        // Once STA is associated, SoftAP may start (Hotspot+STA). Join quiet /
        // deadline only suppress SoftAP while the handshake is still open —
        // otherwise apAlways sits true for ~90s with SoftAP never raised.
        const bool joinInFlight = pendingStaJoin ||
            (!staUp() && (
                (staJoinDeadlineMs != 0 && (int32_t)(millis() - staJoinDeadlineMs) < 0) ||
                lanStaJoinQuiet()));
        if (!joinInFlight && !staUp()) {
            adoptFallbackAp();
            // apAuto may be off from a prior provision POST; still raise the
            // hotspot after a minute without LAN so PSK can be corrected.
            if (!apWanted && !apAlways && millis() > 120000) {
                apWanted = true;
                persistWanted();
                debugLog("ap recovery on, sta still down");
            }
        }
        if (!joinInFlight) {
            if (apShouldRun()) {
                ensureAp();
            } else if (!portalOpen()) {
                stopAp();
            } else {
                stopDns();
            }
        }
    }
    if (dnsUp) {
        dns.processNextRequest();
    }
    if (scanAsked) {
        applyScan();
    }
}

void CaptivePortal::requestScan() {
    scanAsked = true;
    scanReady = false;
}

void CaptivePortal::applyScan() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        return;
    }
    if (n >= 0) {
        scanAsked = false;
        scanCount = n;
        scanReady = true;
        return;
    }
    WiFi.scanNetworks(true, true);
}

bool CaptivePortal::scanJson(String& json, String& error) {
    if (!scanReady) {
        requestScan();
        error = "Scanning.";
        json = "{\"scanning\":true,\"networks\":[]}";
        return true;
    }
    JsonDocument doc;
    doc["scanning"] = false;
    JsonArray list = doc["networks"].to<JsonArray>();
    int n = scanCount;
    if (n < 0) {
        n = 0;
    }
    for (int i = 0; i < n && i < 24; i++) {
        JsonObject row = list.add<JsonObject>();
        row["ssid"] = WiFi.SSID(i);
        row["rssi"] = WiFi.RSSI(i);
        row["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        row["channel"] = WiFi.channel(i);
    }
    serializeJson(doc, json);
    WiFi.scanDelete();
    scanReady = false;
    scanCount = 0;
    return true;
}

String CaptivePortal::statusJson() const {
    JsonDocument doc;
    doc["sta"] = staUp();
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    // softAPIP alone lied during ghost SoftAP (IP set, mode STA / no beacons).
    doc["ap"] = apIfaceLive(0);
    doc["apWanted"] = apWanted;
    doc["apAlways"] = apAlways;
    doc["apAuto"] = apAuto;
    doc["autoConnect"] = autoConnect;
    doc["apSsid"] = kApSsid;
    doc["apPassword"] = kApPass;
    doc["apIp"] = WiFi.softAPIP().toString();
    doc["apStations"] = (int)WiFi.softAPgetStationNum();
    // Hotspot client addresses: station MACs from the WiFi driver, resolved
    // through the DHCP server's lease table.
    {
        wifi_sta_list_t stations = {};
        if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0) {
            const int kMaxClients = 8;
            esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            esp_netif_pair_mac_ip_t pairs[kMaxClients] = {};
            for (int i = 0; i < stations.num && i < kMaxClients; i++) {
                memcpy(pairs[i].mac, stations.sta[i].mac, 6);
            }
            if (apNetif != nullptr
                && esp_netif_dhcps_get_clients_by_mac(apNetif, stations.num, pairs) == ESP_OK) {
                JsonArray clients = doc["apClients"].to<JsonArray>();
                for (int i = 0; i < stations.num && i < kMaxClients; i++) {
                    if (pairs[i].ip.addr == 0) {
                        continue;
                    }
                    JsonObject row = clients.add<JsonObject>();
                    char mac[18] = {};
                    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                        pairs[i].mac[0], pairs[i].mac[1], pairs[i].mac[2],
                        pairs[i].mac[3], pairs[i].mac[4], pairs[i].mac[5]);
                    row["mac"] = mac;
                    char ipStr[16] = {};
                    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
                        (unsigned)((pairs[i].ip.addr) & 0xFF),
                        (unsigned)((pairs[i].ip.addr >> 8) & 0xFF),
                        (unsigned)((pairs[i].ip.addr >> 16) & 0xFF),
                        (unsigned)((pairs[i].ip.addr >> 24) & 0xFF));
                    row["ip"] = ipStr;
                }
            }
        }
    }
    doc["portal"] = portalOpen();
    doc["radio"] = radioOn();
    JsonArray nets = doc["remembered"].to<JsonArray>();
    for (uint8_t i = 0; i < rememberedN; i++) {
        JsonObject row = nets.add<JsonObject>();
        row["ssid"] = remembered[i].ssid;
        row["saved"] = remembered[i].pass.length() > 0;
        row["current"] = staUp() && remembered[i].ssid == WiFi.SSID();
    }
    String json;
    serializeJson(doc, json);
    return json;
}

bool CaptivePortal::connectSta(const String& ssid, const String& password, String& error) {
    if (ssid.length() == 0) {
        error = "SSID is required.";
        return false;
    }
    if (!radioOn()) {
        error = "Wi-Fi radio is off.";
        return false;
    }
    String pass = password;
    if (pass.length() == 0) {
        pass = passwordFor(ssid);
    }
    rememberNet(ssid, pass);
    WiFi.setAutoReconnect(autoConnect);
    // Defer the radio flip so SoftAP can finish the HTTP response that
    // queued this join. SoftAP stays suppressed via pendingStaJoin /
    // lanNoteStaJoin (tick joinInFlight) — do not clear apWanted/apAuto or
    // Hotspot+STA never returns after a successful join.
    pendingStaSsid = ssid;
    pendingStaPass = pass;
    pendingStaJoin = true;
    pendingStaJoinAtMs = millis() + 800;
    lanNoteStaJoin(30000);
    return true;
}

void CaptivePortal::runPendingStaJoin() {
    if (!pendingStaJoin) {
        return;
    }
    if ((int32_t)(millis() - pendingStaJoinAtMs) < 0) {
        return;
    }
    pendingStaJoin = false;
    const String ssid = pendingStaSsid;
    const String pass = pendingStaPass;
    pendingStaSsid = "";
    pendingStaPass = "";
    // STA-only join. AP+STA plus LanKeep WiFi.reconnect() while status was
    // still WL_DISCONNECTED aborted the handshake after NVS erase.
    stopAp();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("showbox");
    // Do not call WiFi.disconnect() first. Arduino's timed disconnect
    // returns immediately when already disconnected (it only waits while
    // connected()), but esp_wifi_disconnect() still posts ASSOC_LEAVE
    // (reason 8) which then arrives after WiFi.begin() and cancels it.
    lanNoteStaJoin(90000);
    staJoinDeadlineMs = millis() + 90000;
    debugLog("sta join begin ssid=%s passLen=%u", ssid.c_str(), (unsigned)pass.length());
    wl_status_t st = WiFi.begin(ssid.c_str(), pass.c_str());
    debugLog("sta begin status=%d", (int)st);
}
