#include "System/HttpsGate.h"
#include "System/HttpsFallbackCert.h"

#include "System/LanKeep.h"
#include "WiredNet/WiredNet.h"
#include "WebUi/WebUi.h"
#include "MackieShowbox/MackieShowbox.h"
#include "DebugLog.h"

extern MackieShowbox showbox;
extern volatile bool gMdnsUp;
extern int gIso;

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <nvs.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <mbedtls/asn1write.h>
#include <mbedtls/error.h>
#ifndef MBEDTLS_ERR_ERROR_GENERIC_ERROR
#define MBEDTLS_ERR_ERROR_GENERIC_ERROR (-0x0001)
#endif
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr int kHttpsPort = 443;
constexpr int kHttpPort = 80;
constexpr int kMaxConn = 1;
constexpr uint32_t kHandshakeMs = 15000;
constexpr int kPrepStack = 16384;
constexpr int kListenStack = 49152;
constexpr int kWorkerStack = 32768;
constexpr UBaseType_t kListenPrio = 1;
constexpr UBaseType_t kWorkerPrio = 1;
constexpr uint32_t kStartAfterBindMs = 2000;

// ChaCha20 returns MBEDTLS_ERR_SSL_INTERNAL_ERROR (-27648) on this
// prebuilt lib (JTAG: hello=185, send=0, first handshake() fails).
// AES-GCM hung for 15 s when every mbedtls calloc was PSRAM — HW AES
// DMA scratch must be INTERNAL|DMA. Large SSL records stay in PSRAM.
// GCM HW returned GENERIC_ERROR (-1) at pageOff=1792 (JTAG). CBC uses
// the AES register path, not GCM DMA against a PSRAM out_buf.
const int kTlsSuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    0,
};

volatile bool gWanted = true;
volatile bool gListening = false;
volatile uint8_t gState = 0;  // 0 idle, 1 starting, 2 up, 3 stopping
volatile bool gStop = false;
volatile bool gNeedSave = false;
volatile bool gBindReady = false;
volatile uint32_t gBindReadyMs = 0;
volatile uint32_t gListenAt = 0;
volatile bool gHsOk = false;
volatile bool gPageOk = false;
volatile uint32_t gPageOkAt = 0;
volatile int gSendDelayMs = 20;
volatile int gIoBudgetMs = 4000;
volatile int gHsHelloN = 0;
volatile int gHsSendN = 0;
volatile int gHsRecvN = 0;
volatile int gHsFirstRet = 0;
volatile int gHsLastRet = 0;
volatile int gHsWantR = 0;
volatile int gHsWantW = 0;
volatile int gHsLoops = 0;
volatile int gPageOff = 0;
volatile int gPageWret = 0;
volatile uint32_t gBeatMs = 0;
volatile int gWorkers = 0;  // NOLINT(runtime/int)
TaskHandle_t gListenTask = nullptr;
SemaphoreHandle_t gLock = nullptr;
volatile int gListenFd = -1;
int gClientFd[kMaxConn];
int gBackendFd[kMaxConn];

String gCaPem;
String gCaKeyPem;
String gSrvPem;
String gSrvKeyPem;
char gErr[96] = {};

mbedtls_x509_crt gSrvCrt;
mbedtls_pk_context gSrvKey;
mbedtls_ssl_config gSslConf;
volatile bool gSslReady = false;
bool gCrtInited = false;
bool gAdvOn = false;

void setErr(const char* text) {
    strncpy(gErr, text == nullptr ? "" : text, sizeof(gErr) - 1);
    gErr[sizeof(gErr) - 1] = '\0';
}

void setErrMbed(const char* what, int ret) {
    char tmp[80];
#ifdef MBEDTLS_ERROR_C
    char mbed[48];
    mbedtls_strerror(ret, mbed, sizeof(mbed));
    snprintf(tmp, sizeof(tmp), "%s (%d %s)", what, ret, mbed);
#else
    snprintf(tmp, sizeof(tmp), "%s (%d)", what, ret);
#endif
    setErr(tmp);
}

void beat() {
    gBeatMs = millis();
}

int rng(void*, unsigned char* out, size_t len) {
    if (out == nullptr || len == 0) {
        return 0;
    }
    esp_fill_random(out, len);
    beat();
    return 0;
}

