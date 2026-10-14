#ifndef DYNAMIC_MULTIPRINT_H
#define DYNAMIC_MULTIPRINT_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

class DynamicMultiPrint : public Print {
  public:
    DynamicMultiPrint();
    ~DynamicMultiPrint();

    void setPrint1(Print* print);
    void setPrint2(Print* print);

    void setBuffer1(bool enable);
    void setBuffer2(bool enable);
    bool buffer2() const { return _buffer2; }

    // Telnet client is up. Live bytes go into the ring even when history is off.
    void setPrint2Live(bool live);
    bool print2Live() const { return _print2Live; }

    using Print::write;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;

    // Ring only: never USB serial, never the telnet socket. Safe from the UART task.
    size_t writeWifi(const uint8_t* buffer, size_t size);

    size_t drainPrint2(size_t maxBytes);
    size_t wifiBuffered() const;
    uint32_t wifiDropped() const;
    uint32_t wifiSeq() const;
    size_t copyWifiSince(uint32_t since, uint8_t* out, size_t cap, uint32_t* next);
    void clearWifi();

  private:
    static const size_t WIFI_LOG_CAP = 65536;
    static const size_t WIFI_LOG_FALLBACK = 8192;

    Print* _print1 = nullptr;
    Print* _print2 = nullptr;
    bool _buffer1 = false;
    bool _buffer2 = false;
    bool _print2Live = false;

    uint8_t* _wifiBuf = nullptr;
    size_t _wifiCap = 0;
    size_t _wifiHead = 0;
    size_t _wifiTail = 0;
    size_t _wifiUsed = 0;
    uint32_t _wifiDropped = 0;
    uint32_t _wifiSeq = 0;
    mutable portMUX_TYPE _wifiMux;

    bool capturePrint2() const { return _buffer2 || _print2Live; }
    void ensureWifiBuffer();
    void pushUnlocked(const uint8_t* data, size_t n);
    size_t popUnlocked(uint8_t* out, size_t maxBytes);
    void clearUnlocked();
};

#endif // DYNAMIC_MULTIPRINT_H
