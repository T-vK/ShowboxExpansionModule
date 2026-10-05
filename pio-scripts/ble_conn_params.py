Import("env")
from os.path import join

# NimBLE's default connection uses a 100% scan (10 ms / 10 ms) and then a
# 20–40 ms interval. Either one takes the radio from Wi-Fi, so the module
# stays associated and still never answers a page. Offer a 20 ms / 100 ms
# scan and a 50–100 ms link, and keep that offer when the peer asks again.
client = join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "BLE", "src", "BLEClient.cpp",
)
try:
    text = open(client, encoding="utf-8").read()
except OSError:
    text = ""

old_scan = """  m_pConnParams.scan_itvl = 16;                                             // Scan interval in 0.625ms units (NimBLE Default)
  m_pConnParams.scan_window = 16;                                           // Scan window in 0.625ms units (NimBLE Default)
  m_pConnParams.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;                   // min_int = 0x10*1.25ms = 20ms
  m_pConnParams.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;                   // max_int = 0x20*1.25ms = 40ms"""

new_scan = """  m_pConnParams.scan_itvl = 160;                                            // 100 ms. A 10 ms window equal to the interval starves Wi-Fi.
  m_pConnParams.scan_window = 16;                                           // 10 ms. Longer than the 128-byte UART FIFO (~11 ms) drops mixer↔base.
  m_pConnParams.itvl_min = 0x50;                                            // 100 ms. Shorter than this starves HTTP.
  m_pConnParams.itvl_max = 0xA0;                                            // 200 ms"""

old_ce = """  m_pConnParams.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;               // Minimum length of connection event in 0.625ms units
  m_pConnParams.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;               // Maximum length of connection event in 0.625ms units"""

new_ce = """  m_pConnParams.min_ce_len = 0;                                             // let the controller pick a short event
  m_pConnParams.max_ce_len = 16;                                            // 10 ms. Longer CE events keep Wi-Fi off the air."""

old_update = """  params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;  // Minimum length of connection event in 0.625ms units
  params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;  // Maximum length of connection event in 0.625ms units

  int rc = ble_gap_update_params(m_conn_id, &params);"""

new_update = """  params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;  // Minimum length of connection event in 0.625ms units
  params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;  // Maximum length of connection event in 0.625ms units
  // Peer update requests are answered from m_pConnParams. Keep them in step
  // with the interval we just asked for, or the next request snaps back to 20 ms.
  m_pConnParams.itvl_min = minInterval;
  m_pConnParams.itvl_max = maxInterval;
  m_pConnParams.latency = latency;
  m_pConnParams.supervision_timeout = timeout;

  int rc = ble_gap_update_params(m_conn_id, &params);"""

old_timeout = """  m_peerAddress = address;

  int rc = 0;

  /* Try to connect the the advertiser.  Allow 30 seconds (30000 ms) for
   *  timeout (default value of m_connectTimeout)."""

new_timeout = """  m_peerAddress = address;
  if (timeoutMs != 0 && timeoutMs < 60000) {
    m_connectTimeout = (int32_t)timeoutMs;
  }

  int rc = 0;

  /* Try to connect the the advertiser.  Allow 30 seconds (30000 ms) for
   *  timeout (default value of m_connectTimeout)."""

updated = text.replace(old_scan, new_scan, 1).replace(old_update, new_update, 1).replace(old_timeout, new_timeout, 1).replace(old_ce, new_ce, 1)
old_scan_mid = """  m_pConnParams.scan_itvl = 160;                                            // 100 ms. A 10 ms window equal to the interval starves Wi-Fi.
  m_pConnParams.scan_window = 32;                                           // 20 ms
  m_pConnParams.itvl_min = 0x50;                                            // 100 ms. Shorter than this starves HTTP.
  m_pConnParams.itvl_max = 0xA0;                                            // 200 ms"""
if old_scan_mid in updated:
    updated = updated.replace(old_scan_mid, new_scan, 1)
if updated != text:
    open(client, "w", encoding="utf-8").write(updated)

# Default HID service has 40 handles. Keyboard+mouse+touch is four input
# reports plus a feature report; the extra characteristics never appear and
# Android/Linux only see report ID 1 (keyboard).
hid_dev = join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "BLE", "src", "BLEHIDDevice.cpp",
)
try:
    hid_text = open(hid_dev, encoding="utf-8").read()
except OSError:
    hid_text = ""