void* httpsCalloc(size_t n, size_t size) {
    if (n == 0 || size == 0) {
        n = 1;
        size = 1;
    }
    if (size != 0 && n > SIZE_MAX / size) {
        return nullptr;
    }
    // Keep every mbedtls alloc in PSRAM. Small INTERNAL/DMA allocs during
    // handshake steal Wi-Fi TCP RAM (ping lives, HTTP/TLS bodies die).
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void httpsMbedAlloc() {
#if defined(MBEDTLS_PLATFORM_MEMORY)
    mbedtls_platform_set_calloc_free(httpsCalloc, heap_caps_free);
#endif
}

void lock() {
    if (gLock != nullptr) {
        xSemaphoreTake(gLock, portMAX_DELAY);
    }
}

void unlock() {
    if (gLock != nullptr) {
        xSemaphoreGive(gLock);
    }
}

String loadPemKey(Preferences& prefs, const char* key) {
    size_t n = prefs.getBytesLength(key);
    if (n > 80 && n < 4096) {
        char* tmp = static_cast<char*>(
            heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (tmp == nullptr) {
            tmp = static_cast<char*>(malloc(n + 1));
        }
        if (tmp != nullptr) {
            size_t got = prefs.getBytes(key, tmp, n);
            tmp[got] = '\0';
            String pem(tmp);
            heap_caps_free(tmp);
            return pem;
        }
    }
    return prefs.getString(key, "");
}

size_t putPem(Preferences& prefs, const char* key, const String& pem) {
    size_t n = pem.length();
    if (n == 0 || n > 2048) {
        return 0;
    }
    size_t written = prefs.putBytes(key, pem.c_str(), n);
    if (written == n) {
        return written;
    }
    return prefs.putString(key, pem.c_str());
}

bool savePemsRaw() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("https", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        debugLog("https nvs open %d", (int)err);
        return false;
    }
    err = nvs_set_blob(handle, "ca", gCaPem.c_str(), gCaPem.length());
    esp_err_t keyErr = nvs_set_blob(handle, "key", gCaKeyPem.c_str(), gCaKeyPem.length());
    esp_err_t commitErr = ESP_OK;
    if (err == ESP_OK && keyErr == ESP_OK) {
        commitErr = nvs_commit(handle);
    }
    nvs_close(handle);
    debugLog("https nvs blob ca=%d key=%d commit=%d", (int)err, (int)keyErr, (int)commitErr);
    return err == ESP_OK && keyErr == ESP_OK && commitErr == ESP_OK;
}

bool savePems() {
    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        debugLog("https nvs used=%u free=%u ns=%u",
            (unsigned)stats.used_entries, (unsigned)stats.free_entries,
            (unsigned)stats.namespace_count);
        if (stats.free_entries < 200) {
            debugLog("https nvs skip, using RAM certs");
            return false;
        }
    }
    Preferences prefs;
    if (!prefs.begin("https", false)) {
        setErr("Could not open HTTPS storage.");
        return savePemsRaw();
    }
    prefs.remove("cakey");
    prefs.remove("srv");
    prefs.remove("srvkey");
    size_t nCa = putPem(prefs, "ca", gCaPem);
    size_t nKey = putPem(prefs, "key", gCaKeyPem);
    prefs.end();
    debugLog("https nvs ca=%u key=%u", (unsigned)nCa, (unsigned)nKey);
    if (nCa > 0 && nKey > 0) {
        return true;
    }
    if (savePemsRaw()) {
        return true;
    }
    return false;
}

void loadPems() {
    Preferences prefs;
    if (!prefs.begin("https", true)) {
        return;
    }
    gCaPem = prefs.getString("ca", "");
    if (gCaPem.length() < 80) {
        gCaPem = loadPemKey(prefs, "ca");
    }
    gCaKeyPem = prefs.getString("key", "");
    if (gCaKeyPem.length() < 80) {
        gCaKeyPem = loadPemKey(prefs, "key");
    }
    if (gCaKeyPem.length() < 80) {
        gCaKeyPem = prefs.getString("cakey", "");
        gSrvPem = prefs.getString("srv", "");
        gSrvKeyPem = prefs.getString("srvkey", "");
    }
    prefs.end();
    if (gSrvPem.length() < 80) {
        gSrvPem = gCaPem;
        gSrvKeyPem = gCaKeyPem;
    }
}

int writeSan(mbedtls_x509write_cert* crt) {
    unsigned char buf[160];
    unsigned char* start = buf;
    unsigned char* p = buf + sizeof(buf);
    size_t len = 0;
    int ret = 0;
    const char* names[] = { "showbox.local", "showbox" };
    for (int i = 1; i >= 0; i--) {
        size_t n = strlen(names[i]);
        MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_raw_buffer(
            &p, start, reinterpret_cast<const unsigned char*>(names[i]), n));
        MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, start, n));
        MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2));
    }
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, start, len));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    return mbedtls_x509write_crt_set_extension(
        crt,
        MBEDTLS_OID_SUBJECT_ALT_NAME,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
        0, p, len);
}

bool writePem(mbedtls_x509write_cert* crt, mbedtls_pk_context* key, String& certOut, String& keyOut) {
    unsigned char* certBuf = static_cast<unsigned char*>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    unsigned char* keyBuf = static_cast<unsigned char*>(
        heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (certBuf == nullptr) {
        certBuf = static_cast<unsigned char*>(malloc(4096));
    }
    if (keyBuf == nullptr) {
        keyBuf = static_cast<unsigned char*>(malloc(1024));
    }
    if (certBuf == nullptr || keyBuf == nullptr) {
        free(certBuf);
        free(keyBuf);
        setErr("Out of memory writing certificates.");
        return false;
    }
    int ret = mbedtls_x509write_crt_pem(crt, certBuf, 4096, rng, nullptr);
    if (ret != 0) {
        setErrMbed("cert pem", ret);
        free(certBuf);
        free(keyBuf);
        return false;
    }
    ret = mbedtls_pk_write_key_pem(key, keyBuf, 1024);
    if (ret != 0) {
        setErrMbed("key pem", ret);
        free(certBuf);
        free(keyBuf);
        return false;
    }
    certOut = reinterpret_cast<char*>(certBuf);
    keyOut = reinterpret_cast<char*>(keyBuf);
    free(certBuf);
    free(keyBuf);
    return true;
}

void fillSerial(unsigned char* serial, size_t n) {
    esp_fill_random(serial, n);
    serial[0] &= 0x7F;
    if (serial[0] == 0) {
        serial[0] = 1;
    }
}

[[maybe_unused]] bool generateCerts() {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        WiFi.macAddress(mac);
    }
    char name[72];
    snprintf(name, sizeof(name),
        "CN=showbox.local,O=ShowboxMod,OU=%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    debugLog("https keygen heap=%u largest=%u psram=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    int ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0) {
        ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng, nullptr);
    }
    vTaskDelay(1);
    if (ret != 0) {
        setErrMbed("keygen", ret);
        mbedtls_pk_free(&key);
        return false;
    }

    String pem, keyPem;
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    unsigned char serial[16];
    fillSerial(serial, sizeof(serial));
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    ret = mbedtls_x509write_crt_set_subject_name(&crt, name);
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, name);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959");
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_key_usage(&crt,
            MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN
                | MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_ns_cert_type(&crt,
            MBEDTLS_X509_NS_CERT_TYPE_SSL_CA | MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER);
    }
    if (ret == 0) {
        ret = writeSan(&crt);
    }
    bool ok = ret == 0 && writePem(&crt, &key, pem, keyPem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (!ok) {
        if (ret != 0) {
            setErrMbed("crt", ret);
        }
        return false;
    }

    gCaPem = pem;
    gSrvPem = pem;
    gCaKeyPem = keyPem;
    gSrvKeyPem = keyPem;
    gNeedSave = true;
    debugLog("https minted CA %u bytes stack=%u",
        (unsigned)gCaPem.length(),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    setErr("");
    return true;
}

bool hasPems() {
    return gCaPem.length() > 80 && gCaKeyPem.length() > 80
        && gSrvPem.length() > 80 && gSrvKeyPem.length() > 80;
}

void useFallbackCerts() {
    gCaPem = kHttpsFallbackCert;
    gSrvPem = kHttpsFallbackCert;
    gCaKeyPem = kHttpsFallbackKey;
    gSrvKeyPem = kHttpsFallbackKey;
    gNeedSave = false;
    setErr("");
    debugLog("https using fallback CA %u bytes", (unsigned)gCaPem.length());
}

bool ensureCertsLocked() {
    if (hasPems()) {
        return true;
    }
    loadPems();
    if (hasPems()) {
        return true;
    }
    useFallbackCerts();
    return hasPems();
}

BaseType_t spawnHttpsTask(TaskFunction_t fn, const char* name, uint32_t stack,
        void* arg, UBaseType_t prio, TaskHandle_t* handle, bool allowShrink) {
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        fn, name, stack, arg, prio, handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) {
        return ok;
    }
    ok = xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, 1);
    if (ok == pdPASS || !allowShrink) {
        return ok;
    }
    if (stack > 12288) {
        ok = xTaskCreatePinnedToCoreWithCaps(
            fn, name, 12288, arg, prio, handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok == pdPASS) {
            return ok;
        }
        ok = xTaskCreatePinnedToCore(fn, name, 12288, arg, prio, handle, 1);
        if (ok == pdPASS) {
            return ok;
        }
    }
    if (stack > 8192) {
        ok = xTaskCreatePinnedToCoreWithCaps(
            fn, name, 8192, arg, prio, handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok == pdPASS) {
            return ok;
        }
        return xTaskCreatePinnedToCore(fn, name, 8192, arg, prio, handle, 1);
    }
    return pdFAIL;
}

