#include "System/LanKeep.h"

#include "System/HttpsGate.h"
#include "FrontPanel/FrontPanel.h"
#include "MackieShowbox/MackieShowbox.h"
#include "WiredNet/WiredNet.h"
#include "DebugLog.h"
#include "System/Diag.h"
#include "MultiMidi/MultiMidi.h"

// UART-safe restart: the Showbox base wedges when the module reboots
// mid-transaction (blind watchdog reboots soft-bricked it repeatedly on
// 2026-10-13/14). Draining the UART first makes the reboot survivable —
// the iso ladder has done waitQuiet-then-restart all along without ever
// bricking the base.
static void uartSafeRestart(const char* why) {
    extern MackieShowbox showbox;
    debugLog("CRITICAL uart-safe restart: %s", why);
    diagEventForce("loop", "uart-safe restart: %s", why);
    if (showbox.uartHot()) {
        showbox.waitQuiet(40);
    }
    delay(50);
    ESP.restart();
}
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <cstring>
#include "esp_private/wifi.h"
#include "ping/ping_sock.h"

extern "C" {
#include "lwip/etharp.h"
#include "lwip/igmp.h"
#include "lwip/ip4.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"
}

volatile bool gMdnsUp = false;
volatile bool gSetupDone = false;

namespace {
unsigned long lastWifiFix = 0;
unsigned long lastMdnsRefresh = 0;
bool wifiWasDown = false;
volatile bool mdnsRefreshQueued = false;
volatile uint32_t lastHttpMs = 0;
volatile uint32_t firstHttpMs = 0;
volatile int httpBodies = 0;
volatile int bigHttpBodies = 0;
volatile uint32_t lastBigHttpMs = 0;
volatile bool arpKickQueued = false;
volatile bool httpRestartQueued = false;
volatile bool rejoinQueued = false;
bool bleModemSleep = false;
bool wifiEventsOn = false;
uint32_t lastArpMs = 0;
volatile bool bleLive = false;
volatile int wsClients = 0;
volatile uint32_t lastWsRxMs = 0;
uint32_t lastRejoinMs = 0;
uint32_t lastStuckMs = 0;
uint32_t lastGapMs = 0;
uint8_t staProto = 0;
bool abortTried = false;
}

// JTAG 2026-10-11 (HTTP dead, ICMP up, wifiWasDown=0): listen :80 still
// present. Active :80 PCBs in FIN_WAIT_1/CLOSING with unsent!=0,
// unacked==nil, nrtx=2..3. tcp_output never handed segments to the MAC.
// The old detector required nrtx>=4 on 2 PCBs after the 12s GAP window,
// so it never fired. Then a later WiFi.disconnect left wifiWasDown=1
// and killed ICMP.
static bool lanPcbTxZombie(const struct tcp_pcb* pcb) {
    if (pcb == nullptr) {
        return false;
    }
    if (pcb->unsent == nullptr || pcb->unacked != nullptr) {
        return false;
    }
    // JTAG: FIN_WAIT_1/CLOSING, unsent set, unacked nil, nrtx>=2.
    if (pcb->state >= FIN_WAIT_1 && pcb->nrtx >= 2) {
        return true;
    }
    // JTAG 2026-10-11 reload: ESTABLISHED :80 nrtx=7 unsent set
    // unacked nil while AIRSTEP MIDI notifies. Small HTTP still
    // ACKed. nrtx>=2 on ESTABLISHED was too eager (killed /services).
    // nrtx>=8 (2026-10-15) let dead transfer PCBs sit for minutes
    // draining internal RAM and starving later transfers — the browser
    // saw ~100% mid-body stalls while clean single transfers passed.
    // The webui shell now resumes aborted transfers via Range, so an
    // aggressive abort costs the client one resume, not the page.
    return pcb->state == ESTABLISHED && pcb->nrtx >= 4;
}

static bool lanUnicastTxStuck() {
    int stuck = 0;
    LOCK_TCPIP_CORE();
    for (struct tcp_pcb* pcb = tcp_active_pcbs; pcb != nullptr; pcb = pcb->next) {
        if (lanPcbTxZombie(pcb)) {
            stuck++;
        }
    }
    UNLOCK_TCPIP_CORE();
    return stuck > 0;
}

static int lanAbortStuckPcbs() {
    int n = 0;
    LOCK_TCPIP_CORE();
    struct tcp_pcb* pcb = tcp_active_pcbs;
    while (pcb != nullptr) {
        struct tcp_pcb* next = pcb->next;
        if (lanPcbTxZombie(pcb)) {
            tcp_abort(pcb);
            n++;
        }
        pcb = next;
    }
    UNLOCK_TCPIP_CORE();
    return n;
}

static ip4_addr_t ip4From(const IPAddress& ip) {
    ip4_addr_t addr;
    IP4_ADDR(&addr, ip[0], ip[1], ip[2], ip[3]);
    return addr;
}

static struct netif* staLwipNetifLocked(const IPAddress& ip) {
    ip4_addr_t want = ip4From(ip);
    if (want.addr == 0) {
        return nullptr;
    }
    struct netif* n = nullptr;
    NETIF_FOREACH(n) {
        if (ip4_addr_cmp(netif_ip4_addr(n), &want)) {
            return n;
        }
    }
    return nullptr;
}

static bool mdnsStaBound = false;
static bool mdnsIgmpJoined = false;
static uint32_t lastIgmpReportMs = 0;

static void staJoinMdnsGroup() {
    IPAddress ip = WiFi.localIP();
    ip4_addr_t ifaddr = ip4From(ip);
    if (ifaddr.addr == 0) {
        return;
    }
    ip4_addr_t group;
    IP4_ADDR(&group, 224, 0, 0, 251);
    LOCK_TCPIP_CORE();
    struct netif* n = staLwipNetifLocked(ip);
    // Do not netif_set_default(STA): AP+STA still needs the AP default
    // route for the hotspot, and flipping it killed STA ARP. Point IPv4
    // multicast at STA so QM answers leave 192.168.x, not 192.168.4.1.
    err_t err = ERR_IF;
    if (n != nullptr) {
        netif_set_flags(n, NETIF_FLAG_IGMP);
#if LWIP_MULTICAST_TX_OPTIONS
        ip4_set_default_multicast_netif(n);
#endif
        if (igmp_lookfor_group(n, &group) == nullptr) {
            err = igmp_joingroup_netif(n, &group);
            mdnsIgmpJoined = (err == ERR_OK);
        } else {
            err = ERR_OK;
            mdnsIgmpJoined = true;
        }
        if (lastIgmpReportMs == 0 || (int32_t)(millis() - lastIgmpReportMs) >= 15000) {
            igmp_report_groups(n);
            lastIgmpReportMs = millis();
        }
    } else {
        err = igmp_joingroup(&ifaddr, &group);
        mdnsIgmpJoined = (err == ERR_OK);
    }
    UNLOCK_TCPIP_CORE();
    static int igmpLogs = 0;
    if (!mdnsIgmpJoined || igmpLogs < 3) {
        igmpLogs++;
        debugLog("mdns igmp join=%d ok=%d ip=%s", (int)err, mdnsIgmpJoined ? 1 : 0, ip.toString().c_str());
    }
}

