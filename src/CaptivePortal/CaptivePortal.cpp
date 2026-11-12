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
#include <esp_heap_caps.h>

namespace {
constexpr const char* kApSsid = "ShowboxMod";
constexpr const char* kApPass = "showboxmod";
constexpr uint32_t kFallbackApMs = 8000;
// Fixed SoftAP address. The DHCP server keeps this pointer for its lifetime.
constexpr char kCapportUri[] = "http://192.168.4.1/captive-portal";
}

const char* CaptivePortal::netModeName(NetMode m) {
    switch (m) {
    case NetMode::SoftAp:
        return "softap";
    case NetMode::Sta:
        return "sta";
    case NetMode::Ethernet:
        return "ethernet";
    }
    return "softap";
}

bool CaptivePortal::parseNetMode(const String& raw, NetMode& out) {
    String s = raw;
    s.trim();
    s.toLowerCase();
    if (s == "softap" || s == "ap" || s == "hotspot") {
        out = NetMode::SoftAp;
        return true;
    }
    if (s == "sta" || s == "station" || s == "wifi") {
        out = NetMode::Sta;
        return true;
    }
    if (s == "ethernet" || s == "eth" || s == "lan") {
        out = NetMode::Ethernet;
        return true;
    }
    return false;
}

void CaptivePortal::loadPrefs() {
    Preferences prefs;
    if (!prefs.begin("radio", true)) {
        mode = NetMode::SoftAp;
        apWanted = true;
        autoConnect = false;
        return;
    }
    // Missing key → SoftAP (gig default). Explicit 0/1/2 wins over legacy flags.
    mode = static_cast<NetMode>(prefs.getUChar("netmode", static_cast<uint8_t>(NetMode::SoftAp)));
    if (mode > NetMode::Ethernet) {
        mode = NetMode::SoftAp;
    }
    apWanted = prefs.getBool("hotspot", true);
    apAlways = prefs.getBool("apalways", false);
    apAuto = prefs.getBool("apauto", true);
    // SoftAP-default: stauto defaults false so reboot does not chase home STA.
    autoConnect = prefs.getBool("stauto", mode == NetMode::Sta);
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
    // Temporary SoftAP yields used to persist apWanted=false; Hotspot+STA
    // must still raise SoftAP after reboot.
    if (apAlways && !apWanted) {
        apWanted = true;
        persistWanted();
    }
}

void CaptivePortal::applyNetMode(bool boot) {
    switch (mode) {
    case NetMode::SoftAp:
        autoConnect = false;
        apAlways = false;
        apWanted = true;
        apAuto = true;
        pendingStaJoin = false;
        WiFi.setAutoReconnect(false);
        if (staUp()) {
            WiFi.disconnect(false);
        }
        if (!boot || radioOn()) {
            ensureAp();
        }
        break;
    case NetMode::Sta:
        autoConnect = true;
        apAlways = false;
        apAuto = true;
        WiFi.setAutoReconnect(true);
        if (radioOn() && rememberedN > 0 && remembered[0].ssid.length() > 0) {
            if (!apAlways) {
                apWanted = false;
            }
            pendingStaSsid = remembered[0].ssid;
            pendingStaPass = remembered[0].pass;
            pendingStaJoin = true;
            pendingStaJoinAtMs = millis() + (boot ? 800 : 200);
            lanNoteStaJoin(90000);
            staJoinDeadlineMs = millis() + 90000;
            debugLog("sta join queued mode=%s ssid=%s", netModeName(mode), pendingStaSsid.c_str());
        } else {
            apWanted = true;
            ensureAp();
        }
        break;
    case NetMode::Ethernet:
        autoConnect = false;
        apAlways = false;
        apWanted = false;
        apAuto = false;
        pendingStaJoin = false;
        WiFi.setAutoReconnect(false);
        stopAp();
        if (staUp()) {
            WiFi.disconnect(false);
        }
        break;
    }
    persistWanted();
}

bool CaptivePortal::setNetMode(NetMode next) {
    if (next > NetMode::Ethernet) {
        return false;
    }
    if (mode == next) {
        applyNetMode(false);
        return true;
    }
    debugLog("netMode %s -> %s", netModeName(mode), netModeName(next));
    mode = next;
    applyNetMode(false);
    return true;
}

NetMode CaptivePortal::cycleNetMode() {
    NetMode next = NetMode::SoftAp;
    switch (mode) {
    case NetMode::SoftAp:
        next = NetMode::Sta;
        break;
    case NetMode::Sta:
        next = NetMode::Ethernet;
        break;
    case NetMode::Ethernet:
        next = NetMode::SoftAp;
        break;
    }
    setNetMode(next);
    return mode;
}

