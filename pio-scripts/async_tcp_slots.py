# AsyncTCP's closed-slot pool: every closed TCP connection consumes an entry
# from a CONFIG_LWIP_MAX_ACTIVE_TCP (16) sized array; a connection that
# closes without a clean _free_closed_slot (browser half-open during a
# latency spike, module wedge, retry storm) leaks it. When all 16 leak,
# _allocate_closed_slot fails forever: lwip still completes the handshake
# (SYN-ACK) but no AsyncClient is created, so requests are never processed —
# the "connect OK, no response" white page. 2026-10-14.
#
# Fix: enlarge the pool (16 -> 160) and make _allocate_closed_slot
# force-recycle the oldest occupied slot when everything is leaked, so the
# wedge becomes impossible regardless of leak rate.
Import("env")
import glob
import os
import site

candidates = []
proj = env["PROJECT_DIR"]
candidates += glob.glob(os.path.join(proj, ".pio", "libdeps", "*", "AsyncTCP", "src", "AsyncTCP.cpp"))
try:
    pkg = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if pkg:
        candidates += glob.glob(os.path.join(pkg, "libraries", "AsyncTCP", "src", "AsyncTCP.cpp"))
except Exception:
    pass
# site-packages location of AsyncTCP when installed as a framework lib
try:
    import framework_async
except ImportError:
    pass

old_pool = "const int _number_of_closed_slots = CONFIG_LWIP_MAX_ACTIVE_TCP;"
new_pool = """const int _number_of_closed_slots = 160;  // 16 leaked into a permanent TCP wedge (2026-10-14)
// Diagnostics for the request-pipeline wedge (2026-10-14): JTAG-read counters
// that show exactly where a connection stops — accept callback, event queued,
// dispatched to the server.
volatile uint32_t _dbg_accept_calls = 0;
volatile uint32_t _dbg_accept_queued = 0;
volatile uint32_t _dbg_accepted_dispatched = 0;"""

old_alloc = """  xSemaphoreTake(_slots_lock, portMAX_DELAY);
  uint32_t closed_slot_min_index = 0;
  for (int i = 0; i < _number_of_closed_slots; ++i) {
    if ((_closed_slot == INVALID_CLOSED_SLOT || _closed_slots[i] <= closed_slot_min_index) && _closed_slots[i] != 0) {
      closed_slot_min_index = _closed_slots[i];
      _closed_slot = i;
    }
  }
  if (_closed_slot != INVALID_CLOSED_SLOT) {
    _closed_slots[_closed_slot] = 0;
  }
  xSemaphoreGive(_slots_lock);
  return (_closed_slot != INVALID_CLOSED_SLOT);"""
new_alloc = """  xSemaphoreTake(_slots_lock, portMAX_DELAY);
  uint32_t closed_slot_min_index = 0;
  uint32_t oldest = 0xFFFFFFFF;
  int oldest_i = -1;
  for (int i = 0; i < _number_of_closed_slots; ++i) {
    if (_closed_slots[i] == 0) {
      continue; // occupied by a live connection
    }
    if ((_closed_slot == INVALID_CLOSED_SLOT || _closed_slots[i] <= closed_slot_min_index)) {
      closed_slot_min_index = _closed_slots[i];
      _closed_slot = i;
    }
    if (_closed_slots[i] < oldest) {
      oldest = _closed_slots[i];
      oldest_i = i;
    }
  }
  if (_closed_slot == INVALID_CLOSED_SLOT && oldest_i >= 0) {
    // All slots leaked (unclosed connections). Steal the oldest: a queued
    // event for the dead connection may land on the new one, which is a
    // transient glitch — far better than a permanent listen wedge.
    _closed_slot = oldest_i;
  }
  if (_closed_slot != INVALID_CLOSED_SLOT) {
    _closed_slots[_closed_slot] = 0;
  }
  xSemaphoreGive(_slots_lock);
  return (_closed_slot != INVALID_CLOSED_SLOT);"""

