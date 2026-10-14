#ifndef UART_INTERCEPTOR_H
#define UART_INTERCEPTOR_H

#include <Arduino.h>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/portmacro.h>
#include "driver/uart.h"
#include "esp_intr_alloc.h"

class UARTInterceptor {
public:
    enum Direction {
        DEVICE1_TO_DEVICE2 = 1,
        DEVICE2_TO_DEVICE1 = 2
    };

    enum PacketHandlerResult {
        PACKET_NOT_MODIFIED = 0,
        PACKET_MODIFIED = 1,
        PACKET_DROP = -1
    };

    using PacketHandlerCallback = std::function<PacketHandlerResult(uint8_t* packet, size_t& length, Direction direction)>;
    using PacketFinderCallback = std::function<void(uint8_t* packetBuffer, size_t* packetBufferPosition, uint8_t* newBytes, size_t* newBytesLength, std::function<void()> sendPacketCallback)>;

    void begin(int u1_rx, int u1_tx, int u2_rx, int u2_tx, uint32_t u1_baud, uint32_t u2_baud, size_t u1_max_packet_size, size_t u2_max_packet_size);
    void setPacketHandler(PacketHandlerCallback callback);
    void setPacketFinder(PacketFinderCallback callback);
    void setPacketFinderStartEndSig(uint8_t* start_sig, size_t start_sig_length, uint8_t* end_sig, size_t end_sig_length);
    void sendPacket(uint8_t* packet, size_t length, Direction direction);
    void tick();
    // Software emulation of the physical unplug that clears a wedged base
    // UART: tri-state the base-side pins, drop in-flight toward-base bytes,
    // then reattach and re-sync after `ms`.
    void wireUnplug(uint32_t ms);
    bool wireUnplugActive() const { return unplugActive; }
    uint32_t wireUnplugCount() const { return unplugCount; }
    uint32_t rxAgeMs(Direction fromDevice) const;
    uint32_t txAgeMs(Direction toDevice) const;
    bool bothSidesLive(uint32_t staleMs = 800) const;
    size_t pendingTo1() const { return pendTo1.used; }
    size_t pendingTo2() const { return pendTo2.used; }
    size_t pendingCap() const { return pendTo1.cap; }
    size_t ringUsed1() const { return ringUsed(hwR1, hwW1, kHwRing); }
    size_t ringUsed2() const { return ringUsed(hwR2, hwW2, kHwRing); }
    size_t ringCap() const { return kHwRing; }
    size_t holdUsed1() const { return ringUsed(holdR1, holdW1, kHold); }
    size_t holdUsed2() const { return ringUsed(holdR2, holdW2, kHold); }
    size_t holdCap() const { return kHold; }
    uint32_t rxFrom1() const { return rxCount1; }
    uint32_t rxFrom2() const { return rxCount2; }
    uint32_t cutThroughFrom1() const { return cutCount1; }
    uint32_t cutThroughFrom2() const { return cutCount2; }
    uint32_t hwRxFifo(uart_port_t port) const;
    uint32_t hwIntSt(uart_port_t port) const;
    uint32_t hwIntRaw(uart_port_t port) const;
    uint32_t droppedFrom1() const { return dropCount1; }
    uint32_t droppedFrom2() const { return dropCount2; }
    uint32_t overflowFrom1() const { return ovfCount1; }
    uint32_t overflowFrom2() const { return ovfCount2; }
    uint32_t frameErrFrom1() const { return frmCount1; }
    uint32_t frameErrFrom2() const { return frmCount2; }
    uint32_t maxGapFrom1() const { return maxGap1; }
    uint32_t maxGapFrom2() const { return maxGap2; }
    bool txToBasePaused() const { return pauseTx1; }
    uint32_t txPauseCount() const { return pauseCount; }
    bool workPending();
    bool wirePending();

private:
    struct FramerState {
        size_t startSigMatchIndex = 0;
        bool packetInProgress = false;
    };
    struct Inject {
        uint8_t data[32];
        uint8_t length;
        uint8_t direction;
    };
    struct Pend {
        uint8_t* buf = nullptr;
        size_t cap = 0;
        size_t head = 0;
        size_t used = 0;
    };

    std::vector<uint8_t> buffer1;
    std::vector<uint8_t> buffer2;
    size_t buffer1Position = 0;
    size_t buffer2Position = 0;
    size_t buffer1MaxSize = 0;
    size_t buffer2MaxSize = 0;
    std::vector<uint8_t> startSignature;
    std::vector<uint8_t> endSignature;
    PacketHandlerCallback packetHandler;
    PacketFinderCallback packetFinder;
    FramerState framerState1;
    FramerState framerState2;
    FramerState* activeFramer = nullptr;
    size_t activeMaxPacketSize = 0;
    volatile uint32_t lastRx1Ms = 0;
    volatile uint32_t lastRx2Ms = 0;
    volatile uint32_t lastTx1Ms = 0;
    volatile uint32_t lastTx2Ms = 0;
    volatile uint32_t rxCount1 = 0;
    volatile uint32_t rxCount2 = 0;
    volatile uint32_t cutCount1 = 0;
    volatile uint32_t cutCount2 = 0;
    volatile uint32_t dropCount1 = 0;
    volatile uint32_t dropCount2 = 0;
    volatile uint32_t ovfCount1 = 0;
    volatile uint32_t ovfCount2 = 0;
    volatile uint32_t frmCount1 = 0;
    volatile uint32_t frmCount2 = 0;
    volatile uint32_t maxGap1 = 0;
    volatile uint32_t maxGap2 = 0;
    // Mixer→base cut-through is held when the base has gone silent so the
    // TX line can idle HIGH and the base UART can re-lock after a framing
    // error. BLE radio TDM is enough to glitch a start bit; continuous
    // mixer traffic then never gives the base an idle gap.
    volatile bool pauseTx1 = false;
    volatile uint32_t pauseCount = 0;
    volatile uint8_t alignTx1 = 0;
    // Base-wedge wire unplug state
    volatile bool unplugActive = false;
    volatile uint32_t unplugCount = 0;
    uint32_t unplugUntilMs = 0;
    int pinTx1 = -1;
    int pinRx1 = -1;
    uint16_t paceN = 0;
    uint16_t paceNeed = 0;
    uint32_t pauseUntilMs = 0;
    uint32_t nextPauseMs = 0;
    uint32_t seenRx1 = 0;
    uint32_t seenRx2 = 0;
    uint32_t seenCut1 = 0;
    uint32_t seenCut2 = 0;
    SemaphoreHandle_t pumpLock = nullptr;
    portMUX_TYPE txMux = portMUX_INITIALIZER_UNLOCKED;

