#include "CaptivePortal.h"

#include "DebugLog.h"
#include "System/LanKeep.h"
#include <UltimateBaseFirmware.h>
#include <WiFi.h>
#include <esp_wifi.h>
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
    prefs.putString("nets", packed);
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
    if (!on && radioOn() && !staUp() && !apAlways) {
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
        apUp = WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
        debugLog("ap begin=%d ip=%s ch=%u wanted=%d", apUp ? 1 : 0, WiFi.softAPIP().toString().c_str(), (unsigned)channel, apWanted ? 1 : 0);
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
    if ((int32_t)(millis() - lastModeMs) >= 1000) {
        lastModeMs = millis();
        adoptFallbackAp();
        if (apShouldRun()) {
            ensureAp();
        } else if (!portalOpen()) {
            stopAp();
        } else {
            stopDns();
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
    WiFi.mode(apShouldRun() || apWanted || apAlways ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname("showbox");
    WiFi.disconnect(false, false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    return true;
}