hid_old = "  m_hidService = server->createService(BLEUUID((uint16_t)0x1812), 40);"
hid_new = "  m_hidService = server->createService(BLEUUID((uint16_t)0x1812), 96);"
if hid_old in hid_text:
    open(hid_dev, "w", encoding="utf-8").write(hid_text.replace(hid_old, hid_new, 1))

# The HTTP task stack is 16 KB. After Bluetooth that no longer fits in internal
# RAM, and the next lock allocation reboots the chip. Put the stack in PSRAM.
tcp = join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "AsyncTCP", "src", "AsyncTCP.cpp")
try:
    tcp_text = open(tcp, encoding="utf-8").read()
except OSError:
    tcp_text = ""
old_task = """static bool customTaskCreateUniversal(
  TaskFunction_t pxTaskCode,
  const char* const pcName,
  const uint32_t usStackDepth,
  void* const pvParameters,
  UBaseType_t uxPriority,
  TaskHandle_t* const pxCreatedTask,
  const BaseType_t xCoreID) {
#ifndef CONFIG_FREERTOS_UNICORE
  if (xCoreID >= 0 && xCoreID < 2) {
    return xTaskCreatePinnedToCore(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, xCoreID);
  } else {
#endif
    return xTaskCreate(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask);
#ifndef CONFIG_FREERTOS_UNICORE
  }
#endif
}"""
new_task = """static bool customTaskCreateUniversal(
  TaskFunction_t pxTaskCode,
  const char* const pcName,
  const uint32_t usStackDepth,
  void* const pvParameters,
  UBaseType_t uxPriority,
  TaskHandle_t* const pxCreatedTask,
  const BaseType_t xCoreID) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
  const BaseType_t core = (xCoreID >= 0 && xCoreID < 2) ? xCoreID : tskNO_AFFINITY;
  if (xTaskCreatePinnedToCoreWithCaps(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
    return true;
  }
#endif
#ifndef CONFIG_FREERTOS_UNICORE
  if (xCoreID >= 0 && xCoreID < 2) {
    return xTaskCreatePinnedToCore(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, xCoreID);
  } else {
#endif
    return xTaskCreate(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask);
#ifndef CONFIG_FREERTOS_UNICORE
  }
#endif
}"""
tcp_updated = tcp_text.replace(old_task, new_task, 1)
if "#include <esp_heap_caps.h>" not in tcp_updated and tcp_updated:
    tcp_updated = tcp_updated.replace('#include "AsyncTCP.h"\n', '#include "AsyncTCP.h"\n#include <esp_heap_caps.h>\n', 1)
if tcp_updated != tcp_text and tcp_updated:
    open(tcp, "w", encoding="utf-8").write(tcp_updated)

def accept_existing_mtu(path):
    try:
        source = open(path, encoding="utf-8").read()
    except OSError:
        return
    old = """        rc = ble_gattc_exchange_mtu(client->m_conn_id, nullptr, nullptr);
        if (rc != 0) {
          log_e("BLEClient", "MTU exchange error; rc=%d %s", rc, BLEUtils::returnCodeToString(rc));
          break;
        }"""
    new = """        rc = ble_gattc_exchange_mtu(client->m_conn_id, nullptr, nullptr);
        // The pedal may start this exchange itself. Already-running means
        // the completion event is still coming.
        if (rc == BLE_HS_EALREADY) {
          return 0;
        }
        if (rc != 0) {
          log_e("BLEClient", "MTU exchange error; rc=%d %s", rc, BLEUtils::returnCodeToString(rc));
          break;
        }"""
    updated_mtu = source.replace(old, new, 1)
    if updated_mtu != source:
        open(path, "w", encoding="utf-8").write(updated_mtu)

accept_existing_mtu(client)

# HTTP responses malloc the whole TCP window in internal RAM. After dual-role
# BLE that allocation fails, so / and /services come back empty or hang.
http_resp = join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "ESPAsyncWebServer", "src", "WebResponses.cpp")
try:
    http_text = open(http_resp, encoding="utf-8").read()
except OSError:
    http_text = ""
old_http = """    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }

    uint8_t* buf = (uint8_t*)malloc(outLen + headLen);"""
old_http_1460 = """    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }
    if (outLen > 1460) {
      outLen = 1460;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(outLen + headLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
      buf = (uint8_t*)malloc(outLen + headLen);
    }"""
