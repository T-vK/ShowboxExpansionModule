#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <IPAddress.h>

class UltimateBaseFirmware;
class AsyncWebServer;

// Gig default is SoftAP-only (phone joins ShowboxMod). STA/Ethernet are
// explicit desk/dev modes — see docs/net-mode-softap-default.md.
enum class NetMode : uint8_t { SoftAp = 0, Sta = 1, Ethernet = 2 };

class CaptivePortal {
public:
    void begin(AsyncWebServer* server, UltimateBaseFirmware* firmware);
    void tick();
    void ensureAp();
    void stopAp();
    // Sole SoftAP teardown entry for LanKeep (HTTP wedge / SPA yield / gw zombie).
    // kind 1 = forced down (long backoff), 2 = yield for STA TX (short retry).
    void dropApForSta(uint8_t kind);
    bool setWantedAp(bool on);
    bool setAlwaysAp(bool on);
    bool setAutoAp(bool on);
    bool setAutoConnect(bool on);
    bool forgetSta(const String& ssid);
    bool wantedAp() const { return apWanted; }
    bool alwaysAp() const { return apAlways; }
    bool autoAp() const { return apAuto; }
    bool autoConnectOn() const { return autoConnect; }
    NetMode netMode() const { return mode; }
    bool setNetMode(NetMode next);
    NetMode cycleNetMode();
    static const char* netModeName(NetMode m);
    static bool parseNetMode(const String& raw, NetMode& out);
    bool fromAccessPoint(IPAddress local) const;
    String statusJson() const;
    bool connectSta(const String& ssid, const String& password, String& error);
    bool scanJson(String& json, String& error);
    void requestScan();

private:
    struct NetCred {
        String ssid;
        String pass;
    };

    void startDns();
    void stopDns();
    // DHCP option 114 (RFC 8910). The API says a login is required and
    // points at the web UI, which is what makes Android open the page.
    // The gateway stays advertised: without one, Android skips that check.
    void advertiseCapport();
    void applyScan();
    bool radioOn() const;
    bool staUp() const;
    bool portalOpen() const;
    bool apShouldRun() const;
    // SoftAP IP + AP/APSTA mode + ShowboxMod SSID (+ optional channel match).
    bool apIfaceLive(uint8_t expectChannel) const;
    bool apSsidOnAir() const;
    void bindApAddress();
    void persistWanted();
    void flushWanted();
    void adoptFallbackAp();
    void loadPrefs();
    void rememberNet(const String& ssid, const String& password);
    String passwordFor(const String& ssid) const;
    void runPendingStaJoin();
    void applyNetMode(bool boot);

    DNSServer dns;
    UltimateBaseFirmware* firmware = nullptr;
    bool dnsUp = false;
    bool apUp = false;
    bool capportSet = false;
    uint32_t capportTryMs = 0;
    NetMode mode = NetMode::SoftAp;
    bool apWanted = true;
    bool apAlways = false;
    bool apAuto = true;
    bool autoConnect = false; // SoftAP-default: do not auto-join home STA
    bool apWantedDirty = false;
    bool scanAsked = false;
    bool scanReady = false;
    bool pendingStaJoin = false;
    // setWantedAp/setAlwaysAp: skip the boot SoftAP settle window.
    bool apForceTry = false;
    int scanCount = 0;
    uint32_t lastModeMs = 0;
    uint32_t lastApTryMs = 0;
    uint32_t pendingStaJoinAtMs = 0;
    uint32_t staJoinDeadlineMs = 0;
    static constexpr uint32_t kApRetryMs = 15000;
    // Automatic SoftAP while STA is up waits this long after boot so L1
    // uart/mdns/telnet are not racing softAP() coex wedges.
    static constexpr uint32_t kApBootSettleMs = 90000;
    NetCred remembered[8];
    uint8_t rememberedN = 0;
    String pendingStaSsid;
    String pendingStaPass;
};