template<typename Fd>
void closeFd(Fd& fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        fd = -1;
    }
}

void kickSockets() {
    closeFd(gListenFd);
    for (int i = 0; i < kMaxConn; i++) {
        closeFd(gClientFd[i]);
        closeFd(gBackendFd[i]);
    }
}

int tlsSend(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    if (fd < 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    int n = send(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return n;
}

int tlsRecv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    if (fd < 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    int n = recv(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (n == 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }
    return n;
}

int sendInternal(void* ctx, const unsigned char* out, size_t len) {
    auto* net = static_cast<mbedtls_net_context*>(ctx);
    // JTAG: PSRAM-origin TLS records must bounce through internal BSS or
    // the host stops after the 96 B ServerHello. SO_SNDBUF is unimplemented.
    // select()-writable before send broke the handshake (EOF, send=792).
    // Pace after each bounce; keep the page small so 11b can finish it.
    static uint8_t bounce[128];
    size_t off = 0;
    uint32_t t0 = millis();
    while (off < len) {
        int budget = gIoBudgetMs > 200 ? gIoBudgetMs : 200;
        if ((int32_t)(millis() - t0) > budget) {
            return off > 0 ? static_cast<int>(off) : MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        size_t chunk = len - off;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }
        memcpy(bounce, out + off, chunk);
        int n = mbedtls_net_send(net, bounce, chunk);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            return off > 0 ? static_cast<int>(off) : n;
        }
        gHsSendN += n;
        off += static_cast<size_t>(n);
        int pace = gSendDelayMs > 0 ? gSendDelayMs : 5;
        vTaskDelay(pdMS_TO_TICKS(pace));
    }
    return static_cast<int>(off);
}

void setSockOpts(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    // Keep at most ~2 KB unacked. A 6 KB unacked pile (JTAG send=6802)
    // filled lwIP and starved HTTP :80 while the host had 161 B.
    int snd = 2048;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool waitConnected(int fd) {
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
        return false;
    }
    int err = 0;
    socklen_t errLen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) != 0) {
        return false;
    }
    return err == 0;
}

int connectBackend() {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    setSockOpts(fd);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kHttpPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 || waitConnected(fd)) {
        return fd;
    }
    IPAddress ip = lanIP();
    if (ip != IPAddress()) {
        addr.sin_addr.s_addr = static_cast<uint32_t>(ip);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 || waitConnected(fd)) {
            return fd;
        }
    }
    close(fd);
    return -1;
}

void freeSsl() {
    if (gSslReady) {
        mbedtls_ssl_config_free(&gSslConf);
        gSslReady = false;
    }
    if (gCrtInited) {
        mbedtls_x509_crt_free(&gSrvCrt);
        mbedtls_pk_free(&gSrvKey);
        gCrtInited = false;
    }
}

