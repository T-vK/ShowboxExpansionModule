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

new_scan = """  m_pConnParams.scan_itvl = 48;                                             // 30 ms. 100 ms missed AIRSTEP ads (HCI 13).
  m_pConnParams.scan_window = 16;                                           // 10 ms. Longer than the 128-byte UART FIFO (~11 ms) drops mixer↔base.
  m_pConnParams.itvl_min = 0x140;                                          // 400 ms. 200 ms BLE events + coex starve sustained TCP (ping any size flows, TCP dies); 400 ms halves the airtime tax.
  m_pConnParams.itvl_max = 0x140;                                          // 400 ms"""

old_ce = """  m_pConnParams.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;               // Minimum length of connection event in 0.625ms units
  m_pConnParams.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;               // Maximum length of connection event in 0.625ms units"""

new_ce = """  m_pConnParams.min_ce_len = 0;                                             // let the controller pick a short event
  m_pConnParams.max_ce_len = 8;                                             // 2.5 ms. 10 ms CEs plus AIRSTEP still starved ARP."""

old_update = """  params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;  // Minimum length of connection event in 0.625ms units
  params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;  // Maximum length of connection event in 0.625ms units

  int rc = ble_gap_update_params(m_conn_id, &params);"""

new_update = """  params.min_ce_len = 0;
  params.max_ce_len = 8;  // 5 ms. Must stay >= slave_ce_len_min (5) or the cap is ignored.
  // Peer update requests are answered from m_pConnParams. Keep them in step
  // with the interval we just asked for, or the next request snaps back to 20 ms.
  m_pConnParams.itvl_min = minInterval;
  m_pConnParams.itvl_max = maxInterval;
  m_pConnParams.latency = latency;
  m_pConnParams.supervision_timeout = timeout;
  m_pConnParams.min_ce_len = 0;
  m_pConnParams.max_ce_len = 8;

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
old_update_uncapped = """  params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;  // Minimum length of connection event in 0.625ms units
  params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;  // Maximum length of connection event in 0.625ms units
  // Peer update requests are answered from m_pConnParams. Keep them in step
  // with the interval we just asked for, or the next request snaps back to 20 ms.
  m_pConnParams.itvl_min = minInterval;
  m_pConnParams.itvl_max = maxInterval;
  m_pConnParams.latency = latency;
  m_pConnParams.supervision_timeout = timeout;

  int rc = ble_gap_update_params(m_conn_id, &params);"""
if old_update_uncapped in updated:
    updated = updated.replace(old_update_uncapped, new_update, 1)
old_scan_mid = """  m_pConnParams.scan_itvl = 160;                                            // 100 ms. A 10 ms window equal to the interval starves Wi-Fi.
  m_pConnParams.scan_window = 32;                                           // 20 ms
  m_pConnParams.itvl_min = 0x50;                                            // 100 ms. Shorter than this starves HTTP.
  m_pConnParams.itvl_max = 0x140;                                          // 400 ms"""
if old_scan_mid in updated:
    updated = updated.replace(old_scan_mid, new_scan, 1)
old_scan_160 = """  m_pConnParams.scan_itvl = 160;                                            // 100 ms. A 10 ms window equal to the interval starves Wi-Fi.
  m_pConnParams.scan_window = 16;                                           // 10 ms. Longer than the 128-byte UART FIFO (~11 ms) drops mixer↔base.
  m_pConnParams.itvl_min = 0x50;                                            // 100 ms. Shorter than this starves HTTP.
  m_pConnParams.itvl_max = 0x140;                                          // 400 ms"""
if old_scan_160 in updated:
    updated = updated.replace(old_scan_160, new_scan, 1)
updated = updated.replace(
    "  m_pConnParams.max_ce_len = 16;                                            // 10 ms. Longer CE events keep Wi-Fi off the air.",
    "  m_pConnParams.max_ce_len = 8;                                             // 2.5 ms. 10 ms CEs plus AIRSTEP still starved ARP.",
)
updated = updated.replace(
    "  params.max_ce_len = 16;  // 10 ms. updateConnParams used to restore unlimited CE events.",
    "  params.max_ce_len = 8;  // 5 ms. Must stay >= slave_ce_len_min (5) or the cap is ignored.",
)
updated = updated.replace("  m_pConnParams.max_ce_len = 16;\n", "  m_pConnParams.max_ce_len = 8;\n")
updated = updated.replace(
    "  m_pConnParams.itvl_min = 0x50;\n  m_pConnParams.itvl_max = 0x140",
    "  m_pConnParams.itvl_min = 0x140;\n  m_pConnParams.itvl_max = 0x140",
)
old_update_req = """      if (!rc && event->type == BLE_GAP_EVENT_CONN_UPDATE_REQ) {
        event->conn_update_req.self_params->itvl_min = client->m_pConnParams.itvl_min;
        event->conn_update_req.self_params->itvl_max = client->m_pConnParams.itvl_max;
        event->conn_update_req.self_params->latency = client->m_pConnParams.latency;
        event->conn_update_req.self_params->supervision_timeout = client->m_pConnParams.supervision_timeout;
      }"""