patched = 0
for path in candidates:
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        continue
    changed = 0
    # Counters: insert declarations before _slots_lock if missing (older patch
    # runs may have added the increments without the declarations).
    if "_dbg_accept_calls = 0" not in text and "_dbg_accept_calls++" in text:
        marker = "SemaphoreHandle_t _slots_lock;"
        decls = ("volatile uint32_t _dbg_accept_calls = 0;\n"
                 "volatile uint32_t _dbg_accept_queued = 0;\n"
                 "volatile uint32_t _dbg_accepted_dispatched = 0;\n")
        text = text.replace(marker, decls + marker, 1)
        changed += 1
    elif "_dbg_accept_calls = 0" not in text:
        marker = "SemaphoreHandle_t _slots_lock;"
        decls = ("// Diagnostics for the request-pipeline wedge: JTAG-read counters\n"
                 "volatile uint32_t _dbg_accept_calls = 0;\n"
                 "volatile uint32_t _dbg_accept_queued = 0;\n"
                 "volatile uint32_t _dbg_accepted_dispatched = 0;\n")
        text = text.replace(marker, decls + marker, 1)
        changed += 1
    if old_pool in text and "_number_of_closed_slots = 160" not in text:
        text = text.replace(old_pool, "const int _number_of_closed_slots = 160;  // 16 leaked into a permanent TCP wedge (2026-10-14)", 1)
        changed += 1
    if old_alloc in text and "oldest_i" not in text:
        text = text.replace(old_alloc, new_alloc, 1)
        changed += 1
    old_accept = """static int8_t _tcp_accept(void* arg, AsyncClient* client) {
  lwip_event_packet_t* e = (lwip_event_packet_t*)malloc(sizeof(lwip_event_packet_t));
  if (e == nullptr) {
    return ERR_MEM;
  }
  e->event = LWIP_TCP_ACCEPT;"""
new_accept = """static int8_t _tcp_accept(void* arg, AsyncClient* client) {
  lwip_event_packet_t* e = (lwip_event_packet_t*)malloc(sizeof(lwip_event_packet_t));
  _dbg_accept_calls++;
  if (e == nullptr) {
    return ERR_MEM;
  }
  e->event = LWIP_TCP_ACCEPT;"""
if old_accept in text and "_dbg_accept_calls++" not in text:
    text = text.replace(old_accept, new_accept, 1)
    changed += 1

    old_recv = """static int8_t _tcp_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* pb, int8_t err) {
  lwip_event_packet_t* e = (lwip_event_packet_t*)malloc(sizeof(lwip_event_packet_t));
  e->arg = arg;"""
    new_recv = """static int8_t _tcp_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* pb, int8_t err) {
  lwip_event_packet_t* e = (lwip_event_packet_t*)malloc(sizeof(lwip_event_packet_t));
  _dbg_recv_calls++;
  e->arg = arg;"""
    if old_recv in text and "_dbg_recv_calls++" not in text:
        text = text.replace(old_recv, new_recv, 1)
        changed += 1

    old_recv_decl = "volatile uint32_t _dbg_accepted_dispatched = 0;"
    new_recv_decl = ("volatile uint32_t _dbg_accepted_dispatched = 0;\n"
                     "volatile uint32_t _dbg_recv_calls = 0;")
    if "_dbg_recv_calls" in text and "_dbg_recv_calls = 0" not in text:
        text = text.replace(old_recv_decl, new_recv_decl, 1)
        changed += 1

old_queue_ok = """  e->event = LWIP_TCP_ACCEPT;
  e->arg = arg;
  e->accept.client = client;
  if (!_prepend_async_event(&e)) {
    free((void*)(e));
  }
  return ERR_OK;"""
new_queue_ok = """  e->event = LWIP_TCP_ACCEPT;
  e->arg = arg;
  e->accept.client = client;
  if (!_prepend_async_event(&e)) {
    free((void*)(e));
  } else {
    _dbg_accept_queued++;
  }
  return ERR_OK;"""
if old_queue_ok in text and "_dbg_accept_queued++" not in text:
    text = text.replace(old_queue_ok, new_queue_ok, 1)
    changed += 1

old_dispatch = """int8_t AsyncServer::_accepted(AsyncClient* client) {
  if (_connect_cb) {
    _connect_cb(_connect_cb_arg, client);
  }
  return ERR_OK;
}"""
new_dispatch = """int8_t AsyncServer::_accepted(AsyncClient* client) {
  _dbg_accepted_dispatched++;
  if (_connect_cb) {
    _connect_cb(_connect_cb_arg, client);
  }
  return ERR_OK;
}"""
if old_dispatch in text and "_dbg_accepted_dispatched++" not in text:
    text = text.replace(old_dispatch, new_dispatch, 1)
    changed += 1

if changed:
    open(path, "w", encoding="utf-8").write(text)
    patched += changed
    print(f"async_tcp_slots.py: patched {path} (+{changed} diag counters)")
if not candidates or patched == 0 and not any("oldest_i" in open(p, encoding="utf-8").read() for p in candidates):
    print("async_tcp_slots.py: WARNING — no AsyncTCP.cpp patched!")