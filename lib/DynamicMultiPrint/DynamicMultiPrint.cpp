#include "DynamicMultiPrint.h"
#include <esp_heap_caps.h>
#include <cstring>

DynamicMultiPrint::DynamicMultiPrint()
    : _print1(nullptr), _print2(nullptr),
      _buffer1(false), _buffer2(false), _print2Live(false),
      _wifiBuf(nullptr), _wifiCap(0), _wifiHead(0), _wifiTail(0),
      _wifiUsed(0), _wifiDropped(0), _wifiSeq(0),
      _wifiMux(portMUX_INITIALIZER_UNLOCKED) {}

DynamicMultiPrint::~DynamicMultiPrint() {
    if (_wifiBuf != nullptr) {
        heap_caps_free(_wifiBuf);
        _wifiBuf = nullptr;
    }
}

void DynamicMultiPrint::setPrint1(Print* print) {
    _print1 = print;
}

void DynamicMultiPrint::setPrint2(Print* print) {
    _print2 = print;
}

void DynamicMultiPrint::setBuffer1(bool enable) {
    _buffer1 = enable;
}

void DynamicMultiPrint::setBuffer2(bool enable) {
    _buffer2 = enable;
    if (enable) {
        ensureWifiBuffer();
        return;
    }
    if (!_print2Live) {
        clearWifi();
    }
}

void DynamicMultiPrint::setPrint2Live(bool live) {
    _print2Live = live;
    if (live) {
        ensureWifiBuffer();
    } else if (!_buffer2) {
        clearWifi();
    }
}

