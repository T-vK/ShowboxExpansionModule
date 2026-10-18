#include "UARTInterceptor.h"
#include <cstring>
#include "soc/uart_reg.h"
#include "hal/uart_ll.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/interrupts.h"
#include <freertos/task.h>
#include <freertos/semphr.h>

static constexpr uint32_t kRxIntrMask =
    UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_RXFIFO_OVF |
    UART_INTR_FRAM_ERR | UART_INTR_BRK_DET;
void IRAM_ATTR UARTInterceptor::drainHoldLocked(
    uart_port_t txPort, uint8_t* holdRing,
    uint16_t& holdR, uint16_t holdW, uint16_t holdMask, volatile uint32_t& cutCount,
    bool allowTx) {
    if (!allowTx) {
        return;
    }
    uart_dev_t* tx = UART_LL_GET_HW(txPort);
    if (txPort == UART_NUM_1 && alignTx1 && !uart_ll_is_tx_idle(tx)) {
        return;
    }
    if (txPort == UART_NUM_1) {
        alignTx1 = 0;
    }
    while (holdR != holdW) {
        if (uart_ll_get_txfifo_len(tx) <= 1) {
            break;
        }
        tx->fifo.val = holdRing[holdR];
        holdR = static_cast<uint16_t>((holdR + 1u) & holdMask);
        cutCount = cutCount + 1;
    }
}

void IRAM_ATTR UARTInterceptor::noteMixerFrame(uint8_t b) {
    if (paceN == 0) {
        paceN = (b == 0xBE) ? 1 : 0;
    } else if (paceN == 1) {
        paceN = (b == 0xEF) ? 2 : (b == 0xBE ? 1 : 0);
    } else if (paceN == 2) {
        paceNeed = static_cast<uint16_t>(b) + 7u;
        if (paceNeed < 7u || paceNeed > 288u) {
            paceN = (b == 0xBE) ? 1 : 0;
            paceNeed = 0;
        } else {
            paceN = 3;
        }
    } else {
        paceN = static_cast<uint16_t>(paceN + 1u);
        if (paceN >= paceNeed) {
            paceN = 0;
            paceNeed = 0;
            alignTx1 = 1;
        }
    }
}

void IRAM_ATTR UARTInterceptor::pullHwFifo(
    uart_port_t rxPort, uart_port_t txPort,
    uint8_t* parseRing, volatile uint16_t& parseW, volatile uint16_t& parseR, size_t parseCap,
    uint8_t* holdRing, volatile uint16_t& holdW, volatile uint16_t& holdR, size_t holdCap,
    volatile uint32_t& drops, volatile uint32_t& ovf, volatile uint32_t& frm,
    volatile uint32_t& rxCount, volatile uint32_t& cutCount) {
    uart_dev_t* rx = UART_LL_GET_HW(rxPort);
    uart_dev_t* tx = UART_LL_GET_HW(txPort);
    const uint32_t raw = REG_READ(UART_INT_RAW_REG(rxPort));
    if (raw & UART_RXFIFO_OVF_INT_RAW) {
        ovf = ovf + 1;
    }
    if (raw & (UART_FRM_ERR_INT_RAW | UART_BRK_DET_INT_RAW)) {
        frm = frm + 1;
    }
    uint16_t pW = parseW;
    uint16_t pR = parseR;
    uint16_t hW = holdW;
    uint16_t hR = holdR;
    const uint16_t parseMask = static_cast<uint16_t>(parseCap - 1u);
    const uint16_t holdMask = static_cast<uint16_t>(holdCap - 1u);
    const bool allowTx = !(txPort == UART_NUM_1 && pauseTx1);
    drainHoldLocked(txPort, holdRing, hR, hW, holdMask, cutCount, allowTx);
    uint32_t cnt = uart_ll_get_rxfifo_len(rx);
    while (cnt > 0) {
        uint8_t b = static_cast<uint8_t>(rx->fifo.val);
        cnt--;
        bool send = allowTx && hW == hR && uart_ll_get_txfifo_len(tx) > 1;
        if (send && txPort == UART_NUM_1 && alignTx1) {
            if (!uart_ll_is_tx_idle(tx)) {
                send = false;
            } else {
                alignTx1 = 0;
            }
        }
        if (send) {
            tx->fifo.val = b;
            cutCount = cutCount + 1;
        } else {
            uint16_t hNext = static_cast<uint16_t>((hW + 1u) & holdMask);
            if (hNext != hR) {
                holdRing[hW] = b;
                hW = hNext;
            } else {
                drops = drops + 1;
            }
        }
        if (txPort == UART_NUM_1) {
            noteMixerFrame(b);
        }
        uint16_t pNext = static_cast<uint16_t>((pW + 1u) & parseMask);
        if (pNext != pR) {
            parseRing[pW] = b;
            pW = pNext;
        }
        rxCount = rxCount + 1;
    }
    drainHoldLocked(txPort, holdRing, hR, hW, holdMask, cutCount, allowTx);
    parseW = pW;
    holdW = hW;
    holdR = hR;
    if (REG_READ(UART_INT_RAW_REG(rxPort)) & UART_RXFIFO_OVF_INT_RAW) {
        ovf = ovf + 1;
    }
    uart_ll_clr_intsts_mask(rx, kRxIntrMask);
}