new_update_req = """      if (!rc && event->type == BLE_GAP_EVENT_CONN_UPDATE_REQ) {
        // AIRSTEP floors at 200 ms. Answering 100–200 ms is an LL_CONNECTION_UPDATE
        // storm that holds the 2.4 GHz PTA on BLE until ARP/ICMP die.
        const ble_gap_upd_params *peer = event->conn_update_req.peer_params;
        ble_gap_upd_params *self = event->conn_update_req.self_params;
        self->min_ce_len = 0;
        self->max_ce_len = 8;
        self->supervision_timeout = peer != nullptr && peer->supervision_timeout != 0
          ? peer->supervision_timeout
          : client->m_pConnParams.supervision_timeout;
        if (peer != nullptr && peer->itvl_min >= 0x140) {
          self->itvl_min = peer->itvl_min;
          self->itvl_max = peer->itvl_max >= peer->itvl_min ? peer->itvl_max : peer->itvl_min;
          self->latency = peer->latency;
        } else {
          self->itvl_min = 0x140;
          self->itvl_max = 0x140;
          self->latency = client->m_pConnParams.latency;
        }
      }"""
if old_update_req in updated:
    updated = updated.replace(old_update_req, new_update_req, 1)
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
# AsyncTCP 3.5.0 reformatted customTaskCreateUniversal to fewer lines; support
# both layouts and FAIL LOUDLY when neither is found — a silent no-op here
# cost us a hard-to-diagnose reboot class (2026-10-18 lib upgrade).
tcp = join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "AsyncTCP", "src", "AsyncTCP.cpp")
try:
    tcp_text = open(tcp, encoding="utf-8").read()
except OSError:
    tcp_text = ""