void DynamicMultiPrint::ensureWifiBuffer() {
    if (_wifiBuf != nullptr && _wifiCap > 0) {
        return;
    }
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(WIFI_LOG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t cap = WIFI_LOG_CAP;
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(
            heap_caps_malloc(WIFI_LOG_FALLBACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        cap = WIFI_LOG_FALLBACK;
    }
    portENTER_CRITICAL(&_wifiMux);
    if (_wifiBuf == nullptr) {
        _wifiBuf = buf;
        _wifiCap = buf == nullptr ? 0 : cap;
        buf = nullptr;
    }
    portEXIT_CRITICAL(&_wifiMux);
    if (buf != nullptr) {
        heap_caps_free(buf);
    }
}

size_t DynamicMultiPrint::write(uint8_t c) {
    return write(&c, 1);
}

size_t DynamicMultiPrint::write(const uint8_t* buffer, size_t size) {
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    if (_print1 != nullptr) {
        // USB Serial/JTAG blocks setup() for seconds when the host enumerates
        // ACM0 but nobody is reading. Drop bytes rather than stall BLE/HTTP.
        if (_print1 == static_cast<Print*>(&Serial)) {
            int room = Serial.availableForWrite();
            if (room > 0) {
                size_t n = size;
                if ((size_t)room < n) {
                    n = (size_t)room;
                }
                _print1->write(buffer, n);
            }
        } else {
            _print1->write(buffer, size);
        }
    }
    writeWifi(buffer, size);
    return size;
}

size_t DynamicMultiPrint::writeWifi(const uint8_t* buffer, size_t size) {
    if (buffer == nullptr || size == 0 || !capturePrint2()) {
        return 0;
    }
    ensureWifiBuffer();
    portENTER_CRITICAL(&_wifiMux);
    if (!capturePrint2()) {
        portEXIT_CRITICAL(&_wifiMux);
        return 0;
    }
    pushUnlocked(buffer, size);
    portEXIT_CRITICAL(&_wifiMux);
    return size;
}

size_t DynamicMultiPrint::drainPrint2(size_t maxBytes) {
    if (_print2 == nullptr || maxBytes == 0) {
        return 0;
    }
    uint8_t chunk[256];
    size_t total = 0;
    while (total < maxBytes) {
        size_t want = maxBytes - total;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        size_t got = 0;
        portENTER_CRITICAL(&_wifiMux);
        got = popUnlocked(chunk, want);
        portEXIT_CRITICAL(&_wifiMux);
        if (got == 0) {
            break;
        }
        _print2->write(chunk, got);
        total += got;
    }
    return total;
}

size_t DynamicMultiPrint::wifiBuffered() const {
    portENTER_CRITICAL(&_wifiMux);
    size_t used = _wifiUsed;
    portEXIT_CRITICAL(&_wifiMux);
    return used;
}

uint32_t DynamicMultiPrint::wifiDropped() const {
    portENTER_CRITICAL(&_wifiMux);
    uint32_t dropped = _wifiDropped;
    portEXIT_CRITICAL(&_wifiMux);
    return dropped;
}

uint32_t DynamicMultiPrint::wifiSeq() const {
    portENTER_CRITICAL(&_wifiMux);
    uint32_t seq = _wifiSeq;
    portEXIT_CRITICAL(&_wifiMux);
    return seq;
}

size_t DynamicMultiPrint::copyWifiSince(uint32_t since, uint8_t* out, size_t cap, uint32_t* next) {
    if (out == nullptr || cap == 0) {
        if (next != nullptr) {
            *next = since;
        }
        return 0;
    }
    portENTER_CRITICAL(&_wifiMux);
    uint32_t seq = _wifiSeq;
    size_t used = _wifiUsed;
    size_t tail = _wifiTail;
    size_t ring = _wifiCap;
    uint8_t* buf = _wifiBuf;
    uint32_t oldest = seq - static_cast<uint32_t>(used);
    if (since < oldest) {
        since = oldest;
    }
    if (since > seq) {
        since = seq;
    }
    size_t skip = static_cast<size_t>(since - oldest);
    size_t avail = used > skip ? used - skip : 0;
    size_t n = avail < cap ? avail : cap;
    if (buf != nullptr && ring > 0 && n > 0) {
        size_t start = (tail + skip) % ring;
        size_t first = n;
        size_t toEnd = ring - start;
        if (first > toEnd) {
            first = toEnd;
        }
        memcpy(out, buf + start, first);
        if (n > first) {
            memcpy(out + first, buf, n - first);
        }
    } else {
        n = 0;
    }
    portEXIT_CRITICAL(&_wifiMux);
    if (next != nullptr) {
        *next = since + static_cast<uint32_t>(n);
    }
    return n;
}

void DynamicMultiPrint::clearWifi() {
    portENTER_CRITICAL(&_wifiMux);
    clearUnlocked();
    portEXIT_CRITICAL(&_wifiMux);
}

void DynamicMultiPrint::pushUnlocked(const uint8_t* data, size_t n) {
    if (_wifiBuf == nullptr || _wifiCap == 0 || n == 0) {
        return;
    }
    if (n >= _wifiCap) {
        _wifiDropped += _wifiUsed + (n - _wifiCap);
        data += n - _wifiCap;
        n = _wifiCap;
        memcpy(_wifiBuf, data, n);
        _wifiHead = 0;
        _wifiTail = 0;
        _wifiUsed = _wifiCap;
        _wifiSeq += static_cast<uint32_t>(n);
        return;
    }
    size_t room = _wifiCap - _wifiUsed;
    if (n > room) {
        size_t drop = n - room;
        _wifiTail = (_wifiTail + drop) % _wifiCap;
        _wifiUsed -= drop;
        _wifiDropped += drop;
    }
    size_t first = n;
    size_t toEnd = _wifiCap - _wifiHead;
    if (first > toEnd) {
        first = toEnd;
    }
    memcpy(_wifiBuf + _wifiHead, data, first);
    if (n > first) {
        memcpy(_wifiBuf, data + first, n - first);
    }
    _wifiHead = (_wifiHead + n) % _wifiCap;
    _wifiUsed += n;
    _wifiSeq += static_cast<uint32_t>(n);
}

size_t DynamicMultiPrint::popUnlocked(uint8_t* out, size_t maxBytes) {
    if (_wifiBuf == nullptr || _wifiUsed == 0 || maxBytes == 0) {
        return 0;
    }
    size_t n = maxBytes < _wifiUsed ? maxBytes : _wifiUsed;
    size_t first = n;
    size_t toEnd = _wifiCap - _wifiTail;
    if (first > toEnd) {
        first = toEnd;
    }
    memcpy(out, _wifiBuf + _wifiTail, first);
    if (n > first) {
        memcpy(out + first, _wifiBuf, n - first);
    }
    _wifiTail = (_wifiTail + n) % _wifiCap;
    _wifiUsed -= n;
    return n;
}

void DynamicMultiPrint::clearUnlocked() {
    _wifiHead = 0;
    _wifiTail = 0;
    _wifiUsed = 0;
    _wifiDropped = 0;
}