static void lanSendGratuitousArp() {
    IPAddress ip = WiFi.localIP();
    LOCK_TCPIP_CORE();
    struct netif* n = staLwipNetifLocked(ip);
    if (n == nullptr) {
        n = netif_default;
    }
    if (n != nullptr && netif_is_up(n) && netif_ip4_addr(n)->addr != 0) {
        etharp_gratuitous(n);
    }
    UNLOCK_TCPIP_CORE();
}

void lanInstallWifiEvents() {
    if (wifiEventsOn) {
        return;
    }
    wifiEventsOn = true;
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_START || event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            lanPreferWifiAirtime();
        }
    });
}

void lanQueueMdnsRefresh() {
    mdnsRefreshQueued = true;
}

static void lanStaPhy() {
    // BLE coex + WIFI_PS_NONE kills the link when AIRSTEP is connected
    // (2026-10-14: 100% loss for 300 s with AIRSTEP on, 0/731 loss with it
    // off; RTT 96 ms avg vs 8 ms). The ESP32 coex scheduler needs WiFi
    // modem sleep to schedule BLE events between DTIM wakes — BUT modem
    // sleep also breaks the link when AIRSTEP is connected (retested with
    // PS_MIN_MODEM: same death cycle). The instability is inherent to the
    // ESP-IDF coex scheduler + this ESP-IDF version + AIRSTEP's connection
    // pattern. Keeping PS_NONE: the pedal link is essential, the webui
    // degrades during BLE-heavy periods, and the gateway-zombie rejoin
    // (when gDiagWatch is enabled) recovers the link automatically.
    // 2026-10-18 RE-TEST on the new framework (IDF coex blob + 400 ms BLE
    // interval + PSRAM NimBLE): the old PS_MIN_MODEM verdict (2026-10-14)
    // predates ALL of that — retesting the documented-correct coex config:
    // modem sleeps between DTIMs and the scheduler slots BLE events into
    // the gaps (Espressif's coexistence model). lanKeepWifi also forces
    // PS_NONE on every associate — changed there too (search WIFI_PS_NONE).
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    // History: 11n + AMPDU-off flapped this AP; 11g+b re-tested 2026-10-13
    // (channel fixed to 6, watchdogs live): RTT improved 45->30 ms but the
    // module hit the gateway-death cycle ~every 40 s and the watchdog reboot
    // looped (boot #275->#295). Reverted to 11b until the root cause is
    // understood — the 11g experiment remains documented in git history.
    // 2026-10-17 re-test of 11g/54M fixed rate with everything else fixed:
    // transfers flew (<1 s for the 37 KB bundle) but the gateway-death cycle
    // returned and the module reboot-looped (boot #54+ reset=SW within
    // minutes) — same verdict as 2026-10-13. That test FORCED 54M; a fixed
    // rate with no adaptation turns every congestion dip into retransmit
    // storms. 2026-10-18: retry 11g with AUTO rate adaptation — if the death
    // cycle still appears, the coex scheduler simply cannot hold OFDM and we
    // stay on 11b for good.
    const uint8_t proto = WIFI_PROTOCOL_11B;
    if (staProto == proto) {
        return;
    }
    esp_wifi_set_protocol(WIFI_IF_STA, proto);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true, WIFI_PHY_RATE_11M_L);
    staProto = proto;
}

void lanPreferWifiAirtime() {
    lanStaPhy();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
}

void lanPreferWifiWithBle() {
    // Stay awake. Do not flip PHY here: dropping 11n after associate
    // leaves the AP sending rates this STA no longer receives.
    lanStaPhy();
    // inactive_time was 30 AP-beacon intervals (~3 s). The module's gateway
    // pings died ~30 s after every association (2026-10-13 watchdog trail),
    // matching that listen interval; with BT coex the STA honours it even
    // under WIFI_PS_NONE. Keep the STA listening every beacon.
    esp_wifi_set_inactive_time(WIFI_IF_STA, 1);
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    bleModemSleep = false;
}

void lanRefreshPhy() {
    staProto = 0;
    lanStaPhy();
}

void lanNoteBleLive(bool on) {
    if (bleLive == on) {
        return;
    }
    bleLive = on;
    if (on) {
        lastGapMs = millis();
        lanPreferWifiWithBle();
        lanKickArp();
    }
}

bool lanBleLive() {
    return bleLive;
}

void lanNoteWsClients(int n) {
    wsClients = n < 0 ? 0 : n;
    if (wsClients == 0) {
        lastWsRxMs = 0;
    }
}

void lanNoteWsRx() {
    lastWsRxMs = millis();
}

int lanWsClients() {
    return wsClients;
}

bool lanWsLive() {
    if (wsClients <= 0 || lastWsRxMs == 0) {
        return false;
    }
    return (int32_t)(millis() - lastWsRxMs) < 20000;
}

void lanNoteHttp() {
    lastHttpMs = millis();
    if (firstHttpMs == 0) {
        firstHttpMs = lastHttpMs;
    }
    // JTAG: every WS/HTTP success used to queue a gratuitous ARP. The
    // debug ring was an ARP storm on the same 2.4 GHz PTA as AIRSTEP.
    if (bleModemSleep) {
        lanPreferWifiAirtime();
        bleModemSleep = false;
    }
}

void lanNoteHttpBody(bool on) {
    if (on) {
        __atomic_add_fetch(&httpBodies, 1, __ATOMIC_RELAXED);
        lanNoteHttp();
        if (bleLive) {
            lanPreferWifiWithBle();
        }
        return;
    }
    int n = __atomic_add_fetch(&httpBodies, -1, __ATOMIC_RELAXED);
    if (n < 0) {
        __atomic_store_n(&httpBodies, 0, __ATOMIC_RELAXED);
    }
}

