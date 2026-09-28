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
  m_pConnParams.scan_window = 32;                                           // 20 ms
  m_pConnParams.itvl_min = 0x28;                                            // 50 ms. 20 ms leaves the page unreachable.
  m_pConnParams.itvl_max = 0x50;                                            // 100 ms"""

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

updated = text.replace(old_scan, new_scan, 1).replace(old_update, new_update, 1)
if updated != text:
    open(client, "w", encoding="utf-8").write(updated)

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
