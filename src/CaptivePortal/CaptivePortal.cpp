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
    apAlways = prefs.getBool("apalways", false);
    apAuto = prefs.getBool("apauto", true);
    autoConnect = prefs.getBool("stauto", true);
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
}

void CaptivePortal::begin(AsyncWebServer* /*server*/, UltimateBaseFirmware* firmwareIn) {
    firmware = firmwareIn;
    loadPrefs();
    WiFi.setAutoReconnect(autoConnect);
    // WiFiManager no longer autoConnects. Kick STA from remembered nets so
    // reboot restores LAN without SoftAP thrash. Keep SoftAP off during the
    // join — ensureAp() mid-handshake was aborting WiFi.begin().
    debugLog("portal begin radio=%d auto=%d nets=%u apWanted=%d",
        radioOn() ? 1 : 0, autoConnect ? 1 : 0, (unsigned)rememberedN, apWanted ? 1 : 0);
    if (radioOn() && autoConnect && rememberedN > 0 && remembered[0].ssid.length() > 0) {
        apWanted = false;
        persistWanted();
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
    // AP+STA plus BLE makes showbox.local unusable on the LAN unless the
    // user explicitly wants the hotspot always on.
    if (staUp() && !(apAlways && apWanted)) {
        return false;
    }
    // JTAG: STA blip + default hotspot + AIRSTEP called softAP every 1s.
    // begin() blocked loop() for 1000ms, returned 0, and WiFi.mode(AP)
    // left status=254 / reconnect=0. Keep STA recovering while a pedal is up.
    if (lanBleLive() && !apAlways) {
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
    String packed;
    for (uint8_t i = 0; i < rememberedN; i++) {
        packed += remembered[i].ssid;
        packed += '\t';
        packed += remembered[i].pass;
        packed += '\n';
    }
    size_t wrote = prefs.putString("nets", packed);
    debugLog("radio prefs flush ap=%d nets=%u packed=%u wrote=%u",
        apWanted ? 1 : 0, (unsigned)rememberedN, (unsigned)packed.length(), (unsigned)wrote);
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
    persistWanted();
    if (on) {
        ensureAp();
    } else {
        stopAp();
    }
    return true;
}

bool CaptivePortal::setAlwaysAp(bool on) {
    apAlways = on;
    persistWanted();
    if (on && apWanted) {
        ensureAp();
    } else if (!apShouldRun() && !portalOpen()) {
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

void CaptivePortal::ensureAp() {
    if (!apShouldRun()) {
        if (!portalOpen()) {
            stopAp();
        } else {
            stopDns();
        }
        return;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    // Never drop to AP-only when STA credentials or auto-reconnect exist.
    // That is what froze status=254 after a BLE-era STA blip.
    const bool keepSta = autoConnect || rememberedN > 0 || WiFi.SSID().length() > 0 || staUp();
    if (keepSta) {
        if (mode != WIFI_MODE_APSTA) {
            WiFi.mode(WIFI_AP_STA);
        }
    } else if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        WiFi.mode(WIFI_AP);
    }
    uint8_t channel = 1;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&channel, &second);
    if (channel == 0) {
        channel = 1;
    }
    if (!apUp || WiFi.softAPIP()[0] == 0) {
        if (lastApTryMs != 0 && (int32_t)(millis() - lastApTryMs) < (int32_t)kApRetryMs) {
            return;
        }
        lastApTryMs = millis();
        // Don't leave STA mid-handshake when bringing SoftAP up — that is
        // what turned ShowboxMod into ESP_XXXX on the air.
        WiFi.disconnect(false, false);
        delay(30);
        apUp = WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
        debugLog("ap begin=%d ip=%s ch=%u wanted=%d ssid=%s", apUp ? 1 : 0,
            WiFi.softAPIP().toString().c_str(), (unsigned)channel, apWanted ? 1 : 0,
            WiFi.softAPSSID().c_str());
        if (!apUp) {
            return;
        }
    }
    // A bare mode switch can leave the default ESP_XXXX beacon up if softAP
    // lost its config; re-assert the hotspot identity.
    if (WiFi.softAPSSID() != kApSsid) {
        debugLog("ap ssid drift -> %s, reasserting", WiFi.softAPSSID().c_str());
        WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
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
    if ((int32_t)(millis() - lastModeMs) >= 1000) {
        lastModeMs = millis();
        // Don't bring SoftAP back while a deferred STA join is queued —
        // ensureAp() disconnects STA mid-handshake.
        if (staUp()) {
            staJoinDeadlineMs = 0;
        }
        const bool joinInFlight = pendingStaJoin ||
            (staJoinDeadlineMs != 0 && (int32_t)(millis() - staJoinDeadlineMs) < 0) ||
            lanStaJoinQuiet();
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
    doc["ap"] = WiFi.softAPIP()[0] != 0;
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
    // queued this join. Stopping AP here made curl see HTTP 000 and left
    // STA wedged in "connecting" when LanKeep was already mid-handshake.
    apWanted = false;
    apAuto = false; // don't raise SoftAP mid-join via adoptFallbackAp()
    // Only mark dirty here — Preferences/NVS must not run on the AsyncTCP
    // callback (spi_flash cache assert). tick()/flushWanted() writes on loop.
    persistWanted();
    pendingStaSsid = ssid;
    pendingStaPass = pass;
    pendingStaJoin = true;
    // Give loop() time to flush NVS before runPendingStaJoin.
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