bool lanHttpBodyBusy() {
    int n = __atomic_load_n(&httpBodies, __ATOMIC_RELAXED);
    if (n <= 0) {
        return false;
    }
    if (lanHttpQuietMs() > 10000) {
        __atomic_store_n(&httpBodies, 0, __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

void lanTouchBigHttp() {
    lastBigHttpMs = millis();
}

void lanNoteBigHttp(bool on) {
    lanTouchBigHttp();
    if (on) {
        __atomic_add_fetch(&bigHttpBodies, 1, __ATOMIC_RELAXED);
        lanNoteHttpBody(true);
        return;
    }
    int n = __atomic_add_fetch(&bigHttpBodies, -1, __ATOMIC_RELAXED);
    if (n < 0) {
        __atomic_store_n(&bigHttpBodies, 0, __ATOMIC_RELAXED);
    }
    lanNoteHttpBody(false);
}

bool lanBigHttpBusy() {
    uint32_t t = lastBigHttpMs;
    if (t != 0 && (int32_t)(millis() - t) < 3000) {
        return true;
    }
    int n = __atomic_load_n(&bigHttpBodies, __ATOMIC_RELAXED);
    if (n > 0 && (t == 0 || (int32_t)(millis() - t) > 8000)) {
        __atomic_store_n(&bigHttpBodies, 0, __ATOMIC_RELAXED);
    }
    return false;
}

bool lanHoldMidi() {
    // Drop CCC only during a large SPA/asset body. STA-up and lanWsLive()
    // used to hold MIDI forever (mixer /ws ping every 4s) so AIRSTEP stayed
    // GAP-up with midi=0 and the looper never saw CC 102/103/104.
    return lanBigHttpBusy();
}

uint32_t lanHttpQuietMs() {
    uint32_t t = lastHttpMs;
    if (t == 0) {
        return 0xFFFFFFFFu;
    }
    return millis() - t;
}

uint32_t lanFirstHttpMs() {
    return firstHttpMs;
}

void lanKickArp() {
    arpKickQueued = true;
}

void lanRecoverUnicast() {
    if (bleLive) {
        lanPreferWifiWithBle();
    } else {
        lanPreferWifiAirtime();
    }
    lanSendGratuitousArp();
    lastArpMs = millis();
    arpKickQueued = false;
}

void lanRejoinSta() {
    // Never WiFi.disconnect on the BLE task. Queue the loop.
    rejoinQueued = true;
}

void lanTickUnicastHealth() {
    if (!bleLive) {
        lastStuckMs = 0;
        abortTried = false;
        return;
    }
    uint32_t now = millis();
    if (lastStuckMs != 0 && (int32_t)(now - lastStuckMs) < 2000) {
        return;
    }
    int aborted = lanAbortStuckPcbs();
    if (aborted <= 0) {
        return;
    }
    lastStuckMs = now;
    debugLog("wifi abort zombie pcbs=%d", aborted);
}

void lanTickRejoin() {
    if (!rejoinQueued) {
        return;
    }
    rejoinQueued = false;
    const bool hasIp = WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0;
    const bool stuck = lanUnicastTxStuck();
    // JTAG: WiFi.disconnect while associated left wifiWasDown=1 and
    // ICMP dead. Only tear STA when abort-first recovery already failed.
    if (hasIp && !stuck) {
        lanKickArp();
        debugLog("wifi skip rejoin, sta healthy ip=%s", WiFi.localIP().toString().c_str());
        return;
    }
    if (hasIp && stuck && !abortTried) {
        lastStuckMs = 0;
        debugLog("wifi skip disconnect, abort stuck pcbs first");
        return;
    }
    if (lastRejoinMs != 0 && (int32_t)(millis() - lastRejoinMs) < 12000) {
        return;
    }
    lastRejoinMs = millis();
    debugLog("wifi rejoin to restore unicast stuck=%d", stuck ? 1 : 0);
    lanPreferWifiWithBle();
    WiFi.disconnect(false, false);
    delay(80);
    WiFi.reconnect();
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - t0) < 8000) {
        delay(100);
    }
    lanRecoverUnicast();
    lanRequestHttpRestart();
    debugLog("wifi rejoin done status=%d ip=%s", (int)WiFi.status(), WiFi.localIP().toString().c_str());
}

void lanRequestHttpRestart() {
    httpRestartQueued = true;
}

bool lanTakeHttpRestart() {
    if (!httpRestartQueued) {
        return false;
    }
    httpRestartQueued = false;
    return true;
}

void lanRestoreBleSleepIfIdle() {
    // Do not put the STA back to sleep while a BLE central is live.
}

// Minimal custom mDNS responder. The esp-idf mdns component never answered
// multicast queries on the STA interface of this AP+STA setup (unicast
// probes to its port worked, group queries never did), and its announcements
// carried a stray duplicate record in the authority section. A plain UDP
// socket answers A queries for showbox.local directly to the querier —
// unicast replies survive any AP — and announces the record once a second
// so passive listeners and caches stay warm.
namespace {
constexpr uint16_t kMdnsPort = 5353;
constexpr const char* kMdnsName = "showbox.local";
constexpr uint32_t kMdnsGroupAddr = 0xE00000FBu;  // 224.0.0.251

void mdnsLowerName(const uint8_t* msg, size_t len, size_t& off, char* out, size_t cap) {
    size_t n = 0;
    int hops = 0;
    for (;;) {
        if (off >= len || ++hops > 16) {
            if (n < cap) out[n] = '\0';
            return;
        }
        uint8_t l = msg[off++];
        if (l == 0) {
            break;
        }
        if (l & 0xC0) {  // compression pointer: not expected in queries
            if (n + 1 < cap) out[n++] = '*';
            break;
        }
        if (n + l + 2 >= cap) {
            break;
        }
        if (n > 0) {
            out[n++] = '.';
        }
        for (uint8_t i = 0; i < l && off + i < len; i++) {
            char c = static_cast<char>(msg[off + i]);
            out[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        off += l;
    }
    if (n < cap) {
        out[n] = '\0';
    }
}

// The module's STA address as a little-endian uint32: written into the A
// record low-byte-first it comes out in network order, and the same value
// is what lwIP's IP_MULTICAST_IF expects.
uint32_t mdnsStaIp() {
    IPAddress ip = lanIP();
    if (ip[0] == 0) {
        return 0;
    }
    return static_cast<uint32_t>(ip[0]) | (static_cast<uint32_t>(ip[1]) << 8)
        | (static_cast<uint32_t>(ip[2]) << 16) | (static_cast<uint32_t>(ip[3]) << 24);
}

// Multicast / full-mDNS announce+answer: qd=0, cache-flush bit set.
size_t mdnsBuildARecord(uint8_t* out, size_t cap, uint32_t ipAddr, uint16_t tid) {
    if (cap < 41) {
        return 0;
    }
    size_t p = 0;
    out[p++] = static_cast<uint8_t>(tid >> 8);
    out[p++] = static_cast<uint8_t>(tid & 0xFF);
    out[p++] = 0x84; out[p++] = 0x00;  // response, authoritative
    out[p++] = 0; out[p++] = 0;        // qdcount 0
    out[p++] = 0; out[p++] = 1;        // ancount 1
    out[p++] = 0; out[p++] = 0;        // nscount 0
    out[p++] = 0; out[p++] = 0;        // arcount 0
    out[p++] = 7; memcpy(out + p, "showbox", 7); p += 7;
    out[p++] = 5; memcpy(out + p, "local", 5); p += 5;
    out[p++] = 0;
    out[p++] = 0; out[p++] = 1;        // type A
    out[p++] = 0x80; out[p++] = 0x01;  // IN + cache-flush
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 120;  // ttl
    out[p++] = 0; out[p++] = 4;        // rdlength
    out[p++] = static_cast<uint8_t>(ipAddr);
    out[p++] = static_cast<uint8_t>(ipAddr >> 8);
    out[p++] = static_cast<uint8_t>(ipAddr >> 16);
    out[p++] = static_cast<uint8_t>(ipAddr >> 24);
    return p;
}

// RFC 6762 §6.7 legacy unicast (Android/DnsResolver one-shot mDNS): echo the
// question, clear cache-flush, and cap TTL. Without this, getaddrinfo for
// showbox.local on Android 12+ times out even when the module heard the query.
size_t mdnsBuildLegacyA(uint8_t* out, size_t cap, uint32_t ipAddr, uint16_t tid, uint16_t qtype) {
    // header(12) + qname(15) + qtype/qclass(4) + aname(15) + rr(10) + rdata(4|0)
    const bool answerA = (qtype == 1 || qtype == 255);
    const size_t need = answerA ? 60 : 31;
    if (cap < need) {
        return 0;
    }
    size_t p = 0;
    out[p++] = static_cast<uint8_t>(tid >> 8);
    out[p++] = static_cast<uint8_t>(tid & 0xFF);
    out[p++] = 0x84; out[p++] = 0x00;  // response, authoritative
    out[p++] = 0; out[p++] = 1;        // qdcount 1
    out[p++] = 0; out[p++] = answerA ? 1 : 0;
    out[p++] = 0; out[p++] = 0;        // nscount 0
    out[p++] = 0; out[p++] = 0;        // arcount 0
    out[p++] = 7; memcpy(out + p, "showbox", 7); p += 7;
    out[p++] = 5; memcpy(out + p, "local", 5); p += 5;
    out[p++] = 0;
    out[p++] = static_cast<uint8_t>(qtype >> 8);
    out[p++] = static_cast<uint8_t>(qtype & 0xFF);
    out[p++] = 0; out[p++] = 1;        // IN (no QU bit)
    if (!answerA) {
        return p;  // NODATA for AAAA etc. so dual-stack resolvers do not hang
    }
    out[p++] = 7; memcpy(out + p, "showbox", 7); p += 7;
    out[p++] = 5; memcpy(out + p, "local", 5); p += 5;
    out[p++] = 0;
    out[p++] = 0; out[p++] = 1;        // type A
    out[p++] = 0; out[p++] = 1;        // IN, no cache-flush
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 10;  // ttl <= 10s
    out[p++] = 0; out[p++] = 4;
    out[p++] = static_cast<uint8_t>(ipAddr);
    out[p++] = static_cast<uint8_t>(ipAddr >> 8);
    out[p++] = static_cast<uint8_t>(ipAddr >> 16);
    out[p++] = static_cast<uint8_t>(ipAddr >> 24);
    return p;
}
}  // namespace

static bool sMdnsResponderUp = false;

static void lanMdnsResponderTask(void*) {
    int sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        debugLog("mdnsrsp socket failed");
        vTaskDelete(nullptr);
        return;
    }
    int yes = 1;
    lwip_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(kMdnsPort);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
    if (lwip_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        debugLog("mdnsrsp bind failed");
        lwip_close(sock);
        vTaskDelete(nullptr);
        return;
    }
    ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = lwip_htonl(kMdnsGroupAddr);
    mreq.imr_interface.s_addr = PP_HTONL(INADDR_ANY);
    lwip_setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    int ttl = 255;
    lwip_setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    // Poll: a blocking recvfrom would park the 1 s announce cadence forever.
    timeval tv = {};
    tv.tv_usec = 100 * 1000;
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    debugLog("mdnsrsp listening on 5353");

    // The membership above lands on whatever netif is default at bind time —
    // in AP+STA that is the AP, and the group dies with it. The STA-side
    // membership is what feeds the WiFi driver's multicast RX filter, so
    // re-assert it (and the multicast TX interface) whenever the STA IP is
    // up, and again whenever it changes.
    uint32_t joinedFor = 0;
    uint32_t lastJoinMs = 0;

    uint32_t lastAnnounceMs = 0;
    int announceFails = 0;
    for (;;) {
        uint32_t now = millis();
        uint32_t ip = mdnsStaIp();
        if (ip != 0 && (ip != joinedFor || (int32_t)(now - lastJoinMs) >= 10000)) {
            // Join on the STA netif explicitly and keep refreshing it: the
            // WiFi driver's multicast RX filter follows this membership.
            ip_mreq sta = {};
            sta.imr_multiaddr.s_addr = lwip_htonl(kMdnsGroupAddr);
            sta.imr_interface.s_addr = ip;
            if (lwip_setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &sta, sizeof(sta)) == 0) {
                joinedFor = ip;
            }
            lastJoinMs = now;
        }
        if (ip != 0 && ((lastAnnounceMs == 0 && (int32_t)(now - lastAnnounceMs) >= 0) || (int32_t)(now - lastAnnounceMs) >= 1000)) {
            in_addr ifAddr;
            ifAddr.s_addr = ip;
            lwip_setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &ifAddr, sizeof(ifAddr));
            uint8_t pkt[64];
            size_t n = mdnsBuildARecord(pkt, sizeof(pkt), ip, 0);
            if (n > 0) {
                sockaddr_in dst = {};
                dst.sin_family = AF_INET;
                dst.sin_port = lwip_htons(kMdnsPort);
                dst.sin_addr.s_addr = lwip_htonl(kMdnsGroupAddr);
                int sent = lwip_sendto(sock, pkt, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
                if (sent < 0 && announceFails < 5) {
                    announceFails++;
                    debugLog("mdnsrsp announce sendto failed errno=%d", errno);
                }
                lastAnnounceMs = now;
            }
        }
        char buf[1500];
        sockaddr_in from = {};
        socklen_t flen = sizeof(from);
        int n = lwip_recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 12) {
            continue;
        }
        uint16_t flags = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        if (flags & 0x8000) {
            continue;  // a response, not a query
        }
        uint16_t qdcount = (static_cast<uint8_t>(buf[4]) << 8) | static_cast<uint8_t>(buf[5]);
        uint16_t tid = (static_cast<uint8_t>(buf[0]) << 8) | static_cast<uint8_t>(buf[1]);
        size_t off = 12;
        bool want = false;
        uint16_t wantType = 1;
        for (int q = 0; q < qdcount && q < 4; q++) {
            char name[64] = {};
            mdnsLowerName(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n), off, name, sizeof(name));
            if (off + 4 > n) {
                break;
            }
            uint16_t qtype = (static_cast<uint8_t>(buf[off]) << 8) | static_cast<uint8_t>(buf[off + 1]);
            off += 4;  // type + class (class may carry QU bit)
            if (strcmp(name, kMdnsName) == 0 && (qtype == 1 || qtype == 28 || qtype == 255)) {
                want = true;
                wantType = qtype;
            }
        }
        if (!want || !lanHasIp()) {
            continue;
        }
        // Source port != 5353 → one-shot / legacy unicast (Android DnsResolver,
        // dig @224.0.0.251, etc.). Full mDNS peers stay on the compact form.
        const bool legacy = lwip_ntohs(from.sin_port) != kMdnsPort;
        uint8_t pkt[64];
        size_t rn = legacy
            ? mdnsBuildLegacyA(pkt, sizeof(pkt), mdnsStaIp(), tid, wantType)
            : mdnsBuildARecord(pkt, sizeof(pkt), mdnsStaIp(), tid);
        if (rn > 0) {
            lwip_sendto(sock, pkt, rn, 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
        }
    }
}