void IRAM_ATTR UARTInterceptor::pollHwIsr() {
    portENTER_CRITICAL_ISR(&txMux);
    pullHwFifo(UART_NUM_1, UART_NUM_2,
        hwRing1, hwW1, hwR1, kHwRing,
        holdRing1, holdW1, holdR1, kHold,
        dropCount1, ovfCount1, frmCount1, rxCount1, cutCount1);
    pullHwFifo(UART_NUM_2, UART_NUM_1,
        hwRing2, hwW2, hwR2, kHwRing,
        holdRing2, holdW2, holdR2, kHold,
        dropCount2, ovfCount2, frmCount2, rxCount2, cutCount2);
    portEXIT_CRITICAL_ISR(&txMux);
}

void UARTInterceptor::pollHwFromTask() {
    portENTER_CRITICAL(&txMux);
    pullHwFifo(UART_NUM_1, UART_NUM_2,
        hwRing1, hwW1, hwR1, kHwRing,
        holdRing1, holdW1, holdR1, kHold,
        dropCount1, ovfCount1, frmCount1, rxCount1, cutCount1);
    pullHwFifo(UART_NUM_2, UART_NUM_1,
        hwRing2, hwW2, hwR2, kHwRing,
        holdRing2, holdW2, holdR2, kHold,
        dropCount2, ovfCount2, frmCount2, rxCount2, cutCount2);
    portEXIT_CRITICAL(&txMux);
}

void IRAM_ATTR UARTInterceptor::pollThunk(void* arg) {
    static_cast<UARTInterceptor*>(arg)->pollHwIsr();
}

size_t UARTInterceptor::ringUsed(volatile uint16_t r, volatile uint16_t w, size_t cap) {
    return static_cast<size_t>((w - r) & (cap - 1u));
}

size_t UARTInterceptor::pendSpace(const Pend& pend) {
    if (pend.buf == nullptr || pend.cap <= pend.used) {
        return 0;
    }
    return pend.cap - pend.used;
}

