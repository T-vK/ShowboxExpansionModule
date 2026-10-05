#include <cstddef>
#include <cstdint>
#include <esp_heap_caps.h>

// NimBLE is built with CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL, so every
// host buffer lands in DIRAM and HTTP/ICMP starve. Controller DMA still uses
// bt_osi_mem_*_internal; those symbols are not wrapped.
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

void* __real_bt_osi_mem_malloc(size_t size);
void* __real_bt_osi_mem_calloc(size_t n, size_t size);

static void* allocPsram(size_t size, bool zero) {
    void* p = zero
        ? heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != nullptr) {
        sHits++;
        sBytes += (uint32_t)size;
        return p;
    }
    sMisses++;
    return nullptr;
}

void* __wrap_bt_osi_mem_malloc(size_t size) {
    void* p = allocPsram(size, false);
    if (p != nullptr) {
        return p;
    }
    return __real_bt_osi_mem_malloc(size);
}

void* __wrap_bt_osi_mem_calloc(size_t n, size_t size) {
    size_t bytes = n * size;
    void* p = allocPsram(bytes, true);
    if (p != nullptr) {
        return p;
    }
    return __real_bt_osi_mem_calloc(n, size);
}

}