void lanKeepMdns() {
    if (gSoakRunning) {
        return;
    }
    static uint32_t lastKeep = 0;
    if (!mdnsRefreshQueued && lastKeep != 0 && (int32_t)(millis() - lastKeep) < 2000) {
        return;
    }
    lastKeep = millis();
    if (!lanHasIp()) {
        return;
    }
    WiFi.setHostname("showbox");
    if (!gMdnsUp) {
        IPAddress ip = lanIP();
        debugLog("mdns responder start ip=%s heap=%u largest=%u",
            ip.toString().c_str(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        httpsOnMdnsStart();
        gMdnsUp = true;
        if (!sMdnsResponderUp) {
            sMdnsResponderUp = true;
            if (xTaskCreate(lanMdnsResponderTask, "mdnsrsp", 6144, nullptr, 1, nullptr) != pdPASS) {
                debugLog("mdnsrsp task create failed");
                sMdnsResponderUp = false;
                gMdnsUp = false;
                mdnsRefreshQueued = true;
                return;
            }
        }
        if (Debug != nullptr) {
            Debug->printf("[  OK  ] mDNS advertised as showbox.local at %s\n", ip.toString().c_str());
        }
    }
    staJoinMdnsGroup();
    mdnsRefreshQueued = false;
    lastMdnsRefresh = millis();
}

// Gateway reachability: a persistent ICMP ping session feeds both the
// zombie-STA detector and the Tasks tab. JTAG-verified failure mode: the
// WiFi stack stays "connected" with a cached IP while the air link is
// one-way dead for minutes; status and ap_info never change, so only an
// active probe notices. Three consecutive timeouts force a re-associate,
// three failed rejoins force the reboot that provably restores the link.
static esp_ping_handle_t sGwPing = nullptr;
static volatile uint32_t sGwTargetIp = 0;
static volatile uint32_t sGwRttMs = 0;
static volatile int sGwMisses = 0;
static volatile int sGwZombieRejoins = 0;

// BLE-host yield for association: AIRSTEP's 200 ms connection events starve
// the WiFi association handshake (fresh boots associate because BLE only
// connects ~20 s later; a later drop never re-associates while the pedal is
// connected). When association keeps failing, drop the pedal link and gate
// outgoing BLE until the WiFi is back, then lift the gate and let
// autoConnect re-establish the pedal.
//
// NEVER combine the yield with WiFi.disconnect()/reconnect(): under coex
// that crashes the driver (LoadProhibited in _do_wifi_start ->
// wifi_softap_start -> ieee80211_hostap_attach, ppTask, Core 0 —
// reproduced twice 2026-10-14). Yielding alone is the cure: the one-way
// zombie mode is coex starvation, and with the pedal gone the gateway
// answers again without any re-association.
static MultiMidi* sMultiMidi = nullptr;
static int assocFails = 0;
static uint32_t lastAssocTryMs = 0;
static bool bleYieldForWifi = false;
static uint32_t bleYieldHealthyMs = 0;

void lanSetMultiMidi(MultiMidi* mm) {
    sMultiMidi = mm;
}

static void lanYieldBleForAssoc(const char* why, int failCount) {
    if (bleYieldForWifi || sMultiMidi == nullptr) {
        return;
    }
    if (!sMultiMidi->hostConnected()) {
        return;
    }
    bleYieldForWifi = true;
    bleYieldHealthyMs = 0;
    sMultiMidi->dropHostLink();
    sMultiMidi->allowOutgoing(false);
    diagEventForce("wifi", "%s: yielded BLE host for association (fail=%d)", why, failCount);
    debugLog("wifi %s: yielded BLE host for association (fail=%d)", why, failCount);
}

static void lanBleYieldTick() {
    if (!bleYieldForWifi) {
        return;
    }
    const bool healthy = WiFi.status() == WL_CONNECTED && sGwPing != nullptr && sGwMisses == 0;
    if (!healthy) {
        bleYieldHealthyMs = 0;
        return;
    }
    if (bleYieldHealthyMs == 0) {
        bleYieldHealthyMs = millis();
        return;
    }
    if ((int32_t)(millis() - bleYieldHealthyMs) >= 30000) {
        bleYieldForWifi = false;
        bleYieldHealthyMs = 0;
        if (sMultiMidi != nullptr) {
            sMultiMidi->allowOutgoing(true);
        }
        diagEventForce("wifi", "gateway healthy 30 s: BLE host yield lifted");
        debugLog("wifi healthy 30s: BLE host yield lifted, pedal will reconnect");
    }
}

// AP+BLE+UART does not fit the internal heap: at the gig the fallback
// hotspot came up next to a live pedal, softAP ate the last contiguous
// blocks and the UART interceptor starved until the next reboot (and the
// persisted apWanted kept it broken across reboots). While the AP is up,
// yield the pedal link first; it reconnects on its own when the AP stops.
static bool apYieldForAp = false;
static uint32_t apYieldSinceMs = 0;

void lanNoteApActive(bool on) {
    static bool last = false;
    if (on == last) {
        return;
    }
    last = on;
    if (on) {
        apYieldSinceMs = millis();
        debugLog("ap active: yield BLE host");
        if (sMultiMidi != nullptr && sMultiMidi->hostConnected()) {
            apYieldForAp = true;
            sMultiMidi->dropHostLink();
            sMultiMidi->allowOutgoing(false);
            diagEventForce("wifi", "fallback AP up: BLE host yielded");
        }
    } else {
        apYieldForAp = false;
        if (sMultiMidi != nullptr) {
            sMultiMidi->allowOutgoing(true);
        }
        debugLog("ap stopped: BLE host released");
    }
}

static void lanBleApTick() {
    if (!apYieldForAp) {
        return;
    }
    if (sMultiMidi == nullptr || !sMultiMidi->hostConnected()) {
        apYieldForAp = false;
        return;
    }
    // autoConnect keeps trying to re-establish while the AP is up; keep
    // stamping on it until the AP hands the radio back.
    if ((int32_t)(millis() - apYieldSinceMs) >= 2000) {
        apYieldSinceMs = millis();
        sMultiMidi->dropHostLink();
    }
}

static void gwPingStart(uint32_t ip) {
    if (sGwPing != nullptr) {
        esp_ping_stop(sGwPing);
        esp_ping_delete_session(sGwPing);
        sGwPing = nullptr;
    }
    sGwTargetIp = ip;
    sGwMisses = 0;
    if (ip == 0) {
        return;
    }
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = [](esp_ping_handle_t hdl, void* /*arg*/) {
        uint32_t rtt = 0;
        esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
        sGwRttMs = rtt;
        sGwMisses = 0;
        sGwZombieRejoins = 0;
    };
    cbs.on_ping_timeout = [](esp_ping_handle_t /*hdl*/, void* /*arg*/) {
        sGwMisses++;
    };
    cbs.on_ping_end = [](esp_ping_handle_t /*hdl*/, void* /*arg*/) {
        sGwPing = nullptr;
    };
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 0; // infinite
    cfg.interval_ms = 2000;
    // Timeout must clear the BLE-coex latency spikes (measured up to 1.1 s
    // with AIRSTEP connected): a 900 ms timeout read coex latency as a dead
    // gateway and the rejoin/reboot cascade fired on healthy links.
    cfg.timeout_ms = 2500;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip;
    cfg.interface = 0;
    if (esp_ping_new_session(&cfg, &cbs, &sGwPing) == ESP_OK) {
        esp_ping_start(sGwPing);
    } else {
        sGwPing = nullptr;
    }
}

void lanGwPingTick() {
    uint32_t gw = WiFi.gatewayIP();
    if (WiFi.status() != WL_CONNECTED || gw == 0) {
        if (sGwPing != nullptr) {
            gwPingStart(0);
        }
        return;
    }
    if (gw != sGwTargetIp) {
        gwPingStart(gw);
        return;
    }
    if (sGwPing == nullptr) {
        // Session creation failed (xTaskCreate with no contiguous task
        // memory). Retrying every tick flooded the console and reset the
        // miss counter forever, so nothing ever recovered. Rate-limit the
        // retries and, while creation keeps failing, force the zombie
        // ladder so the heap-guarded rejoin / UART-safe restart can act.
        static uint32_t lastPingStartMs = 0;
        static uint32_t lastForcedZombieMs = 0;
        if (lastPingStartMs == 0 || (int32_t)(millis() - lastPingStartMs) >= 15000) {
            lastPingStartMs = millis();
            gwPingStart(gw);
        }
        if (sGwPing == nullptr && (int32_t)(millis() - lastForcedZombieMs) >= 60000) {
            lastForcedZombieMs = millis();
            // The creation failure is a LOW HEAP problem (the ping task needs
            // ~4 KB contiguous). A rejoin/restart doesn't free heap — the next
            // boot has the same pressure. Keep retrying the creation instead
            // of forcing the zombie ladder into a reboot loop (boot #924→#1017
            // was a ~700-reboot loop from exactly this).
            diagEvent("wifi", "ping task create failing, retrying (low heap)");
            debugLog("wifi ping task create failing, retrying (low heap)");
        }
        lanBleYieldTick();
        return;
    }
    lanBleYieldTick();
    // ICMP pings contend for the same coex airtime as a large HTTP body;
    // pause the ping while one is in flight (zombie detection is
    // meaningless mid-transfer anyway) and resume 3 s after it ends.
    // Webui-active BLE policy: one radio, and the webui is the priority
    // while someone is actually using it (any HTTP request in the last 30 s,
    // or an open page that just loaded). While active, the AIRSTEP BLE link
    // yields; 30 s after the webui goes quiet it reconnects on its own.
    static bool webuiYield = false;
    static uint32_t webuiQuietSinceMs = 0;
    const uint32_t webuiHttpQuiet = lanHttpQuietMs();
    if (webuiHttpQuiet < 30000) {
        webuiQuietSinceMs = 0;
        if (!webuiYield) {
            webuiYield = true;
            if (sMultiMidi != nullptr && sMultiMidi->hostConnected()) {
                sMultiMidi->dropHostLink();
                sMultiMidi->allowOutgoing(false);
                diagEventForce("wifi", "webui active: BLE host yielded");
                debugLog("wifi webui active: BLE host yielded");
            }
        }
    } else if (webuiYield) {
        if (webuiQuietSinceMs == 0) {
            webuiQuietSinceMs = millis();
        }
        if ((int32_t)(millis() - webuiQuietSinceMs) > 30000) {
            webuiYield = false;
            webuiQuietSinceMs = 0;
            if (sMultiMidi != nullptr) {
                sMultiMidi->allowOutgoing(true);
            }
            diagEventForce("wifi", "webui quiet 30 s: BLE host released");
            debugLog("wifi webui quiet: BLE host released, pedal reconnecting");
        }
    }
    static bool pingPaused = false;
    const bool httpBusy = lanBigHttpBusy();
    if (httpBusy != pingPaused && sGwPing != nullptr) {
        pingPaused = httpBusy;
        if (httpBusy) {
            esp_ping_stop(sGwPing);
        } else {
            esp_ping_start(sGwPing);
        }
    }
    // Zombie check: the stack claims associated but the gateway stopped
// answering. Re-associate. If repeated rejoins fail, the last resort is
// a UART-safe reboot (drained first — see uartSafeRestart).
//
// With a BLE host (AIRSTEP) connected, the gateway dies under coex while
// the pedal holds the radio (A/B measured: pedal off = 0/731 loss, pedal
// on = sustained TCP death). Yield the pedal at the first confirmed
// zombie — a few seconds of gateway silence is already a real death
// window, and the yield heals the link in place instead of burning it
// until a forced restart. The pedal reconnects on its own once the
// gateway is healthy again (lanBleYieldTick). Without a host, rejoin.
    const bool hostUp = sMultiMidi != nullptr && sMultiMidi->hostConnected();
    static uint32_t zombieSinceMs = 0;
    uint32_t now = millis();
    if (hostUp) {
        if (sGwMisses < 3) {
            zombieSinceMs = 0;
            return;
        }
        // ~8 s of gateway silence: coex starvation is real and only gets
        // worse. Yield now — no WiFi churn, no rejoin, no restart.
        sGwMisses = 0;
        zombieSinceMs = 0;
        lanYieldBleForAssoc("gateway zombie", 1);
        return;
    }
    if (sGwMisses >= 3) {
        static uint32_t lastZombieMs = 0;
        if (lastZombieMs != 0 && (int32_t)(now - lastZombieMs) < 12000) {
            return;
        }
        lastZombieMs = now;
        int n = ++sGwZombieRejoins;
        sGwMisses = 0;
        diagEventForce("wifi", "sta zombie: gateway dead, rejoin #%d", n);
        debugLog("wifi zombie gateway unreachable rejoin#%d", n);
        if (n >= 3) {
            uartSafeRestart("gateway unreachable, rejoins failed");
            return;
        }
        // Low heap makes WiFi.disconnect crash the driver (LoadProhibited in
        // _do_wifi_start -> wifi_softap_start: allocation failure inside the
        // mode process). Only rejoin with enough heap; below that, the
        // ping-session recreation / BLE yield / last-resort restart handle it.
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 20480) {
            diagEvent("wifi", "rejoin skipped, heap largest too small");
            debugLog("wifi rejoin skipped, low heap");
            return;
        }
        if (n >= 2) {
            // No esp_wifi_stop/start — crashes the WiFi driver (see the
            // assoc path). The BLE yield frees the airtime instead.
            lanYieldBleForAssoc("zombie rejoin", n);
        }
        WiFi.disconnect(false, false);
        delay(80);
        WiFi.reconnect();
    }
}

uint32_t lanGwRttMs() {
    return sGwPing != nullptr ? __atomic_load_n(&sGwRttMs, __ATOMIC_RELAXED) : 0;
}

int lanGwMisses() {
    return sGwPing != nullptr ? __atomic_load_n(&sGwMisses, __ATOMIC_RELAXED) : 0;
}

// TCP-listen health: the AsyncWebServer wedge is real (JTAG 2026-10-13:
// ping 5 ms but :80 dead; "after AIRSTEP, SYN-ACK stays up but GET bodies
// never start"). The probe MUST be loopback 127.0.0.1: it tests only the
// listener and never touches the AP. The earlier WiFi-IP probe was a
// hairpin through the AP, which this AP never loops back — the probe
// failed permanently and the rebind cycle it triggered WAS the webui
// outage (2026-10-14). Loopback fails only when the listener truly wedges.
static uint32_t sHttpProbeMs = 0;
static int sHttpProbeFails = 0;
static bool sHttpRebound = false;

static bool httpListenProbe() {
    WiFiClient probe;
    if (!probe.connect(IPAddress(127, 0, 0, 1), 80, 2000)) {
        return false;
    }
    probe.setTimeout(3000);
    probe.print("GET /api/v1/showbox/link HTTP/1.0\r\nHost: probe\r\n\r\n");
    bool got = false;
    uint32_t t0 = millis();
    while (millis() - t0 < 3000) {
        int n = probe.read();
        if (n >= 0) {
            got = true;
            break;
        }
        delay(20);
    }
    probe.stop();
    return got;
}

void lanTickHttpHealth() {
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP()[0] == 0) {
        sHttpProbeFails = 0;
        sHttpRebound = false;
        return;
    }
    if (sHttpProbeMs != 0 && (int32_t)(millis() - sHttpProbeMs) < 10000) {
        return;
    }
    sHttpProbeMs = millis();
    if (httpListenProbe()) {
        if (sHttpProbeFails != 0) {
            diagEventForce("http", "listener recovered after %d fails", sHttpProbeFails);
        }
        sHttpProbeFails = 0;
        sHttpRebound = false;
        return;
    }
    sHttpProbeFails++;
    if (sHttpProbeFails >= 2 && !sHttpRebound) {
        sHttpRebound = true;
        diagEventForce("http", "listener wedged after %d fails, rebinding", sHttpProbeFails);
        debugLog("http probe fail=%d, rebind listener", sHttpProbeFails);
        lanRequestHttpRestart();
        return;
    }
    // Rebinds did not cure the wedge: last resort is a UART-safe reboot —
    // the UART is drained first, so the base survives it (as with iso).
    if (sHttpProbeFails >= 6) {
        uartSafeRestart("http listener wedge, rebind failed");
    }
}