void CaptivePortal::begin(AsyncWebServer* /*server*/, UltimateBaseFirmware* firmwareIn) {
    firmware = firmwareIn;
    loadPrefs();
    debugLog("portal begin mode=%s radio=%d auto=%d nets=%u apWanted=%d apAlways=%d",
        netModeName(mode), radioOn() ? 1 : 0, autoConnect ? 1 : 0, (unsigned)rememberedN,
        apWanted ? 1 : 0, apAlways ? 1 : 0);
    applyNetMode(true);
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
    if (mode == NetMode::Ethernet) {
        return false;
    }
    // SoftAP-only gig mode: keep the phone hotspot up even with BLE MIDI.
    if (mode == NetMode::SoftAp) {
        return apWanted || (apAuto && millis() > kFallbackApMs);
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
    prefs.putUChar("netmode", static_cast<uint8_t>(mode));
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
    // Hotspot+STA (apAlways): POST ap=false is a temporary SoftAP yield for
    // STA HTTP (L1 after air scan), not a permanent prefs change. Persisting
    // apWanted=false left SoftAP dark across reboot for the whole association.
    if (!on && apAlways) {
        apForceTry = false;
        // Back off SoftAP retries so STA HTTP can recover; tick will raise
        // SoftAP again after kApRetryMs because apWanted stays true.
        lastApTryMs = millis();
        stopAp();
        debugLog("ap: temporary SoftAP yield (apAlways) — prefs keep wanted");
        return true;
    }
    apWanted = on;
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
    // treat SSID+mode as live only when the AP address is up so clients
    // can open the portal. Callers that see SSID-without-IP should
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
    // Binding the address heals L1 without softAPdisconnect thrash.
    if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
        debugLog("ap ssid on air without IP — bindApAddress");
        bindApAddress();
        delay(40);
    }
    // Ghost SoftAP: latched apUp but mode/SSID drifted — clear for restart.
    if (apUp && !apIfaceLive(channel) && !apSsidOnAir()) {
        debugLog("ap ghost (mode/ssid/ch) — clearing latch for softAP restart");
        apUp = false;
        stopDns();
        lanNoteApActive(false);
    }
    if (apIfaceLive(channel)) {
        apUp = true;
        apForceTry = false;
    } else if (!apUp || WiFi.softAPIP()[0] == 0) {
        // Automatic SoftAP while STA is up waits out the boot settle window so
        // L1 uart/mdns/telnet are not racing softAP() coex wedges. Explicit
        // API/UI (apForceTry) skips the wall — hotspot POST still brings it up.
        if (!apForceTry && staUp() && lastApTryMs == 0 && millis() < kApBootSettleMs) {
            return;
        }
        // Explicit API force: retry every 2s so L1's hotspot wait sees SoftAP.
        const uint32_t backoff = apForceTry ? 2000u : kApRetryMs;
        if (lastApTryMs != 0 && (int32_t)(millis() - lastApTryMs) < (int32_t)backoff) {
            return;
        }
        lastApTryMs = millis();
        // Never WiFi.disconnect() while STA is up — that drops the home AP
        // every time SoftAP starts (Hotspot+STA). Only bounce the radio when
        // there is no LAN association to protect.
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
        // then continue in this tick — deferring forever left SoftAP dark
        // when hostConnected() stayed true after dropHostLink().
        if (lanYieldBleForSoftAp()) {
            delay(30);
        }
        // Already advertising ShowboxMod — do not softAP() again; bind IP.
        if (apSsidOnAir()) {
            bindApAddress();
            for (uint8_t i = 0; i < 5 && WiFi.softAPIP()[0] == 0; i++) {
                delay(20);
            }
            debugLog("ap reclaim ssid ip=%s", WiFi.softAPIP().toString().c_str());
        } else {
            capportSet = false;
            capportTryMs = 0;
            const bool began = WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
            debugLog("ap begin=%d ip=%s ch=%u wanted=%d always=%d ssid=%s", began ? 1 : 0,
                WiFi.softAPIP().toString().c_str(), (unsigned)channel, apWanted ? 1 : 0,
                apAlways ? 1 : 0, WiFi.softAPSSID().c_str());
            for (uint8_t i = 0; i < 5 && (!apSsidOnAir() || WiFi.softAPIP()[0] == 0); i++) {
                delay(20);
            }
            if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
                bindApAddress();
                delay(20);
            }
            if (began && !apIfaceLive(channel) && !apSsidOnAir()) {
                debugLog("ap begin partial (ssid=%s) — reassert", WiFi.softAPSSID().c_str());
                capportSet = false;
                capportTryMs = 0;
                WiFi.softAP(kApSsid, kApPass, channel, 0, 2);
                delay(20);
                if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
                    bindApAddress();
                }
            }
        }
        if (!apIfaceLive(channel)) {
            apUp = false;
            // Only tear down when the ShowboxMod beacon is NOT on air —
            // disconnecting a beacon-only SoftAP left apIp=0 while air still
            // saw ShowboxMod and STA HTTP wedged on retry.
            if (staUp() && !apSsidOnAir()) {
                WiFi.softAPdisconnect(false);
            }
            debugLog(apAlways
                ? "ap begin failed — STA kept, SoftAP retry later (always)"
                : "ap begin failed — SoftAP abandoned this try");
            lanNoteApActive(false);
            stopDns();
            return;
        }
        apUp = true;
        apForceTry = false;
        // Prefer WiFi airtime right after softAP — otherwise the next STA
        // HTTP times out while BLE coex catches up.
        lanPreferWifiAirtime();
    }
    // A bare mode switch can leave the default ESP_XXXX beacon up if softAP
    // lost its config; re-assert the hotspot identity.
    if (WiFi.softAPSSID() != kApSsid) {
        debugLog("ap ssid drift -> %s, reasserting", WiFi.softAPSSID().c_str());
        capportSet = false;
        capportTryMs = 0;
        if (!WiFi.softAP(kApSsid, kApPass, channel, 0, 2) || WiFi.softAPIP()[0] == 0) {
            if (apSsidOnAir() && WiFi.softAPIP()[0] == 0) {
                bindApAddress();
            }
            if (!apIfaceLive(channel)) {
                apUp = false;
                return;
            }
        }
    }
    lanNoteApActive(true);
    startDns();
    advertiseCapport();
}