    // ~2.8 s at 115200: ALL_ENTITIES (~234 B) plus a full per-control dump
    // and slack for a short unplug. PSRAM so the ISR DRAM rings stay small.
    static constexpr size_t kPend = 32768;
    static constexpr size_t kPendFallback = 4096;
    Pend pendTo2;
    Pend pendTo1;

    static constexpr int kInjectQ = 8;
    Inject injectQ[kInjectQ] = {};
    volatile uint8_t injectHead = 0;
    volatile uint8_t injectTail = 0;
    portMUX_TYPE injectMux = portMUX_INITIALIZER_UNLOCKED;

    // Mixer↔base is a transparent wire. The IRAM ISR copies every RX byte
    // to the other TX FIFO in order. hold[] is only a TX-full spill; newer
    // bytes never pass older ones. parse[] is sniff-only for entity state.
    static constexpr size_t kHwRing = 2048;
    static constexpr size_t kHold = 1024;
    uint8_t hwRing1[kHwRing] = {};
    uint8_t hwRing2[kHwRing] = {};
    uint8_t holdRing1[kHold] = {};
    uint8_t holdRing2[kHold] = {};
    volatile uint16_t hwW1 = 0;
    volatile uint16_t hwR1 = 0;
    volatile uint16_t hwW2 = 0;
    volatile uint16_t hwR2 = 0;
    volatile uint16_t holdW1 = 0;
    volatile uint16_t holdR1 = 0;
    volatile uint16_t holdW2 = 0;
    volatile uint16_t holdR2 = 0;
    intr_handle_t uartIntr1 = nullptr;
    intr_handle_t uartIntr2 = nullptr;

    static size_t ringUsed(volatile uint16_t r, volatile uint16_t w, size_t cap);
    static size_t pendSpace(const Pend& pend);
    bool allocPend(Pend& pend);
    size_t enqueuePend(Pend& pend, const uint8_t* data, size_t length);
    void flushPend(uart_port_t uartNum, Pend& pend, volatile uint32_t& lastTxMs);
    void drainInject();
    void drainHwFromTask();
    void processPort(HardwareSerial& rx, HardwareSerial& tx,
        std::vector<uint8_t>& buffer, size_t& bufferPosition, Direction direction,
        volatile uint32_t& rxCount, volatile uint32_t& cutCount, volatile uint32_t& drops);
    void IRAM_ATTR pullHwFifo(uart_port_t rxPort, uart_port_t txPort,
        uint8_t* parseRing, volatile uint16_t& parseW, volatile uint16_t& parseR, size_t parseCap,
        uint8_t* holdRing, volatile uint16_t& holdW, volatile uint16_t& holdR, size_t holdCap,
        volatile uint32_t& drops, volatile uint32_t& ovf, volatile uint32_t& frm,
        volatile uint32_t& rxCount, volatile uint32_t& cutCount);
    void IRAM_ATTR drainHoldLocked(uart_port_t txPort, uint8_t* holdRing,
        uint16_t& holdR, uint16_t holdW, uint16_t holdMask, volatile uint32_t& cutCount,
        bool allowTx);
    void IRAM_ATTR noteMixerFrame(uint8_t b);
    void IRAM_ATTR pollHwIsr();
    void pollHwFromTask();
    void drainHoldFromTask();
    void unplugTick();
    void pushHold(uint8_t* holdRing, volatile uint16_t& holdW, volatile uint16_t holdR,
        size_t holdCap, const uint8_t* data, size_t n, volatile uint32_t& drops);
    void armRx();
    void stampAgesFromTask();
    void updateResyncPause(uint32_t now);
    void flushToBase();
    static void IRAM_ATTR pollThunk(void* arg);
    size_t popHwRing(uint8_t* ring, volatile uint16_t& r, volatile uint16_t w, size_t cap, uint8_t* out, size_t outCap);
    void pumpHold(uint8_t* ring, volatile uint16_t& r, volatile uint16_t& w, size_t cap, Pend& destPend);
    void pumpParse(uint8_t* ring, volatile uint16_t& r, volatile uint16_t& w, size_t cap,
        std::vector<uint8_t>& buffer, size_t& bufferPosition, Direction direction);
};

#endif // UART_INTERCEPTOR_H