new_task = """static bool customTaskCreateUniversal(
  TaskFunction_t pxTaskCode, const char *const pcName, const uint32_t usStackDepth, void *const pvParameters, UBaseType_t uxPriority,
  TaskHandle_t *const pxCreatedTask, const BaseType_t xCoreID
) {
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
tcp_updated = tcp_text
if "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS" not in tcp_updated:
    old_compact = """static bool customTaskCreateUniversal(
  TaskFunction_t pxTaskCode, const char *const pcName, const uint32_t usStackDepth, void *const pvParameters, UBaseType_t uxPriority,
  TaskHandle_t *const pxCreatedTask, const BaseType_t xCoreID
) {
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
    old_multi = """static bool customTaskCreateUniversal(
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
    if old_compact in tcp_updated:
        tcp_updated = tcp_updated.replace(old_compact, new_task, 1)
    elif old_multi in tcp_updated:
        tcp_updated = tcp_updated.replace(old_multi, new_task, 1)
    else:
        raise SystemExit("ble_conn_params.py: AsyncTCP customTaskCreateUniversal layout changed — re-port the PSRAM task patch (src: %s)" % tcp)
    if "#include <esp_heap_caps.h>" not in tcp_updated and tcp_updated:
        tcp_updated = tcp_updated.replace('#include "AsyncTCP.h"\n', '#include "AsyncTCP.h"\n#include <esp_heap_caps.h>\n', 1)
    open(tcp, "w", encoding="utf-8").write(tcp_updated)
    print("async_tcp: PSRAM task-stack patch applied to %s" % tcp)

def skip_mtu_on_connect(path):
    try:
        source = open(path, encoding="utf-8").read()
    except OSError:
        return
    olds = [
        """        rc = ble_gattc_exchange_mtu(client->m_conn_id, nullptr, nullptr);
        // The pedal starts this exchange itself. Already-running means the
        // completion event is still coming; releasing early signals the
        // waiter twice and reboots the module.
        if (rc == BLE_HS_EALREADY) {
          return 0;
        }
        if (rc != 0) {
          log_e("BLEClient", "MTU exchange error; rc=%d %s", rc, BLEUtils::returnCodeToString(rc));
          break;
        }

        if (BLESecurity::m_securityEnabled && BLESecurity::m_forceSecurity) {
          BLESecurity::startSecurity(client->m_conn_id);
        }""",
        """        rc = ble_gattc_exchange_mtu(client->m_conn_id, nullptr, nullptr);
        // The pedal may start this exchange itself. Already-running means
        // the completion event is still coming.
        if (rc == BLE_HS_EALREADY) {
          return 0;
        }
        if (rc != 0) {
          log_e("BLEClient", "MTU exchange error; rc=%d %s", rc, BLEUtils::returnCodeToString(rc));
          break;
        }

        if (BLESecurity::m_securityEnabled && BLESecurity::m_forceSecurity) {
          BLESecurity::startSecurity(client->m_conn_id);
        }""",
        """        rc = ble_gattc_exchange_mtu(client->m_conn_id, nullptr, nullptr);
        if (rc != 0) {
          log_e("BLEClient", "MTU exchange error; rc=%d %s", rc, BLEUtils::returnCodeToString(rc));
          break;
        }

        if (BLESecurity::m_securityEnabled && BLESecurity::m_forceSecurity) {
          BLESecurity::startSecurity(client->m_conn_id);
        }""",
    ]
    new = """        // Skip ATT MTU exchange. Default 23-byte PDUs keep AIRSTEP CEs
        // short; MTU 256 / DLE 251 held PTA until ARP died.
        if (BLESecurity::m_securityEnabled && BLESecurity::m_forceSecurity) {
          BLESecurity::startSecurity(client->m_conn_id);
        }
        rc = 0;
        break;"""
    updated = source
    for old in olds:
        if old in updated:
            updated = updated.replace(old, new, 1)
            break
    if updated != source:
        open(path, "w", encoding="utf-8").write(updated)

skip_mtu_on_connect(client)

# NimBLE memset's advertising params to 0. A 0 interval is HCI 20–40 ms, and
# start() is a no-op if GAP is already active, so setMinInterval never lands.
# Put 200–400 ms on the first start so showbox.local can finish TCP.
adv = join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "BLE", "src", "BLEAdvertising.cpp",
)
try:
    adv_text = open(adv, encoding="utf-8").read()
except OSError:
    adv_text = ""
old_nimble_reset = """  m_advParams.conn_mode = BLE_GAP_CONN_MODE_UND;
  m_advParams.disc_mode = BLE_GAP_DISC_MODE_GEN;
  m_customAdvData = false;"""
new_nimble_reset = """  m_advParams.conn_mode = BLE_GAP_CONN_MODE_UND;
  m_advParams.disc_mode = BLE_GAP_DISC_MODE_GEN;
  m_advParams.itvl_min = 0x1000;  // 2560 ms. 1280 ms ads plus AIRSTEP still killed ARP.
  m_advParams.itvl_max = 0x1400;  // 3200 ms
  m_customAdvData = false;"""
old_blue_int = """  m_advParams.adv_int_min = 0x20;
  m_advParams.adv_int_max = 0x40;"""
new_blue_int = """  m_advParams.adv_int_min = 0x400;
  m_advParams.adv_int_max = 0x800;"""
if old_nimble_reset in adv_text:
    adv_text = adv_text.replace(old_nimble_reset, new_nimble_reset, 1)
if old_blue_int in adv_text:
    adv_text = adv_text.replace(old_blue_int, new_blue_int, 1)
adv_text = adv_text.replace(
    "  m_advParams.itvl_min = 0x400;  // 640 ms. Library 0 means HCI 20 ms and starves HTTP.\n  m_advParams.itvl_max = 0x800;  // 1280 ms",
    "  m_advParams.itvl_min = 0x1000;  // 2560 ms. 1280 ms ads plus AIRSTEP still killed ARP.\n  m_advParams.itvl_max = 0x1400;  // 3200 ms",
)
adv_text = adv_text.replace(
    "  m_advParams.itvl_min = 0x800;  // 1280 ms. Library 0 means HCI 20 ms and starves HTTP.\n  m_advParams.itvl_max = 0xC00;  // 1920 ms",
    "  m_advParams.itvl_min = 0x1000;  // 2560 ms. 1280 ms ads plus AIRSTEP still killed ARP.\n  m_advParams.itvl_max = 0x1400;  // 3200 ms",
)
# Do not stop a live advert to apply intervals. stop/start every second plus
# AIRSTEP held the PTA on BLE until ARP died.
adv_text = adv_text.replace(
    """  if (ble_gap_adv_active()) {
    ble_gap_adv_stop();
  }""",
    """  if (ble_gap_adv_active()) {
    return true;
  }""",
)
# Central connect can host-reset. The library then restarts forever-ads by
# itself, which is the dual-role PTA lock that kills ARP/ICMP.
adv_text = adv_text.replace(
    """void BLEAdvertising::onHostSync() {
  log_v("Host re-synced");

  m_advDataSet = false;
  // If we were advertising forever, restart it now
  if (m_duration == 0) {
    start(m_duration, m_advCompCB);
  } else {
    // Otherwise we should tell the app that advertising stopped.
    advCompleteCB();
  }
}  // onHostSync""",
    """void BLEAdvertising::onHostSync() {
  log_v("Host re-synced");

  m_advDataSet = false;
  advCompleteCB();
}  // onHostSync""",
)
if adv_text:
    open(adv, "w", encoding="utf-8").write(adv_text)