new_http = """    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }
    if (outLen > 1024) {
      outLen = 1024;
    }

    uint8_t* buf = nullptr;
    do {
      const size_t need = outLen + headLen;
      if (need == 0) {
        break;
      }
      buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
      if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
      if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      }
      if (buf) {
        break;
      }
      if (outLen <= 1) {
        break;
      }
      outLen = outLen / 2;
    } while (outLen > 0);"""
old_http_capped = """    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }
    if (outLen > 1024) {
      outLen = 1024;
    }

    uint8_t* buf = (uint8_t*)malloc(outLen + headLen);"""
old_http_512 = """    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }
    if (outLen > 512) {
      outLen = 512;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(outLen + headLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
      buf = (uint8_t*)malloc(outLen + headLen);
    }"""
old_http_64 = """    if (outLen > 1024) {
      outLen = 1024;
    }

    uint8_t* buf = nullptr;
    while (outLen > 64) {
      const size_t need = outLen + headLen;
      buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
      if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
      if (!buf) {
        buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      }
      if (buf) {
        break;
      }
      outLen = outLen / 2;
    }"""
if old_http in http_text:
    http_text = http_text.replace(old_http, new_http, 1)
elif old_http_capped in http_text:
    http_text = http_text.replace(old_http_capped, new_http, 1)
elif old_http_512 in http_text:
    http_text = http_text.replace(old_http_512, new_http, 1)
elif old_http_1460 in http_text:
    http_text = http_text.replace(old_http_1460, new_http, 1)
elif old_http_64 in http_text:
    http_text = http_text.replace(old_http_64, new_http[new_http.find("    if (outLen > 1024)") :], 1)
if http_text and "#include <esp_heap_caps.h>" not in http_text:
    http_text = http_text.replace('#include "WebResponseImpl.h"\n', '#include "WebResponseImpl.h"\n#include <esp_heap_caps.h>\n', 1)
if http_text:
    open(http_resp, "w", encoding="utf-8").write(http_text)

# A failed mbuf alloc used to pass nullptr into ble_gatts_notify_custom and
# reboot the chip (debug assert / heap abort) the first time HID notified.
chr_path = join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "BLE", "src", "BLECharacteristic.cpp",
)
try:
    chr_text = open(chr_path, encoding="utf-8").read()
except OSError:
    chr_text = ""
old_mbuf = """    os_mbuf *om = ble_hs_mbuf_from_flat((uint8_t *)value.c_str(), length);

    if (!is_notification && (m_properties & BLECharacteristic::PROPERTY_INDICATE)) {"""
new_mbuf = """    os_mbuf *om = ble_hs_mbuf_from_flat((uint8_t *)value.c_str(), length);
    if (om == nullptr) {
      log_e("notify mbuf alloc failed, len=%u", (unsigned)length);
      m_pCallbacks->onStatus(this, BLECharacteristicCallbacks::Status::ERROR_GATT, BLE_HS_ENOMEM);
      return;
    }

    if (!is_notification && (m_properties & BLECharacteristic::PROPERTY_INDICATE)) {"""
if old_mbuf in chr_text:
    open(chr_path, "w", encoding="utf-8").write(chr_text.replace(old_mbuf, new_mbuf, 1))

# HOGP needs several 0x2A4D Report characteristics. NimBLE kept only the first
# UUID match, so mouse and absolute pointer never appeared in GATT.
svc_path = join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "BLE", "src", "BLEService.cpp",
)
try:
    svc_text = open(svc_path, encoding="utf-8").read()
except OSError:
    svc_text = ""
old_dup = """#if defined(CONFIG_NIMBLE_ENABLED)
  if (pExisting != nullptr) {
    pExisting->m_removed = 0;
  } else
#endif
  {
    // Remember this characteristic in our map of characteristics.  At this point, we can lookup by UUID
    // but not by handle.  The handle is allocated to us on the ESP_GATTS_ADD_CHAR_EVT.
    m_characteristicMap.setByUUID(pCharacteristic, pCharacteristic->getUUID());
  }"""
new_dup = """  // HOGP uses several 0x2A4D Report characteristics (keyboard, mouse, abs).
  // NimBLE used to keep only the first UUID match, so mouse/abs never reached GATT.
  m_characteristicMap.setByUUID(pCharacteristic, pCharacteristic->getUUID());
  if (pExisting != nullptr) {
#if defined(CONFIG_NIMBLE_ENABLED)
    pExisting->m_removed = 0;
#endif
  }"""
if old_dup in svc_text:
    open(svc_path, "w", encoding="utf-8").write(svc_text.replace(old_dup, new_dup, 1))
