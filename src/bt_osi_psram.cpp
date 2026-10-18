#include <cstddef>
#include <cstdint>
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_wifi.h>

// Newer IDF (55.03.312 framework) removed the S3 bt_osi_mem_* allocator —
// NimBLE host buffers are allocated by the port itself, so the malloc/calloc
// PSRAM wraps are gone. What remains is the coex-critical controller config
// and the AMPDU-off wifi init.
extern "C" {

static volatile uint32_t sHits = 0;
static volatile uint32_t sMisses = 0;
static volatile uint32_t sBytes = 0;

void bt_osi_psram_stats(uint32_t* hits, uint32_t* misses, uint32_t* bytes) {
    if (hits != nullptr) {
        *hits = sHits;
    }
    if (misses != nullptr) {
        *misses = sMisses;
    }
    if (bytes != nullptr) {
        *bytes = sBytes;
    }
}

static volatile int sWifiInitWrap = 0;

void wifi_init_wrap_stats(int* called) {
    if (called != nullptr) {
        *called = sWifiInitWrap;
    }
}

esp_err_t __real_esp_bt_controller_init(esp_bt_controller_config_t* cfg);
esp_err_t __real_esp_wifi_init(const wifi_init_config_t* cfg);

// Arduino-ESP32's NimBLE blob is built with CE_LENGTH_TYPE_ORIG and
// MODEM_SLEEP off, so max_ce_len is ignored and the controller never
// releases 2.4 GHz between AIRSTEP events. Honor HCI CE length and sleep.
esp_err_t __wrap_esp_bt_controller_init(esp_bt_controller_config_t* cfg) {
    if (cfg != nullptr) {
        cfg->sleep_mode = ESP_BT_SLEEP_MODE_1;
        cfg->sleep_clock = ESP_BT_SLEEP_CLOCK_MAIN_XTAL;
        cfg->ce_len_type = ESP_BLE_CE_LEN_TYPE_CE;
        cfg->ble_ping_en = 0;
    }
    return __real_esp_bt_controller_init(cfg);
}

// Arduino-ESP32 enables AMPDU. An 11n aggregate plus AIRSTEP holds PTA so
// ARP/ICMP die while the STA still reports WL_CONNECTED. Single MPDUs fit
// the 2.5 ms BLE gap.
esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* cfg) {
    sWifiInitWrap = 1;
    if (cfg == nullptr) {
        return __real_esp_wifi_init(cfg);
    }
    wifi_init_config_t local = *cfg;
    local.ampdu_rx_enable = 0;
    local.ampdu_tx_enable = 0;
    local.amsdu_tx_enable = 0;
    local.rx_ba_win = 0;
    return __real_esp_wifi_init(&local);
}

}