void lanKeepWifi(const FrontPanel& panel) {
    lanBleApTick();
    lanGwPingTick();
    if (!panel.wifiRadioOn() || panel.wifiPortalOpen()) {
        return;
    }
    if (arpKickQueued) {
        arpKickQueued = false;
        if (WiFi.status() == WL_CONNECTED) {
            lanSendGratuitousArp();
            lastArpMs = millis();
            debugLog("arp kick");
        }
    }
    if (millis() - lastWifiFix < 5000) {
        return;
    }
    lastWifiFix = millis();
    wifi_ap_record_t ap;
    wl_status_t st = WiFi.status();
    const bool haveAp = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const bool hasIp = st == WL_CONNECTED && WiFi.localIP()[0] != 0;
    bool associated = hasIp;
    static int misses = 0;
    if (associated) {
        misses = 0;
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        static int upLogs = 0;
        if (wifiWasDown) {
            wifiWasDown = false;
            upLogs = 0;
            assocFails = 0;
            lastAssocTryMs = 0;
            debugLog("wifi back ip=%s rssi=%d n=%u heap=%u largest=%u",
                WiFi.localIP().toString().c_str(), haveAp ? ap.rssi : 0, haveAp ? (unsigned)ap.phy_11n : 0,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (Debug != nullptr) {
                Debug->printf("[ INFO ] WiFi back at %s\n", WiFi.localIP().toString().c_str());
            }
            staProto = 0;
            lanPreferWifiAirtime();
            if (gMdnsUp) {
                gMdnsUp = false;
                mdnsIgmpJoined = false;
                lastIgmpReportMs = 0;
            }
            mdnsRefreshQueued = true;
            lanKeepMdns();
            lastMdnsRefresh = millis();
        } else if (++upLogs >= 6) {
            upLogs = 0;
            debugLog("wifi up ip=%s rssi=%d n=%u heap=%u largest=%u",
                WiFi.localIP().toString().c_str(), haveAp ? ap.rssi : 0, haveAp ? (unsigned)ap.phy_11n : 0,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        if (lastArpMs == 0 || (int32_t)(millis() - lastArpMs) >= (bleLive ? 10000 : 3000)) {
            lanSendGratuitousArp();
            lastArpMs = millis();
            if (gMdnsUp) {
                staJoinMdnsGroup();
            }
        }
        return;
    }
    // JTAG: esp_wifi_sta_get_ap_info failed during AIRSTEP while ICMP
    // still answered. Three misses then disconnected a live STA.
    if (st == WL_CONNECTED && WiFi.localIP()[0] != 0) {
        misses = 0;
        wifiWasDown = false;
        debugLog("wifi ap-info miss, keep sta ip=%s", WiFi.localIP().toString().c_str());
        return;
    }
    wifiWasDown = true;
    staProto = 0;
    if (++misses < 3) {
        debugLog("wifi down status=%d rssi=%d miss=%d", (int)st, WiFi.RSSI(), misses);
        return;
    }
    misses = 0;
    const uint32_t nowFix = millis();
    const uint32_t backoff = assocFails < 4 ? 5000 : 15000;
    if (lastAssocTryMs != 0 && (int32_t)(nowFix - lastAssocTryMs) < (int32_t)backoff) {
        return;
    }
    lastAssocTryMs = nowFix;
    assocFails++;
    if (Debug != nullptr) {
        Debug->printf("[ INFO ] WiFi link is down (status %d rssi %d). Reconnecting (attempt %d).\n",
            (int)st, WiFi.RSSI(), assocFails);
    }
    if (assocFails >= 3) {
        lanYieldBleForAssoc("assoc", assocFails);
    }
    if (st == WL_CONNECTED) {
        WiFi.disconnect(false, false);
    }
    // NOTE: no esp_wifi_stop()/start() here — calling that outside the
    // Arduino WiFi layer crashed in _do_wifi_start/wifi_softap_start
    // (LoadProhibited, Core 0, backtrace via ppTask). The BLE yield above
    // is what actually frees airtime for the association handshake.
    bool ok = WiFi.reconnect();
    debugLog("wifi reconnect=%d fails=%d status=%d rssi=%d ip=%s",
        ok ? 1 : 0, assocFails, (int)WiFi.status(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
}

void lanTickIsoReboot(MackieShowbox& showbox) {
    if (gIsoPending >= 0) {
        int level = gIsoPending;
        gIsoPending = -1;
        if (showbox.uartHot()) {
            showbox.waitQuiet(40);
        }
        Preferences prefs;
        if (prefs.begin("debug", false)) {
            prefs.putInt("iso", level);
            prefs.end();
        }
        debugLog("iso save %d reboot", level);
        delay(50);
        ESP.restart();
    }
    if (gIsoRebootAt != 0 && (int32_t)(millis() - gIsoRebootAt) >= 0) {
        gIsoRebootAt = 0;
        ESP.restart();
    }
}

void lanTickSoak(MackieShowbox& showbox) {
    if (gSoakSeconds <= 0 || gSoakRunning) {
        return;
    }
    int seconds = gSoakSeconds;
    gSoakSeconds = 0;
    gSoakRunning = true;
    uint32_t initM0 = showbox.initFromMixer();
    uint32_t initB0 = showbox.initFromBase();
    uint32_t hbM0 = showbox.hbFromMixer();
    uint32_t hbB0 = showbox.hbFromBase();
    uint32_t rxB0 = showbox.rxFromBase();
    uint32_t rxM0 = showbox.rxFromMixer();
    uint32_t maxBase = 0;
    uint32_t maxMix = 0;
    uint32_t deaths = 0;
    bool wasLive = showbox.linkOk(2000);
    debugLog("uart soak start %ds wifi off", seconds);
    delay(200);
    esp_wifi_stop();
    uint32_t t0 = millis();
    uint32_t dur = static_cast<uint32_t>(seconds) * 1000u;
    while ((int32_t)(millis() - t0) < (int32_t)dur) {
        showbox.tick();
        uint32_t ba = showbox.baseAgeMs();
        uint32_t ma = showbox.mixerAgeMs();
        if (ba != 0xFFFFFFFFu && ba > maxBase) {
            maxBase = ba;
        }
        if (ma != 0xFFFFFFFFu && ma > maxMix) {
            maxMix = ma;
        }
        bool live = showbox.linkOk(2000);
        if (wasLive && !live) {
            deaths = deaths + 1;
        }
        wasLive = live;
        delay(20);
    }
    esp_wifi_start();
    WiFi.reconnect();
    char body[512];
    snprintf(body, sizeof(body),
        "{\"done\":true,\"running\":false,\"seconds\":%d,\"deaths\":%u,\"maxBase\":%u,\"maxMix\":%u,"
        "\"initM\":[%u,%u],\"initB\":[%u,%u],\"hbM\":[%u,%u],\"hbB\":[%u,%u],"
        "\"rxB\":[%u,%u],\"rxM\":[%u,%u],\"dropB\":%u,\"dropM\":%u,\"frmB\":%u,\"frmM\":%u,\"ok\":%s}",
        seconds, (unsigned)deaths, (unsigned)maxBase, (unsigned)maxMix,
        (unsigned)initM0, (unsigned)showbox.initFromMixer(),
        (unsigned)initB0, (unsigned)showbox.initFromBase(),
        (unsigned)hbM0, (unsigned)showbox.hbFromMixer(),
        (unsigned)hbB0, (unsigned)showbox.hbFromBase(),
        (unsigned)rxB0, (unsigned)showbox.rxFromBase(),
        (unsigned)rxM0, (unsigned)showbox.rxFromMixer(),
        (unsigned)showbox.droppedFromBase(), (unsigned)showbox.droppedFromMixer(),
        (unsigned)showbox.frameErrFromBase(), (unsigned)showbox.frameErrFromMixer(),
        (deaths == 0 && maxBase < 2000 && maxMix < 2000
            && showbox.rxFromBase() > rxB0 && showbox.rxFromMixer() > rxM0
            && showbox.initFromMixer() == initM0 && showbox.initFromBase() == initB0)
            ? "true" : "false");
    uartSoakStore(body);
    debugLog("uart soak done deaths=%u maxB=%u maxM=%u", (unsigned)deaths, (unsigned)maxBase, (unsigned)maxMix);
    gSoakRunning = false;
}