bool UARTInterceptor::allocPend(Pend& pend) {
    if (pend.buf != nullptr && pend.cap > 0) {
        pend.head = 0;
        pend.used = 0;
        return true;
    }
    pend.buf = static_cast<uint8_t*>(heap_caps_malloc(kPend, MALLOC_CAP_SPIRAM));
    pend.cap = kPend;
    if (pend.buf == nullptr) {
        pend.buf = static_cast<uint8_t*>(heap_caps_malloc(kPendFallback, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        pend.cap = kPendFallback;
    }
    if (pend.buf == nullptr) {
        pend.cap = 0;
        pend.head = 0;
        pend.used = 0;
        return false;
    }
    pend.head = 0;
    pend.used = 0;
    return true;
}

void UARTInterceptor::begin(int u1_rx, int u1_tx, int u2_rx, int u2_tx, uint32_t u1_baud, uint32_t u2_baud, size_t u1_max_packet_size, size_t u2_max_packet_size) {
    if (pumpLock == nullptr) {
        pumpLock = xSemaphoreCreateMutex();
    }
    allocPend(pendTo1);
    allocPend(pendTo2);
    // Known-good mixer↔base path: HardwareSerial + complete frames only.
    // Custom ISR cut-through kept the TX line busy and the base MCU dropped
    // the link every few seconds even with BLE off. Packet-gate idles the
    // line between frames. BLE is locked off, so the flash UART ISR is fine.
    Serial1.setRxBufferSize(2048);
    Serial1.setTxBufferSize(2048);
    Serial2.setRxBufferSize(2048);
    Serial2.setTxBufferSize(2048);
    Serial1.begin(u1_baud, SERIAL_8N1, u1_rx, u1_tx);
    Serial2.begin(u2_baud, SERIAL_8N1, u2_rx, u2_tx);
    pinRx1 = u1_rx;
    pinTx1 = u1_tx;
    unplugActive = false;
    unplugUntilMs = 0;
    Serial1.setRxTimeout(1);
    Serial2.setRxTimeout(1);
    buffer1MaxSize = u1_max_packet_size;
    buffer2MaxSize = u2_max_packet_size;
    buffer1.assign(u1_max_packet_size, 0);
    buffer2.assign(u2_max_packet_size, 0);
    buffer1Position = 0;
    buffer2Position = 0;
    framerState1 = FramerState();
    framerState2 = FramerState();
    lastRx1Ms = 0;
    lastRx2Ms = 0;
    lastTx1Ms = 0;
    lastTx2Ms = 0;
    rxCount1 = 0;
    rxCount2 = 0;
    cutCount1 = 0;
    cutCount2 = 0;
    dropCount1 = 0;
    dropCount2 = 0;
    ovfCount1 = 0;
    ovfCount2 = 0;
    frmCount1 = 0;
    frmCount2 = 0;
    maxGap1 = 0;
    maxGap2 = 0;
    pauseTx1 = false;
    pauseCount = 0;
    alignTx1 = 0;
    paceN = 0;
    paceNeed = 0;
    pauseUntilMs = 0;
    nextPauseMs = 0;
    hwW1 = 0;
    hwR1 = 0;
    hwW2 = 0;
    hwR2 = 0;
    holdW1 = 0;
    holdR1 = 0;
    holdW2 = 0;
    holdR2 = 0;
    injectHead = 0;
    injectTail = 0;
}

void UARTInterceptor::setPacketHandler(PacketHandlerCallback callback) {
    packetHandler = callback;
}

void UARTInterceptor::setPacketFinder(PacketFinderCallback callback) {
    packetFinder = callback;
}

void UARTInterceptor::setPacketFinderStartEndSig(uint8_t* start_sig, size_t start_sig_length, uint8_t* end_sig, size_t end_sig_length) {
    startSignature.assign(start_sig, start_sig + start_sig_length);
    endSignature.assign(end_sig, end_sig + end_sig_length);

    packetFinder = [this](uint8_t* packetBuffer, size_t* packetBufferPosition, uint8_t* newBytes, size_t* newBytesLength, std::function<void()> sendPacketCallback) {
        if (activeFramer == nullptr || activeMaxPacketSize == 0 || startSignature.empty()) {
            return;
        }

        FramerState& framer = *activeFramer;
        size_t i = 0;
        while (i < *newBytesLength) {
            uint8_t byte = newBytes[i];
            bool reprocess = false;

            if (!framer.packetInProgress) {
                if (byte == startSignature[framer.startSigMatchIndex]) {
                    framer.startSigMatchIndex++;
                    if (framer.startSigMatchIndex == startSignature.size()) {
                        framer.packetInProgress = true;
                        framer.startSigMatchIndex = 0;
                        *packetBufferPosition = 0;
                        if (startSignature.size() > activeMaxPacketSize) {
                            framer.packetInProgress = false;
                        } else {
                            for (size_t j = 0; j < startSignature.size(); ++j) {
                                packetBuffer[*packetBufferPosition] = startSignature[j];
                                (*packetBufferPosition)++;
                            }
                        }
                    }
                } else {
                    framer.startSigMatchIndex = 0;
                    if (startSignature.size() > 1 && byte == startSignature[0]) {
                        framer.startSigMatchIndex = 1;
                    }
                }
            } else if (*packetBufferPosition >= activeMaxPacketSize) {
                *packetBufferPosition = 0;
                framer.packetInProgress = false;
                framer.startSigMatchIndex = 0;
                reprocess = true;
            } else {
                packetBuffer[*packetBufferPosition] = byte;
                (*packetBufferPosition)++;
                if (*packetBufferPosition >= 3) {
                    size_t expected = static_cast<size_t>(packetBuffer[2]) + 7;
                    if (expected < 7 || expected > activeMaxPacketSize) {
                        *packetBufferPosition = 0;
                        framer.packetInProgress = false;
                        framer.startSigMatchIndex = 0;
                        reprocess = true;
                    } else if (*packetBufferPosition >= expected) {
                        sendPacketCallback();
                        *packetBufferPosition = 0;
                        framer.packetInProgress = false;
                        framer.startSigMatchIndex = 0;
                    }
                }
            }

            if (!reprocess) {
                i++;
            }
        }
    };
}

size_t UARTInterceptor::enqueuePend(Pend& pend, const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0 || pend.buf == nullptr || pend.cap == 0) {
        return 0;
    }
    size_t space = pendSpace(pend);
    if (space == 0) {
        return 0;
    }
    size_t n = length < space ? length : space;
    size_t remaining = n;
    const uint8_t* src = data;
    while (remaining > 0) {
        size_t tail = (pend.head + pend.used) % pend.cap;
        size_t contig = pend.cap - tail;
        if (contig > remaining) {
            contig = remaining;
        }
        memcpy(pend.buf + tail, src, contig);
        pend.used += contig;
        src += contig;
        remaining -= contig;
    }
    return n;
}

void UARTInterceptor::flushPend(uart_port_t uartNum, Pend& pend, volatile uint32_t& lastTxMs) {
    if (pend.buf == nullptr || pend.cap == 0) {
        return;
    }
    uart_dev_t* hw = UART_LL_GET_HW(uartNum);
    while (pend.used > 0) {
        uint32_t space = uart_ll_get_txfifo_len(hw);
        if (space <= 1) {
            vTaskDelay(0);
            space = uart_ll_get_txfifo_len(hw);
            if (space <= 1) {
                return;
            }
        }
        portENTER_CRITICAL(&txMux);
        space = uart_ll_get_txfifo_len(hw);
        if (space <= 1) {
            portEXIT_CRITICAL(&txMux);
            return;
        }
        space -= 1;
        size_t contig = pend.cap - pend.head;
        if (contig > pend.used) {
            contig = pend.used;
        }
        if (contig > space) {
            contig = space;
        }
        uart_ll_write_txfifo(hw, pend.buf + pend.head, static_cast<uint32_t>(contig));
        lastTxMs = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        pend.head = (pend.head + contig) % pend.cap;
        pend.used -= contig;
        portEXIT_CRITICAL(&txMux);
    }
}

void UARTInterceptor::sendPacket(uint8_t* packet, size_t length, Direction direction) {
    if (packet == nullptr || length == 0) {
        return;
    }
    bool took = false;
    if (pumpLock != nullptr && xSemaphoreGetMutexHolder(pumpLock) != xTaskGetCurrentTaskHandle()) {
        took = xSemaphoreTake(pumpLock, pdMS_TO_TICKS(8)) == pdTRUE;
    }
    if (direction == DEVICE2_TO_DEVICE1) {
        if (unplugActive) {
            // Line is tri-stated toward the base; nothing in flight may leak
            // out when the pins reattach mid-byte.
            if (took) {
                xSemaphoreGive(pumpLock);
            }
            return;
        }
        Serial1.write(packet, length);
        lastTx1Ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    } else if (direction == DEVICE1_TO_DEVICE2) {
        Serial2.write(packet, length);
        lastTx2Ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }
    if (took) {
        xSemaphoreGive(pumpLock);
    }
}

void UARTInterceptor::pushHold(uint8_t* holdRing, volatile uint16_t& holdW, volatile uint16_t holdR,
    size_t holdCap, const uint8_t* data, size_t n, volatile uint32_t& drops) {
    if (data == nullptr || n == 0) {
        return;
    }
    const uint16_t holdMask = static_cast<uint16_t>(holdCap - 1u);
    portENTER_CRITICAL(&txMux);
    uint16_t hW = holdW;
    uint16_t hR = holdR;
    for (size_t i = 0; i < n; i++) {
        uint16_t hNext = static_cast<uint16_t>((hW + 1u) & holdMask);
        if (hNext == hR) {
            drops = drops + 1;
            break;
        }
        holdRing[hW] = data[i];
        hW = hNext;
    }
    holdW = hW;
    portEXIT_CRITICAL(&txMux);
}

void UARTInterceptor::drainHoldFromTask() {
    portENTER_CRITICAL(&txMux);
    uint16_t hR1 = holdR1;
    uint16_t hW1 = holdW1;
    uint16_t hR2 = holdR2;
    uint16_t hW2 = holdW2;
    drainHoldLocked(UART_NUM_2, holdRing1, hR1, hW1, static_cast<uint16_t>(kHold - 1u), cutCount1, true);
    drainHoldLocked(UART_NUM_1, holdRing2, hR2, hW2, static_cast<uint16_t>(kHold - 1u), cutCount2, !pauseTx1);
    holdR1 = hR1;
    holdR2 = hR2;
    portEXIT_CRITICAL(&txMux);
}

void UARTInterceptor::processPort(HardwareSerial& rx, HardwareSerial& tx,
    std::vector<uint8_t>& buffer, size_t& bufferPosition, Direction direction,
    volatile uint32_t& rxCount, volatile uint32_t& cutCount, volatile uint32_t& drops) {
    int avail = rx.available();
    while (avail > 0) {
        uint8_t tmp[256];
        size_t want = static_cast<size_t>(avail) > sizeof(tmp) ? sizeof(tmp) : static_cast<size_t>(avail);
        size_t n = rx.read(tmp, want);
        if (n == 0) {
            break;
        }
        rxCount = rxCount + static_cast<uint32_t>(n);
        size_t sent = 0;
        while (sent < n) {
            size_t w = tx.write(tmp + sent, n - sent);
            if (w == 0) {
                delay(0);
                w = tx.write(tmp + sent, n - sent);
                if (w == 0) {
                    drops = drops + static_cast<uint32_t>(n - sent);
                    break;
                }
            }
            sent += w;
        }
        cutCount = cutCount + static_cast<uint32_t>(sent);
        if (packetFinder != nullptr) {
            activeFramer = (direction == DEVICE1_TO_DEVICE2) ? &framerState1 : &framerState2;
            activeMaxPacketSize = (direction == DEVICE1_TO_DEVICE2) ? buffer1MaxSize : buffer2MaxSize;
            packetFinder(buffer.data(), &bufferPosition, tmp, &n, [&]() {
                if (packetHandler != nullptr) {
                    size_t length = bufferPosition;
                    packetHandler(buffer.data(), length, direction);
                }
            });
        }
        avail = rx.available();
    }
}

void UARTInterceptor::drainInject() {
}

size_t UARTInterceptor::popHwRing(uint8_t* ring, volatile uint16_t& r, volatile uint16_t w, size_t cap, uint8_t* out, size_t outCap) {
    size_t n = 0;
    uint16_t ri = r;
    uint16_t mask = static_cast<uint16_t>(cap - 1u);
    while (n < outCap) {
        if (ri == w) {
            break;
        }
        out[n++] = ring[ri];
        ri = static_cast<uint16_t>((ri + 1u) & mask);
    }
    r = ri;
    return n;
}

void UARTInterceptor::pumpHold(uint8_t* ring, volatile uint16_t& r, volatile uint16_t& w, size_t cap, Pend& destPend) {
    size_t space = pendSpace(destPend);
    if (space == 0) {
        return;
    }
    uint8_t buf[256];
    size_t take = space < sizeof(buf) ? space : sizeof(buf);
    size_t n = popHwRing(ring, r, w, cap, buf, take);
    if (n > 0) {
        enqueuePend(destPend, buf, n);
    }
}

void UARTInterceptor::pumpParse(uint8_t* ring, volatile uint16_t& r, volatile uint16_t& w, size_t cap,
    std::vector<uint8_t>& buffer, size_t& bufferPosition, Direction direction) {
    uint8_t newBytes[256];
    size_t newBytesLength = popHwRing(ring, r, w, cap, newBytes, sizeof(newBytes));
    if (newBytesLength == 0 || packetFinder == nullptr) {
        return;
    }
    activeFramer = (direction == DEVICE1_TO_DEVICE2) ? &framerState1 : &framerState2;
    activeMaxPacketSize = (direction == DEVICE1_TO_DEVICE2) ? buffer1MaxSize : buffer2MaxSize;
    packetFinder(buffer.data(), &bufferPosition, newBytes, &newBytesLength, [&]() {
        if (packetHandler != nullptr) {
            size_t length = bufferPosition;
            packetHandler(buffer.data(), length, direction);
        }
    });
}

void UARTInterceptor::armRx() {
    uart_dev_t* u1 = UART_LL_GET_HW(UART_NUM_1);
    uart_dev_t* u2 = UART_LL_GET_HW(UART_NUM_2);
    uart_ll_ena_intr_mask(u1, kRxIntrMask);
    uart_ll_ena_intr_mask(u2, kRxIntrMask);
}

void UARTInterceptor::drainHwFromTask() {
    // Never disable the UART ISRs. Task poll is a backup under the same
    // mux the ISR uses so RX/hold/TX stay ordered.
    pollHwFromTask();
}

void UARTInterceptor::stampAgesFromTask() {
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    uint32_t r1 = rxCount1;
    uint32_t r2 = rxCount2;
    uint32_t c1 = cutCount1;
    uint32_t c2 = cutCount2;
    if (r1 != seenRx1) {
        if (lastRx1Ms != 0) {
            uint32_t gap = now - lastRx1Ms;
            if (gap > maxGap1) {
                maxGap1 = gap;
            }
        }
        seenRx1 = r1;
        lastRx1Ms = now;
    }
    if (r2 != seenRx2) {
        if (lastRx2Ms != 0) {
            uint32_t gap = now - lastRx2Ms;
            if (gap > maxGap2) {
                maxGap2 = gap;
            }
        }
        seenRx2 = r2;
        lastRx2Ms = now;
    }
    if (c1 != seenCut1) {
        seenCut1 = c1;
        lastTx2Ms = now;
    }
    if (c2 != seenCut2) {
        seenCut2 = c2;
        lastTx1Ms = now;
    }
    updateResyncPause(now);
}

void UARTInterceptor::updateResyncPause(uint32_t now) {
    (void)now;
    pauseTx1 = false;
}

void UARTInterceptor::flushToBase() {
    if (unplugActive) {
        return;
    }
    flushPend(UART_NUM_1, pendTo1, lastTx1Ms);
}

void UARTInterceptor::wireUnplug(uint32_t ms) {
    if (pinTx1 < 0 || pinRx1 < 0) {
        return;
    }
    if (pumpLock != nullptr && xSemaphoreTake(pumpLock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    // Detach both base-side pins: the base's RX floats, exactly like the
    // physical unplug the user demonstrated clears a wedged base.
    // NOTE: uart_set_pin with UART_PIN_NO_CHANGE keeps pins ATTACHED — it
    // never floats the wire (the first implementation was a silent no-op,
    // which is why three software unplug rounds achieved nothing while the
    // user's physical unplug worked). gpio_reset_pin returns the pad to
    // plain GPIO input — the UART peripheral keeps clocking into the void.
    // unplugTick re-matrixes the UART signals back via uart_set_pin.
    gpio_reset_pin(static_cast<gpio_num_t>(pinTx1));
    gpio_reset_pin(static_cast<gpio_num_t>(pinRx1));
    uart_ll_txfifo_rst(UART_LL_GET_HW(UART_NUM_1));
    uart_ll_rxfifo_rst(UART_LL_GET_HW(UART_NUM_1));
    pendTo1.head = 0;
    pendTo1.used = 0;
    buffer1Position = 0;
    framerState1 = FramerState();
    holdW1 = 0;
    holdR1 = 0;
    unplugUntilMs = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL) + ms;
    unplugActive = true;
    if (pumpLock != nullptr) {
        xSemaphoreGive(pumpLock);
    }
}

void UARTInterceptor::unplugTick() {
    if (!unplugActive) {
        return;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if ((int32_t)(now - unplugUntilMs) < 0) {
        return;
    }
    uart_set_pin(UART_NUM_1, pinTx1, pinRx1, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_ll_txfifo_rst(UART_LL_GET_HW(UART_NUM_1));
    uart_ll_rxfifo_rst(UART_LL_GET_HW(UART_NUM_1));
    pendTo1.head = 0;
    pendTo1.used = 0;
    buffer1Position = 0;
    framerState1 = FramerState();
    holdW1 = 0;
    holdR1 = 0;
    unplugActive = false;
    unplugCount = unplugCount + 1;
}

void UARTInterceptor::tick() {
    if (pumpLock != nullptr && xSemaphoreTake(pumpLock, 0) != pdTRUE) {
        return;
    }
    unplugTick();
    processPort(Serial1, Serial2, buffer1, buffer1Position, DEVICE1_TO_DEVICE2, rxCount1, cutCount1, dropCount1);
    processPort(Serial2, Serial1, buffer2, buffer2Position, DEVICE2_TO_DEVICE1, rxCount2, cutCount2, dropCount2);
    if (pumpLock != nullptr) {
        xSemaphoreGive(pumpLock);
    }
    stampAgesFromTask();
}

uint32_t UARTInterceptor::rxAgeMs(Direction fromDevice) const {
    uint32_t stamped = fromDevice == DEVICE1_TO_DEVICE2 ? lastRx1Ms : lastRx2Ms;
    if (stamped == 0) {
        uint32_t cnt = fromDevice == DEVICE1_TO_DEVICE2 ? rxCount1 : rxCount2;
        return cnt == 0 ? 0xFFFFFFFFu : 0;
    }
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    return now - stamped;
}

uint32_t UARTInterceptor::txAgeMs(Direction toDevice) const {
    uint32_t stamped = toDevice == DEVICE1_TO_DEVICE2 ? lastTx2Ms : lastTx1Ms;
    if (stamped == 0) {
        return 0xFFFFFFFFu;
    }
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    return now - stamped;
}

bool UARTInterceptor::bothSidesLive(uint32_t staleMs) const {
    if (lastRx1Ms == 0 || lastRx2Ms == 0) {
        return false;
    }
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    return (now - lastRx1Ms) < staleMs && (now - lastRx2Ms) < staleMs;
}

uint32_t UARTInterceptor::hwRxFifo(uart_port_t port) const {
    return uart_ll_get_rxfifo_len(UART_LL_GET_HW(port));
}

uint32_t UARTInterceptor::hwIntSt(uart_port_t port) const {
    return REG_READ(UART_INT_ST_REG(port));
}

uint32_t UARTInterceptor::hwIntRaw(uart_port_t port) const {
    return REG_READ(UART_INT_RAW_REG(port));
}

bool UARTInterceptor::workPending() {
    return Serial1.available() > 0 || Serial2.available() > 0;
}

bool UARTInterceptor::wirePending() {
    return Serial1.available() > 0 || Serial2.available() > 0;
}