void CaptivePortal::stopAp() {
    capportSet = false;
    capportTryMs = 0;
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
    // SoftAP-only gig mode: never tear the phone hotspot for STA TX health.
    if (mode == NetMode::SoftAp) {
        return;
    }
    if (WiFi.softAPIP()[0] == 0 && !apUp) {
        return;
    }
    stopAp();
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

void CaptivePortal::advertiseCapport() {
    if (capportSet || WiFi.softAPIP()[0] == 0) {
        return;
    }
    // ensureAp runs about once a second. A failed attempt must not stop and
    // start the DHCP server on every tick.
    if (capportTryMs != 0 && (int32_t)(millis() - capportTryMs) < 15000) {
        return;
    }
    capportTryMs = millis();
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif == nullptr) {
        return;
    }
    const esp_err_t stopped = esp_netif_dhcps_stop(netif);
    if (stopped != ESP_OK && stopped != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        debugLog("ap capport stop %d", (int)stopped);
        return;
    }
    // The server stores this pointer and reads it on each offer.
    // Leave the default gateway in place. Android does not open the portal
    // on a network that has no route.
    char* uri = const_cast<char*>(kCapportUri);
    const esp_err_t set = esp_netif_dhcps_option(
        netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, sizeof(kCapportUri) - 1);
    const esp_err_t started = esp_netif_dhcps_start(netif);
    if (set == ESP_OK && (started == ESP_OK || started == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)) {
        capportSet = true;
        debugLog("ap capport %s", kCapportUri);
        return;
    }
    debugLog("ap capport set=%d start=%d", (int)set, (int)started);
    if (started != ESP_OK && started != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        esp_netif_dhcps_start(netif);
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
    // API set apForceTry — raise SoftAP from the main loop. Do not gate on
    // lanHttpBodyBusy(): continuous /wifi poll keeps httpBodies>0 so
    // quietMs never hits 10s and SoftAP stayed dark for the whole wait.
    if (apForceTry && apShouldRun()) {
        ensureAp();
    }
    if (const uint8_t apDown = lanTakeApForcedDown()) {
        // Fallback path: LanKeep tore SoftAP down without a portal pointer.
        // Clear the latch so ensureAp calls softAP() again after backoff.
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
    doc["netMode"] = netModeName(mode);
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
    doc["heapLargest"] = (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    doc["heapFree"] = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
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
    // Explicit STA join from UI/API leaves SoftAP-only gig mode.
    mode = NetMode::Sta;
    autoConnect = true;
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