# Arduino-ESP32 NimBLE is built with CE_LENGTH_TYPE_ORIG and modem sleep off.
# nimble_port_init() applies those baked defaults. Init the controller ourselves
# with HCI CE length + modem sleep, then start only the NimBLE host.
dev = join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "BLE", "src", "BLEDevice.cpp",
)
try:
    dev_text = open(dev, encoding="utf-8").read()
except OSError:
    dev_text = ""
old_nimble_init = """#if defined(CONFIG_NIMBLE_ENABLED)
  errRc = nimble_port_init();
  if (errRc != ESP_OK) {
    log_e("nimble_port_init: rc=%d %s", errRc, GeneralUtils::errorToString(errRc));
    return false;
  }"""
new_nimble_init = """#if defined(CONFIG_NIMBLE_ENABLED)
  {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.sleep_mode = ESP_BT_SLEEP_MODE_1;
    bt_cfg.sleep_clock = ESP_BT_SLEEP_CLOCK_MAIN_XTAL;
    bt_cfg.ce_len_type = ESP_BLE_CE_LEN_TYPE_CE;
    bt_cfg.ble_ping_en = 0;
    errRc = esp_bt_controller_init(&bt_cfg);
    if (errRc != ESP_OK && errRc != ESP_ERR_INVALID_STATE) {
      log_e("esp_bt_controller_init: rc=%d %s", errRc, GeneralUtils::errorToString(errRc));
      return false;
    }
    errRc = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (errRc != ESP_OK && errRc != ESP_ERR_INVALID_STATE) {
      log_e("esp_bt_controller_enable: rc=%d %s", errRc, GeneralUtils::errorToString(errRc));
      return false;
    }
  }
  errRc = esp_nimble_init();
  if (errRc != ESP_OK) {
    log_e("esp_nimble_init: rc=%d %s", errRc, GeneralUtils::errorToString(errRc));
    return false;
  }"""
if old_nimble_init in dev_text:
    dev_text = dev_text.replace(old_nimble_init, new_nimble_init, 1)
    open(dev, "w", encoding="utf-8").write(dev_text)

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
    if (outLen > 8192) {
      outLen = 8192;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(outLen + headLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
      buf = (uint8_t*)malloc(outLen + headLen);
    }"""
new_http = """    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }
    if (outLen > 8192) {
      outLen = 8192;
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
    if (outLen > 8192) {
      outLen = 8192;
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
old_http_64 = """    if (outLen > 8192) {
      outLen = 8192;
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
    http_text = http_text.replace(old_http_64, new_http[new_http.find("    if (outLen > 1460)") :], 1)
http_text = http_text.replace(
    """    if (outLen > 1024) {
      outLen = 1024;
    }""",
    """    if (outLen > 8192) {
      outLen = 8192;
    }""",
)
http_text = http_text.replace(
    """    if (outLen > 1460) {
      outLen = 1460;
    }""",
    """    if (outLen > 8192) {
      outLen = 8192;
    }""",
)
http_text = http_text.replace(
    """    if (outLen > 8192) {
      outLen = 8192;
    }""",
    """    if (outLen > 1460) {
      outLen = 1460;
    }""",
)
http_text = http_text.replace(
    """    if (outLen > 1460) {
      outLen = 1460;
    }""",
    """    if (outLen > 1460) {
      outLen = 1460;
    }""",
)
http_text = http_text.replace(
    """    if (outLen > 1024) {
      outLen = 1024;
    }""",
    """    if (outLen > 1460) {
      outLen = 1460;
    }""",
)
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

# Loud post-verification: a framework upgrade that restructures the BLE
# library would silently revert the coex-critical patches (400 ms interval,
# MTU 23, capped connection events, modem-sleep init) and reintroduce the
# gateway-death regime. Refuse to build without them.
def _ble_marker_check():
    import os as _os
    base = join(env.PioPlatform().get_package_dir("framework-arduinoespressif32"), "libraries", "BLE", "src")
    checks = [
        (join(base, "BLEClient.cpp"), ["0x140", "Default 23-byte PDUs"]),
        (join(base, "BLEDevice.cpp"), ["ble_ping_en = 0", "ESP_BLE_CE_LEN_TYPE_CE"]),
        (join(base, "BLEAdvertising.cpp"), ["itvl_min = 0x1000"]),
    ]
    for path, markers in checks:
        if not _os.path.isfile(path):
            continue
        text = open(path, encoding="utf-8").read()
        for marker in markers:
            if marker not in text:
                raise SystemExit("ble_conn_params.py: BLE marker %r missing in %s — the framework changed and the coex-critical patch no longer applies. Re-port ble_conn_params.py before flashing." % (marker, path))
_ble_marker_check()