bool setupSsl() {
    freeSsl();
    mbedtls_x509_crt_init(&gSrvCrt);
    mbedtls_pk_init(&gSrvKey);
    mbedtls_ssl_config_init(&gSslConf);
    gCrtInited = true;
    int ret = mbedtls_x509_crt_parse(
        &gSrvCrt,
        reinterpret_cast<const unsigned char*>(gSrvPem.c_str()),
        gSrvPem.length() + 1);
    if (ret == 0 && gCaPem != gSrvPem) {
        ret = mbedtls_x509_crt_parse(
            &gSrvCrt,
            reinterpret_cast<const unsigned char*>(gCaPem.c_str()),
            gCaPem.length() + 1);
    }
    if (ret == 0) {
        ret = mbedtls_pk_parse_key(
            &gSrvKey,
            reinterpret_cast<const unsigned char*>(gSrvKeyPem.c_str()),
            gSrvKeyPem.length() + 1,
            nullptr, 0, rng, nullptr);
    }
    if (ret == 0) {
        ret = mbedtls_ssl_config_defaults(
            &gSslConf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (ret != 0) {
        setErrMbed("ssl setup", ret);
        freeSsl();
        return false;
    }
    mbedtls_ssl_conf_rng(&gSslConf, rng, nullptr);
    mbedtls_ssl_conf_authmode(&gSslConf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_min_tls_version(&gSslConf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&gSslConf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&gSslConf, kTlsSuites);
#ifdef MBEDTLS_SSL_RENEGOTIATION
    mbedtls_ssl_conf_renegotiation(&gSslConf, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
    ret = mbedtls_ssl_conf_own_cert(&gSslConf, &gSrvCrt, &gSrvKey);
    if (ret != 0) {
        setErrMbed("own cert", ret);
        freeSsl();
        return false;
    }
    gSslReady = true;
    return true;
}

bool openListen() {
    beat();
    debugLog("https bind begin errno=%d", errno);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        setErr("Could not open port 443.");
        debugLog("https socket failed errno=%d", errno);
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kHttpsPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        debugLog("https bind errno=%d", errno);
        close(fd);
        setErr("Could not bind port 443.");
        return false;
    }
    if (listen(fd, kMaxConn) != 0) {
        debugLog("https listen errno=%d", errno);
        close(fd);
        setErr("Could not listen on port 443.");
        return false;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    gListenFd = fd;
    beat();
    return true;
}

void advertiseFromLoop() {
    // The custom mDNS responder only answers A records; HTTPS stays
    // discoverable by port, not by service browse.
    gAdvOn = gListening;
}

void spliceConn(int slot, int client) {
    gClientFd[slot] = client;
    setSockOpts(client);
    beat();
    gSendDelayMs = 20;
    gHsOk = false;
    // Do not clear gPageOk / gPageOkAt here. The first successful page
    // releases the BLE hold after a settle; later connections must not
    // start that timer over.
    gHsHelloN = 0;
    gHsSendN = 0;
    gHsRecvN = 0;
    gHsFirstRet = 0;
    gHsLastRet = 0;
    gHsWantR = 0;
    gHsWantW = 0;
    gHsLoops = 0;
    gPageOff = 0;
    gPageWret = 0;
    debugLog("https client heap=%u largest=%u stack=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    auto* ssl = static_cast<mbedtls_ssl_context*>(
        heap_caps_malloc(sizeof(mbedtls_ssl_context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ssl == nullptr) {
        ssl = static_cast<mbedtls_ssl_context*>(
            heap_caps_malloc(sizeof(mbedtls_ssl_context), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    auto* buf = static_cast<unsigned char*>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ssl == nullptr || buf == nullptr) {
        setErr("Out of memory for TLS.");
        heap_caps_free(ssl);
        heap_caps_free(buf);
        closeFd(gClientFd[slot]);
        return;
    }
    mbedtls_ssl_init(ssl);
    int ret = mbedtls_ssl_setup(ssl, &gSslConf);
    if (ret != 0) {
        setErrMbed("ssl ctx", ret);
        mbedtls_ssl_free(ssl);
        heap_caps_free(ssl);
        heap_caps_free(buf);
        closeFd(gClientFd[slot]);
        return;
    }
    // Host received the 96-byte ServerHello and then nothing, while
    // mbedtls_net_send queued ~787 bytes. 11b drops the larger cert
    // record; cap records near the size that actually arrives.
    mbedtls_ssl_set_mtu(ssl, 128);
    mbedtls_net_context net;
    mbedtls_net_init(&net);
    net.fd = client;
    // Let the full ClientHello land before the first recv. An immediate
    // short read used to drop the rest of the TCP segment on 11b+BLE.
    vTaskDelay(pdMS_TO_TICKS(80));
    constexpr int kHelloCap = 4096;
    unsigned char* hello = static_cast<unsigned char*>(
        heap_caps_malloc(kHelloCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (hello == nullptr) {
        hello = static_cast<unsigned char*>(
            heap_caps_malloc(kHelloCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (hello == nullptr) {
        setErr("Out of memory for ClientHello.");
        mbedtls_ssl_free(ssl);
        heap_caps_free(ssl);
        heap_caps_free(buf);
        closeFd(gClientFd[slot]);
        return;
    }
    int helloN = 0;
    uint32_t helloAt = millis();
    auto recvMore = [&]() -> bool {
        while ((int32_t)(millis() - helloAt) < 8000) {
            int room = kHelloCap - helloN;
            if (room <= 0) {
                return helloN > 0;
            }
            int n = recv(client, hello + helloN, static_cast<size_t>(room), 0);
            if (n > 0) {
                helloN += n;
                return true;
            }
            if (n == 0) {
                return helloN > 0;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
                return false;
            }
            vTaskDelay(1);
        }
        return helloN > 0;
    };
    recvMore();
    int helloNeed = 5;
    if (helloN >= 5 && hello[0] == 0x16) {
        helloNeed = 5 + ((static_cast<int>(hello[3]) << 8) | hello[4]);
        if (helloNeed > kHelloCap) {
            helloNeed = kHelloCap;
        }
    }
    while (helloN < helloNeed && (int32_t)(millis() - helloAt) < 8000) {
        if (!recvMore()) {
            break;
        }
        if (helloN >= 5 && hello[0] == 0x16) {
            helloNeed = 5 + ((static_cast<int>(hello[3]) << 8) | hello[4]);
            if (helloNeed > kHelloCap) {
                helloNeed = kHelloCap;
            }
        }
    }
    gHsHelloN = helloN;
    if (helloN < helloNeed) {
        char shortHello[80];
        snprintf(shortHello, sizeof(shortHello), "short ClientHello n=%d need=%d b0=%02x",
            helloN, helloNeed, helloN > 0 ? hello[0] : 0);
        setErr(shortHello);
        heap_caps_free(hello);
        mbedtls_ssl_free(ssl);
        heap_caps_free(ssl);
        heap_caps_free(buf);
        closeFd(gClientFd[slot]);
        return;
    }
    struct HelloBio {
        mbedtls_net_context* net;
        const unsigned char* hello;
        int helloN;
        int helloOff;
    } bio = { &net, hello, helloN, 0 };
    auto recvHello = [](void* ctx, unsigned char* out, size_t len) -> int {
        auto* b = static_cast<HelloBio*>(ctx);
        if (b->helloOff < b->helloN) {
            int n = b->helloN - b->helloOff;
            if (n > static_cast<int>(len)) {
                n = static_cast<int>(len);
            }
            memcpy(out, b->hello + b->helloOff, static_cast<size_t>(n));
            b->helloOff += n;
            return n;
        }
        int n = mbedtls_net_recv(b->net, out, len);
        if (n > 0) {
            gHsRecvN += n;
        }
        return n;
    };
    auto sendHello = [](void* ctx, const unsigned char* out, size_t len) -> int {
        auto* b = static_cast<HelloBio*>(ctx);
        return sendInternal(b->net, out, len);
    };
    mbedtls_ssl_set_bio(ssl, &bio, sendHello, recvHello, nullptr);
    uint32_t t0 = millis();
    uint32_t lastArp = t0;
    while ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
        beat();
        if (gHsLoops == 0) {
            gHsFirstRet = ret;
        }
        gHsLastRet = ret;
        gHsLoops = gHsLoops + 1;
        if (gStop) {
            break;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            gHsWantR = gHsWantR + 1;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            gHsWantW = gHsWantW + 1;
        } else {
            setErrMbed("handshake", ret);
            break;
        }
        if ((int32_t)(millis() - t0) > (int32_t)kHandshakeMs) {
            char to[96];
            snprintf(to, sizeof(to),
                "TLS handshake timed out hello=%d send=%d recv=%d wr=%d ww=%d first=%d last=%d",
                helloN, gHsSendN, gHsRecvN, gHsWantR, gHsWantW, gHsFirstRet, gHsLastRet);
            setErr(to);
            ret = -1;
            break;
        }
        if ((int32_t)(millis() - lastArp) >= 1000) {
            lanKickArp();
            lastArp = millis();
        }
        vTaskDelay(1);
    }
    debugLog("https hs %d send=%d recv=%d first=%d last=%d wr=%d ww=%d loops=%d stack=%u",
        ret, gHsSendN, gHsRecvN, gHsFirstRet, gHsLastRet, gHsWantR, gHsWantW, gHsLoops,
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (ret == 0) {
        gHsOk = true;
        // HS stays at 20 ms/record. After that, writable-wait is the
        // throttle; a short yield keeps the Wi-Fi task running.
        gSendDelayMs = 15;
        // HS needed 128 B records. Application data after that can use
        // slightly larger records so a 1.2 KB page is a handful of frames
        // instead of ~20 (JTAG/host: got=472 then HTTP :80 died).
        mbedtls_ssl_set_mtu(ssl, 256);
    }
    if (ret != 0 || gStop) {
        heap_caps_free(hello);
        mbedtls_ssl_free(ssl);
        heap_caps_free(ssl);
        heap_caps_free(buf);
        closeFd(gClientFd[slot]);
        return;
    }
    mbedtls_ssl_set_bio(ssl, &net, sendInternal, mbedtls_net_recv, nullptr);
    heap_caps_free(hello);
    hello = nullptr;
    int nReq = 0;
    uint32_t r0 = millis();
    while ((nReq = mbedtls_ssl_read(ssl, buf, 4096)) < 0) {
        if (nReq != MBEDTLS_ERR_SSL_WANT_READ && nReq != MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        }
        if ((int32_t)(millis() - r0) > 4000) {
            break;
        }
        vTaskDelay(1);
    }
    auto pathIs = [&](const char* path) -> bool {
        const size_t n = strlen(path);
        if (nReq < static_cast<int>(4 + n + 1) || memcmp(buf, "GET ", 4) != 0) {
            return false;
        }
        if (memcmp(buf + 4, path, n) != 0) {
            return false;
        }
        const unsigned char c = buf[4 + n];
        return c == ' ' || c == '\r' || c == '?';
    };
    auto writePlain = [&](const char* type, const char* body, size_t bodyLen) -> bool {
        int hdrLen = snprintf(reinterpret_cast<char*>(buf), 4096,
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            type, (unsigned)bodyLen);
        if (hdrLen <= 0 || static_cast<size_t>(hdrLen) + bodyLen > 4096) {
            return false;
        }
        memcpy(buf + hdrLen, body, bodyLen);
        const unsigned char* p = buf;
        size_t left = static_cast<size_t>(hdrLen) + bodyLen;
        uint32_t tWrite = millis();
        while (left > 0 && !gStop) {
            if ((int32_t)(millis() - tWrite) > 8000) {
                return false;
            }
            size_t chunk = left > 128 ? 128 : left;
            int w = mbedtls_ssl_write(ssl, p, chunk);
            gPageWret = w;
            gPageOff = static_cast<int>(p - buf);
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (w <= 0) {
                return false;
            }
            p += w;
            left -= static_cast<size_t>(w);
            gPageOff = static_cast<int>(p - buf);
        }
        return left == 0;
    };
    auto finishTls = [&]() {
        // Clients stop at Content-Length. close_notify/FIN is often lost on
        // 11b (JTAG: full page got=1069 then recv timeout). Do not sit in
        // sendInternal for 4 s — that held kMaxConn=1 so the next GET /
        // only received headers (got=125).
        gIoBudgetMs = 200;
        mbedtls_ssl_close_notify(ssl);
        gIoBudgetMs = 4000;
        mbedtls_ssl_free(ssl);
        heap_caps_free(ssl);
        heap_caps_free(buf);
        closeFd(gClientFd[slot]);
        lanRecoverUnicast();
    };
    if (pathIs("/api/v1/https") || pathIs("/api/v1/showbox/link")) {
        String js = pathIs("/api/v1/https") ? httpsStatusJson() : httpsLinkJson();
        writePlain("application/json", js.c_str(), js.length());
        finishTls();
        return;
    }
    const bool wantRoot = nReq > 4 && (memcmp(buf, "GET / ", 6) == 0
        || memcmp(buf, "GET / HTTP", 10) == 0
        || memcmp(buf, "GET /index", 10) == 0
        || memcmp(buf, "HEAD / ", 7) == 0);
    if (wantRoot) {
        // JTAG: 64 KB gzip over TLS on 11b dies around 40 KB (host
        // got=48813, then WANT_WRITE / lost FIN). Small JSON on this
        // same writePlain path finishes. SO_SNDBUF is unimplemented and
        // AIRSTEP drops the air below 64 kbit/s, so 20×64 KB cannot work.
        // HTTP :80 still serves the full mixer UI.
        static const char kPage[] =
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Showbox</title>"
            "<style>body{margin:0;font-family:system-ui,sans-serif;background:#101418;color:#e8eef2}"
            "main{max-width:36rem;margin:12vh auto;padding:0 1.5rem}h1{margin:0 0 .6rem}"
            "p{line-height:1.5;color:#c5d0d6}a{color:#7ec8ff}code{color:#f3d67b}</style>"
            "</head><body><main><h1>Showbox</h1>"
            "<p>https://showbox.local/ is up. Mixer UART and Bluetooth stay live.</p>"
            "<p>Open the mixer UI over HTTP (the full app is too large for TLS on this radio):</p>"
            "<p><a href=\"http://showbox.local/\">http://showbox.local/</a></p>"
            "<p>Device status: <a href=\"/api/v1/https\">/api/v1/https</a> · "
            "<a href=\"/api/v1/https/ca.pem\">ca.pem</a> · "
            "<a href=\"/api/v1/showbox/link\">mixer link</a></p>"
            "<p>This landing page is kept small on purpose so HTTPS keeps answering "
            "while AIRSTEP holds the PTA and the Showbox mixer is connected.</p>"
            "<p>Repeat requests to this URL must keep working. The Showbox mixer "
            "link and AIRSTEP connection are left running on purpose.</p>"
            "</main></body></html>";
        const size_t pageLen = sizeof(kPage) - 1;
        gPageOff = 0;
        gPageWret = 0;
        bool ok = writePlain("text/html; charset=utf-8", kPage, pageLen);
        gPageOff = ok ? static_cast<int>(pageLen) : gPageOff;
        debugLog("https page %s %u send=%d", ok ? "ok" : "fail",
            (unsigned)pageLen, gHsSendN);
        if (ok) {
            gPageOk = true;
            if (gPageOkAt == 0) {
                gPageOkAt = millis();
            }
        } else {
            setErr("page write failed");
        }
        finishTls();
        return;
    }
    int backend = connectBackend();
    if (backend < 0) {
        setErr("HTTPS could not reach HTTP on port 80.");
        finishTls();
        return;
    }
    gBackendFd[slot] = backend;
    if (nReq > 0) {
        send(backend, buf, static_cast<size_t>(nReq), 0);
    }
    while (!gStop && gClientFd[slot] >= 0 && gBackendFd[slot] >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = gClientFd[slot] > gBackendFd[slot] ? gClientFd[slot] : gBackendFd[slot];
        FD_SET(gClientFd[slot], &rfds);
        FD_SET(gBackendFd[slot], &rfds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int sel = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            break;
        }
        if (sel == 0) {
            beat();
            continue;
        }
        beat();
        if (FD_ISSET(gClientFd[slot], &rfds) || mbedtls_ssl_get_bytes_avail(ssl) > 0) {
            int n = mbedtls_ssl_read(ssl, buf, 4096);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                // wait
            } else if (n <= 0) {
                break;
            } else if (send(gBackendFd[slot], buf, static_cast<size_t>(n), 0) != n) {
                break;
            }
        }
        if (FD_ISSET(gBackendFd[slot], &rfds)) {
            int n = recv(gBackendFd[slot], buf, 4096, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }
            const unsigned char* p = buf;
            size_t left = static_cast<size_t>(n);
            while (left > 0 && !gStop) {
                int w = mbedtls_ssl_write(ssl, p, left);
                if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                if (w <= 0) {
                    left = 1;
                    break;
                }
                p += w;
                left -= static_cast<size_t>(w);
            }
            if (left > 0) {
                break;
            }
        }
    }
    mbedtls_ssl_close_notify(ssl);
    mbedtls_ssl_free(ssl);
    heap_caps_free(ssl);
    heap_caps_free(buf);
    closeFd(gBackendFd[slot]);
    closeFd(gClientFd[slot]);
    lanRecoverUnicast();
}

void httpsWorker(void* arg) {
    int client = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    gWorkers = gWorkers + 1;
    if (!gStop && client >= 0) {
        spliceConn(0, client);
    } else if (client >= 0) {
        close(client);
    }
    gWorkers = gWorkers - 1;
    vTaskDelete(nullptr);
}

void httpsListenTask(void*) {
    debugLog("https listen heap=%u largest=%u stack=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    while (!gStop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (gListenFd < 0) {
            break;
        }
        FD_SET(gListenFd, &rfds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int sel = select(gListenFd + 1, &rfds, nullptr, nullptr, &tv);
        if (gStop) {
            break;
        }
        beat();
        if (sel <= 0 || !FD_ISSET(gListenFd, &rfds)) {
            continue;
        }
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        int client = accept(gListenFd, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (client < 0) {
            continue;
        }
        if (gWorkers >= kMaxConn) {
            close(client);
            continue;
        }
        // Inline on this 32 KB task. A PSRAM worker often never ran, so
        // 443 stayed open, ClientHello sat unread, and HTTP on :80 wedged.
        gWorkers = gWorkers + 1;
        spliceConn(0, client);
        gWorkers = gWorkers - 1;
    }
    kickSockets();
    uint32_t t0 = millis();
    while (gWorkers > 0 && (int32_t)(millis() - t0) < 1500) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    freeSsl();
    gListening = false;
    gListenTask = nullptr;
    gState = 0;
    debugLog("https stopped heap=%u largest=%u psram=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    vTaskDelete(nullptr);
}

void httpsPrepTask(void*) {
    httpsMbedAlloc();
    debugLog("https prep begin heap=%u largest=%u stack=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    lock();
    bool ok = ensureCertsLocked();
    unlock();
    debugLog("https certs %d stack=%u", ok ? 1 : 0,
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (ok && !gStop) {
        ok = setupSsl();
        debugLog("https ssl %d stack=%u", ok ? 1 : 0,
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
    if (!ok || gStop) {
        debugLog("https start failed: %s", gErr);
        kickSockets();
        freeSsl();
        gListening = false;
        gListenTask = nullptr;
        gState = 0;
        vTaskDelete(nullptr);
        return;
    }
    bool bound = openListen();
    debugLog("https bind %d fd=%d", bound ? 1 : 0, (int)gListenFd);
    if (!bound || gStop) {
        debugLog("https bind failed: %s", gErr);
        kickSockets();
        freeSsl();
        gListening = false;
        gListenTask = nullptr;
        gState = 0;
        vTaskDelete(nullptr);
        return;
    }
    gListening = true;
    gListenAt = millis();
    gState = 2;
    setErr("");
    preloadWebUi();
    lanRecoverUnicast();
    debugLog("https listen 443 heap=%u largest=%u psram=%u stack=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    TaskHandle_t listen = nullptr;
    BaseType_t spawned = spawnHttpsTask(
        httpsListenTask, "httpsL", kListenStack, nullptr, kListenPrio, &listen, false);
    if (spawned == pdPASS && listen != nullptr) {
        gListenTask = listen;
        vTaskDelete(nullptr);
        return;
    }
    httpsListenTask(nullptr);
}

}  // namespace

void httpsLoad() {
    if (gLock == nullptr) {
        gLock = xSemaphoreCreateMutex();
    }
    for (int i = 0; i < kMaxConn; i++) {
        gClientFd[i] = -1;
        gBackendFd[i] = -1;
    }
    Preferences radio;
    if (radio.begin("radio", true)) {
        gWanted = radio.getBool("https", false);
        radio.end();
    }
    lock();
    loadPems();
    unlock();
    debugLog("https wanted=%d hasCa=%d", gWanted ? 1 : 0, hasPems() ? 1 : 0);
}

void httpsTick() {
    const bool allowed = gWanted && lanHasIp() && gBindReady
        && (int32_t)(millis() - gBindReadyMs) >= (int32_t)kStartAfterBindMs;
    const bool shouldStart = allowed;
    static uint32_t lastStartMs = 0;
    if (shouldStart && gState == 0 && gListenTask == nullptr) {
        if (lastStartMs != 0 && (int32_t)(millis() - lastStartMs) < 5000) {
            return;
        }
        lastStartMs = millis();
        gStop = false;
        gState = 1;
        beat();
        BaseType_t ok = spawnHttpsTask(
            httpsPrepTask, "https", kPrepStack, nullptr, kListenPrio, &gListenTask, false);
        if (ok != pdPASS) {
            ok = spawnHttpsTask(
                httpsPrepTask, "https", 8192, nullptr, kListenPrio, &gListenTask, false);
        }
        if (ok != pdPASS) {
            gListenTask = nullptr;
            gState = 0;
            setErr("Could not start the HTTPS task.");
            debugLog("https task create failed heap=%u largest=%u psram=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        return;
    }
    static uint32_t ipLostMs = 0;
    if (lanHasIp()) {
        ipLostMs = 0;
    } else if (ipLostMs == 0) {
        ipLostMs = millis();
    }
    const bool lostIp = !lanHasIp() && ipLostMs != 0
        && (int32_t)(millis() - ipLostMs) >= 20000;
    // Do not tear down 443 on a brief STA rejoin. AIRSTEP is already live
    // then, and a fresh HTTPS start fights the PTA.
    if ((!gWanted || !gBindReady || lostIp) && (gState == 1 || gState == 2 || gListening)) {
        gStop = true;
        gState = 3;
        closeFd(gListenFd);
        for (int i = 0; i < kMaxConn; i++) {
            closeFd(gClientFd[i]);
            closeFd(gBackendFd[i]);
        }
    }
    advertiseFromLoop();
}

void httpsWaitReady(uint32_t timeoutMs) {
    (void)timeoutMs;
}

void httpsAllowBind() {
    gBindReady = true;
    gBindReadyMs = millis();
}

void httpsOnMdnsStart() {
    gAdvOn = false;
    advertiseFromLoop();
}

bool httpsWanted() {
    return gWanted;
}

bool httpsListening() {
    return gListening;
}

bool httpsHoldOutgoing() {
    if (!gWanted) {
        return false;
    }
    if (!gListening) {
        return (int32_t)millis() < 60000;
    }
    // Keep AIRSTEP off until the first full page AND a short settle.
    // JTAG: gPageOk=1 immediately started the pedal connect and STA
    // unicast died, so the post-page /link snapshot looked like mixer down.
    if (!gPageOk) {
        return true;
    }
    return gPageOkAt != 0 && (int32_t)(millis() - gPageOkAt) < 3000;
}

bool httpsSetEnabled(bool on, String& error) {
    (void)error;
    if (on == gWanted) {
        return true;
    }
    Preferences radio;
    if (radio.begin("radio", false)) {
        radio.putBool("https", on);
        radio.end();
    }
    gWanted = on;
    lanRefreshPhy();
    if (!on) {
        gStop = true;
        closeFd(gListenFd);
        for (int i = 0; i < kMaxConn; i++) {
            closeFd(gClientFd[i]);
            closeFd(gBackendFd[i]);
        }
        debugLog("https disable requested");
    } else {
        debugLog("https enable requested");
    }
    return true;
}

bool httpsEnsureCa(String& error) {
    lock();
    bool ok = ensureCertsLocked();
    unlock();
    if (!ok) {
        error = gErr[0] ? gErr : "Could not mint a device certificate.";
    }
    return ok;
}

const char* httpsCaPem() {
    return gCaPem.c_str();
}

String httpsStatusJson() {
    char err[96];
    size_t n = 0;
    for (size_t i = 0; gErr[i] != '\0' && n + 1 < sizeof(err); i++) {
        if (gErr[i] == '"' || gErr[i] == '\\') {
            if (n + 2 >= sizeof(err)) {
                break;
            }
            err[n++] = '\\';
        }
        if (static_cast<unsigned char>(gErr[i]) >= 32) {
            err[n++] = gErr[i];
        }
    }
    err[n] = '\0';
    char buf[576];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"enabled\":%s,\"listening\":%s,\"hasCa\":%s,\"hsOk\":%s,\"pageOk\":%s,\"ble\":%s,"
        "\"url\":\"%s\",\"caUrl\":\"/api/v1/https/ca.pem\",\"state\":%u,\"largest\":%u,"
        "\"hello\":%d,\"send\":%d,\"recv\":%d,\"first\":%d,\"last\":%d,\"wr\":%d,\"ww\":%d,"
        "\"loops\":%d,\"pageOff\":%d,\"pageWret\":%d,\"error\":\"%s\"}",
        gWanted ? "true" : "false",
        gListening ? "true" : "false",
        (gCaPem.length() > 80) ? "true" : "false",
        gHsOk ? "true" : "false",
        gPageOk ? "true" : "false",
        lanBleLive() ? "true" : "false",
        gListening ? "https://showbox.local/" : "",
        (unsigned)gState,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        gHsHelloN, gHsSendN, gHsRecvN, gHsFirstRet, gHsLastRet, gHsWantR, gHsWantW,
        gHsLoops, gPageOff, gPageWret, err);
    return String(buf);
}

String httpsLinkJson() {
    char body[640];
    int n = snprintf(body, sizeof(body),
        "{\"ok\":%s,\"baseAgeMs\":%u,\"mixerAgeMs\":%u,\"heartbeatAgeMs\":%u,"
        "\"mdns\":%s,\"iso\":%d,\"lock\":%u,\"pause\":%u,"
        "\"initM\":%u,\"initB\":%u,\"hbM\":%u,\"hbB\":%u,"
        "\"frmBase\":%u,\"frmMix\":%u,\"goodM\":%u,\"goodB\":%u,"
        "\"badM\":%u,\"badB\":%u,\"rxBase\":%u,\"rxMix\":%u,"
        "\"dropBase\":%u,\"dropMix\":%u,\"ovfBase\":%u,\"ovfMix\":%u,"
        "\"entM\":%u,\"fifoBase\":%u,\"fifoMix\":%u}",
        showbox.linkOk(1500) ? "true" : "false",
        (unsigned)showbox.baseAgeMs(),
        (unsigned)showbox.mixerAgeMs(),
        (unsigned)showbox.heartbeatAgeMs(),
        gMdnsUp ? "true" : "false",
        gIso,
        showbox.linkOk(1500) ? 1u : 0u,
        showbox.txToBasePaused() ? 1u : 0u,
        (unsigned)showbox.initFromMixer(),
        (unsigned)showbox.initFromBase(),
        (unsigned)showbox.hbFromMixer(),
        (unsigned)showbox.hbFromBase(),
        (unsigned)showbox.frameErrFromBase(),
        (unsigned)showbox.frameErrFromMixer(),
        (unsigned)showbox.goodFromMixer(),
        (unsigned)showbox.goodFromBase(),
        (unsigned)showbox.badFromMixer(),
        (unsigned)showbox.badFromBase(),
        (unsigned)showbox.rxFromBase(),
        (unsigned)showbox.rxFromMixer(),
        (unsigned)showbox.droppedFromBase(),
        (unsigned)showbox.droppedFromMixer(),
        (unsigned)showbox.overflowFromBase(),
        (unsigned)showbox.overflowFromMixer(),
        (unsigned)showbox.entityFromMixer(),
        (unsigned)showbox.hwRxFifoBase(),
        (unsigned)showbox.hwRxFifoMixer());
    if (n < 0 || n >= (int)sizeof(body)) {
        return String("{\"ok\":false}");
    }
    return String(body);
}

void httpsAddRoutes(AsyncWebServer* server) {
    if (server == nullptr) {
        return;
    }
    auto sendCa = [](AsyncWebServerRequest* request) {
        const char* pem = httpsCaPem();
        if (pem == nullptr || pem[0] == '\0') {
            request->send(404, "text/plain", "The device CA is not ready yet. Leave HTTPS on and try again.");
            return;
        }
        AsyncWebServerResponse* response = request->beginResponse(200, "application/x-pem-file", pem);
        response->addHeader("Content-Disposition", "attachment; filename=\"showbox-ca.pem\"");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    };
    // More specific paths first: "/api/v1/https" also matches "/api/v1/https/...".
    server->on("/api/v1/https/ca.pem", HTTP_GET, sendCa);
    server->on("/ca.pem", HTTP_GET, sendCa);
    server->on("/api/v1/https", HTTP_GET, [](AsyncWebServerRequest* request) {
        String js = httpsStatusJson();
        sendJsonChunked(request, js.c_str(), js.length());
    });
}
