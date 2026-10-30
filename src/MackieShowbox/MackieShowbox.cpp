#define SHOWBOX_DEBUG

#include "MackieShowbox.h"
#include "DebugLog.h"
#include "System/Diag.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef SHOWBOX_DEBUG
#include "string_mappings.h"
#endif

namespace {
constexpr float kAnalogHoldNorm = 0.009f;
constexpr float kAnalogFloor = -120.0f;
constexpr float kAnalogCeil = 10.0f;
constexpr float kAnalogSpan = 130.0f;
constexpr float kAnalogStep = 0.5f;
// Hardware pots stop short of the electrical ends (~−118 / +9.5). Grab those
// as −∞ / +10 so the UI does not flicker −118↔−∞ or sit at 195% / +9.5 dB.
constexpr float kAnalogFloorGrab = -115.0f;
constexpr float kAnalogCeilGrab = 9.25f;
constexpr float kAnalogFloorLeave = -114.0f;
constexpr float kAnalogCeilLeave = 8.75f;
constexpr uint32_t kAnalogNotifyMs = 40;

bool analogEntity(entity_id id) {
    return id == MAIN_HEADPHONE_GAIN || id == LOOPER_LEVEL;
}

float snapAnalog(float value) {
    if (!std::isfinite(value)) {
        return kAnalogFloor;
    }
    if (value <= kAnalogFloorGrab) {
        return kAnalogFloor;
    }
    if (value >= kAnalogCeilGrab) {
        return kAnalogCeil;
    }
    return roundf(value / kAnalogStep) * kAnalogStep;
}

float snapAnalogHeld(float value, float previous, bool heard) {
    float snapped = snapAnalog(value);
    if (!heard) {
        return snapped;
    }
    if (previous <= kAnalogFloor + 0.01f && value < kAnalogFloorLeave) {
        return kAnalogFloor;
    }
    if (previous >= kAnalogCeil - 0.01f && value > kAnalogCeilLeave) {
        return kAnalogCeil;
    }
    return snapped;
}

const float kMainMasterGain[] = {
    Showbox::MainMasterGain::POSITION_1, Showbox::MainMasterGain::POSITION_2, Showbox::MainMasterGain::POSITION_3,
    Showbox::MainMasterGain::POSITION_4, Showbox::MainMasterGain::POSITION_5, Showbox::MainMasterGain::POSITION_6,
    Showbox::MainMasterGain::POSITION_7, Showbox::MainMasterGain::POSITION_8, Showbox::MainMasterGain::POSITION_9,
    Showbox::MainMasterGain::POSITION_10, Showbox::MainMasterGain::POSITION_11, Showbox::MainMasterGain::POSITION_12,
    Showbox::MainMasterGain::POSITION_13, Showbox::MainMasterGain::POSITION_14, Showbox::MainMasterGain::POSITION_15,
    Showbox::MainMasterGain::POSITION_16
};
const float kInputGain[] = {
    Showbox::InputGain::POSITION_1, Showbox::InputGain::POSITION_2, Showbox::InputGain::POSITION_3,
    Showbox::InputGain::POSITION_4, Showbox::InputGain::POSITION_5, Showbox::InputGain::POSITION_6,
    Showbox::InputGain::POSITION_7, Showbox::InputGain::POSITION_8, Showbox::InputGain::POSITION_9,
    Showbox::InputGain::POSITION_10, Showbox::InputGain::POSITION_11, Showbox::InputGain::POSITION_12,
    Showbox::InputGain::POSITION_13, Showbox::InputGain::POSITION_14, Showbox::InputGain::POSITION_15,
    Showbox::InputGain::POSITION_16
};
const float kInputVolume[] = {
    Showbox::InputVolume::POSITION_1, Showbox::InputVolume::POSITION_2, Showbox::InputVolume::POSITION_3,
    Showbox::InputVolume::POSITION_4, Showbox::InputVolume::POSITION_5, Showbox::InputVolume::POSITION_6,
    Showbox::InputVolume::POSITION_7, Showbox::InputVolume::POSITION_8, Showbox::InputVolume::POSITION_9,
    Showbox::InputVolume::POSITION_10, Showbox::InputVolume::POSITION_11, Showbox::InputVolume::POSITION_12,
    Showbox::InputVolume::POSITION_13, Showbox::InputVolume::POSITION_14, Showbox::InputVolume::POSITION_15,
    Showbox::InputVolume::POSITION_16
};
const float kUnitAmount[] = {
    Showbox::InputEffectAmount::POSITION_1, Showbox::InputEffectAmount::POSITION_2, Showbox::InputEffectAmount::POSITION_3,
    Showbox::InputEffectAmount::POSITION_4, Showbox::InputEffectAmount::POSITION_5, Showbox::InputEffectAmount::POSITION_6,
    Showbox::InputEffectAmount::POSITION_7, Showbox::InputEffectAmount::POSITION_8, Showbox::InputEffectAmount::POSITION_9,
    Showbox::InputEffectAmount::POSITION_10, Showbox::InputEffectAmount::POSITION_11, Showbox::InputEffectAmount::POSITION_12,
    Showbox::InputEffectAmount::POSITION_13, Showbox::InputEffectAmount::POSITION_14, Showbox::InputEffectAmount::POSITION_15,
    Showbox::InputEffectAmount::POSITION_16
};
const float kEqGain[] = {
    Showbox::InputEqGain::POSITION_1, Showbox::InputEqGain::POSITION_2, Showbox::InputEqGain::POSITION_3,
    Showbox::InputEqGain::POSITION_4, Showbox::InputEqGain::POSITION_5, Showbox::InputEqGain::POSITION_6,
    Showbox::InputEqGain::POSITION_7, Showbox::InputEqGain::POSITION_8, Showbox::InputEqGain::POSITION_9,
    Showbox::InputEqGain::POSITION_10, Showbox::InputEqGain::POSITION_11, Showbox::InputEqGain::POSITION_12,
    Showbox::InputEqGain::POSITION_13, Showbox::InputEqGain::POSITION_14, Showbox::InputEqGain::POSITION_15
};
const float kExtSends[] = {
    Showbox::InputExtFxSends::POSITION_0, Showbox::InputExtFxSends::POSITION_1, Showbox::InputExtFxSends::POSITION_2,
    Showbox::InputExtFxSends::POSITION_3, Showbox::InputExtFxSends::POSITION_4, Showbox::InputExtFxSends::POSITION_5,
    Showbox::InputExtFxSends::POSITION_6, Showbox::InputExtFxSends::POSITION_7, Showbox::InputExtFxSends::POSITION_8,
    Showbox::InputExtFxSends::POSITION_9, Showbox::InputExtFxSends::POSITION_10, Showbox::InputExtFxSends::POSITION_11,
    Showbox::InputExtFxSends::POSITION_12, Showbox::InputExtFxSends::POSITION_13, Showbox::InputExtFxSends::POSITION_14,
    Showbox::InputExtFxSends::POSITION_15
};

float nearestStep(float value, const float* steps, size_t count) {
    float best = steps[0];
    float bestD = fabsf(value - steps[0]);
    for (size_t i = 1; i < count; i++) {
        float d = fabsf(value - steps[i]);
        if (d < bestD) {
            bestD = d;
            best = steps[i];
        }
    }
    return best;
}
}

MackieShowbox::MackieShowbox(uint8_t baseRx, uint8_t baseTx, uint8_t mixerRx, uint8_t mixerTx)
    : interceptor() {
    this->baseRx = baseRx;
    this->baseTx = baseTx;
    this->mixerRx = mixerRx;
    this->mixerTx = mixerTx;

    // Initialize entityValues with default values based on entity_type_mapping
    for (const auto& pair : entity_type_mapping) {
        entity_id entityId = pair.first;
        entityValues[entityId] = EntityValue();
    }
}

void MackieShowbox::begin() {
    // The parse queue is ~18 KB of internal DRAM as a static array — on the
    // S3 that is the difference between "heap plateau at 4 KB, every task
    // allocation fails" and a healthy floor. It is only touched in task
    // context, so PSRAM is fine. Fall back to DRAM if PSRAM is absent.
    parseQ = static_cast<ParseItem*>(
        heap_caps_calloc(kParseQ, sizeof(ParseItem), MALLOC_CAP_SPIRAM));
    if (parseQ == nullptr) {
        parseQ = static_cast<ParseItem*>(calloc(kParseQ, sizeof(ParseItem)));
    }
    if (parseQ == nullptr) {
        debugLog("uart parse queue alloc failed");
    }
    loadAnalogHold();
    uint8_t startSig[] = {0xBE, 0xEF};
    uint8_t endSig[] = {0xEF, 0xBE};
    interceptor.setPacketFinderStartEndSig(startSig, sizeof(startSig), endSig, sizeof(endSig));

    interceptor.setPacketHandler([this](uint8_t* packet, size_t& length, UARTInterceptor::Direction direction) {
        // Keep this path tiny. BLE MIDI and debug prints used to run here and
        // stall the pump until the mixer/base dropped the link.
        lastPacketMs = millis();
        const bool fromMixer = direction == TO_BASE;
        const bool framed = length >= 6
            && packet[0] == 0xBE && packet[1] == 0xEF
            && packet[length - 2] == 0xEF && packet[length - 1] == 0xBE;
        if (framed) {
            if (fromMixer) {
                goodMix = goodMix + 1;
            } else {
                goodBase = goodBase + 1;
            }
        } else {
            if (fromMixer) {
                badMix = badMix + 1;
            } else {
                badBase = badBase + 1;
            }
        }
        if (length >= 4) {
            packet_type t = static_cast<packet_type>(packet[3]);
            if (t == HEARTBEAT) {
                lastHeartbeatMs = lastPacketMs;
                if (fromMixer) {
                    hbMix = hbMix + 1;
                } else {
                    hbBase = hbBase + 1;
                }
            } else if (t == ALL_ENTITIES) {
                if (fromMixer) {
                    initMix = initMix + 1;
                } else {
                    initBase = initBase + 1;
                    ignoreTouchUntil = millis() + 400;
                }
            } else if (t == ENTITY && fromMixer) {
                entMix = entMix + 1;
                const bool analog = length >= 6 && analogEntity(static_cast<entity_id>(packet[5]));
                rememberMixerEntity(packet, length);
                if (analog) {
                    // Analog pots stream unique floats. Storing here keeps analog-hold
                    // restore correct; skipping parseQ stops the main-loop hex/WS flood
                    // that wedged the chip when the phones/looper knobs moved.
                    analogSkip = analogSkip + 1;
                    return UARTInterceptor::PACKET_NOT_MODIFIED;
                }
            } else if (t == DATA_REQUEST && fromMixer) {
                reqMix = reqMix + 1;
                debugLog("uart DATA_REQUEST from mixer len=%u %02X %02X %02X %02X %02X %02X",
                    (unsigned)length,
                    length > 0 ? packet[0] : 0, length > 1 ? packet[1] : 0,
                    length > 2 ? packet[2] : 0, length > 3 ? packet[3] : 0,
                    length > 4 ? packet[4] : 0, length > 5 ? packet[5] : 0);
            }
        }
        enqueueParse(packet, length, direction);
        return UARTInterceptor::PACKET_NOT_MODIFIED;
    });

    // 255 payload + 7 header/trailer. 256 used to drop the longest dumps.
    interceptor.begin(baseRx, baseTx, mixerRx, mixerTx, 115200, 115200, 288, 288);
    uartStarted = true;
    debugLog("uart begin heap=%u largest=%u pend=%u ring=%u hold=%u fifo1=%u fifo2=%u int1=%08x int2=%08x",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)interceptor.pendingCap(),
        (unsigned)interceptor.ringCap(),
        (unsigned)interceptor.holdCap(),
        (unsigned)interceptor.hwRxFifo(UART_NUM_1),
        (unsigned)interceptor.hwRxFifo(UART_NUM_2),
        (unsigned)interceptor.hwIntSt(UART_NUM_1),
        (unsigned)interceptor.hwIntSt(UART_NUM_2));
    if (xTaskCreatePinnedToCore(MackieShowbox::uartTask, "showbox-uart", 8192, this, 20, &uartTaskHandle, 1) != pdPASS) {
        uartTaskRunning = false;
        uartTaskHandle = nullptr;
        debugLog("uartTask create FAILED heap=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (Debug != nullptr) {
            Debug->println("Showbox UART task failed; pass-through stays on the main loop");
        }
    } else {
        uartTaskRunning = true;
        debugLog("uartTask ok handle=%p", (void*)uartTaskHandle);
        if (Debug != nullptr) {
            Debug->println("Showbox UART pass-through is on core 1");
        }
    }
}

void MackieShowbox::queueEmit(entity_id entityId, const uint8_t* packet, size_t length) {
    if (packet == nullptr || entityId > FX_BYPASS || length < 12 || length > 16) {
        return;
    }
    memcpy(emitPkt[entityId], packet, length);
    emitLen[entityId] = static_cast<uint8_t>(length);
    emitPending[entityId] = 1;
}

void MackieShowbox::flushEmits() {
    uint32_t now = millis();
    int sent = 0;
    for (uint8_t i = 0; i <= FX_BYPASS && sent < 2; i++) {
        if (emitPending[i] == 0) {
            continue;
        }
        const uint8_t n = emitLen[i];
        if (n < 12 || n > 16) {
            emitPending[i] = 0;
            continue;
        }
        const bool slow = n >= 15;
        if (slow && lastEmitMs[i] != 0 && (int32_t)(now - lastEmitMs[i]) < 40) {
            continue;
        }
        uint8_t packet[16];
        memcpy(packet, emitPkt[i], n);
        emitPending[i] = 0;
        lastEmitMs[i] = now;
        interceptor.sendPacket(packet, n, TO_BASE);
        interceptor.sendPacket(packet, n, TO_MIXER);
        sent++;
    }
}

void MackieShowbox::setEntityValue(entity_id entityId, bool value, bool emit) {
    entityValues[entityId].boolValue = value;
    entityValues[entityId].heard = true;
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        queueEmit(entityId, packet, sizeof(packet));
    }
}
void MackieShowbox::setEntityValue(entity_id entityId, uint8_t value, bool emit) {
    entityValues[entityId].uint8Value = value;
    entityValues[entityId].heard = true;
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        queueEmit(entityId, packet, sizeof(packet));
    }
}
void MackieShowbox::setEntityValue(entity_id entityId, float value, bool emit) {
    value = snapFloat(entityId, value);
    entityValues[entityId].floatValue = value;
    entityValues[entityId].heard = true;
    if (emit) {
        if (entityId == MAIN_HEADPHONE_GAIN || entityId == LOOPER_LEVEL) {
            mixerTouched[entityId] = true;
            AnalogRest& rest = entityId == MAIN_HEADPHONE_GAIN ? phonesRest : loopRest;
            rest.have = false;
            rest.dir = 0;
        }
        uint8_t floatBytes[4];
        memcpy(floatBytes, &value, sizeof(float));
        uint8_t packet[15] = { 0xBE, 0xEF, 0x08, 0x03, 0x00, entityId, 0x00, 0x00, 0x00,
            floatBytes[0], floatBytes[1], floatBytes[2], floatBytes[3], 0xEF, 0xBE };
        queueEmit(entityId, packet, sizeof(packet));
    }
}
bool MackieShowbox::getBoolEntityValue(entity_id entityId) {
    return entityValues[entityId].boolValue;
}
uint8_t MackieShowbox::getUint8EntityValue(entity_id entityId) {
    return entityValues[entityId].uint8Value;
}
float MackieShowbox::getFloatEntityValue(entity_id entityId) {
    return entityValues[entityId].floatValue;
}

bool MackieShowbox::hasEntityValue(entity_id entityId) const {
    auto it = entityValues.find(entityId);
    return it != entityValues.end() && it->second.heard;
}

void MackieShowbox::requestMixerDump() {
    if (!uartStarted) {
        return;
    }
    if (dumpGen == 0) {
        // AUTOGAIN/SD use BE EF 00 <type> <arg> EF BE. The previous dump
        // request used a different layout and only went to the mixer, so
        // ALL_ENTITIES never arrived after the ESP32 booted mid-session.
        uint8_t req[8];
        size_t n = 0;
        const uint8_t phase = static_cast<uint8_t>(dumpTries % 3);
        if (phase == 1) {
            req[0] = 0xBE; req[1] = 0xEF; req[2] = 0x00;
            req[3] = DATA_REQUEST; req[4] = ALL_ENTITIES;
            req[5] = 0xEF; req[6] = 0xBE;
            n = 7;
        } else if (phase == 2) {
            req[0] = 0xBE; req[1] = 0xEF; req[2] = 0x00;
            req[3] = DATA_REQUEST; req[4] = 0x00;
            req[5] = 0xEF; req[6] = 0xBE;
            n = 7;
        } else {
            req[0] = 0xBE; req[1] = 0xEF; req[2] = 0x01;
            req[3] = DATA_REQUEST; req[4] = 0x00; req[5] = ALL_ENTITIES;
            req[6] = 0xEF; req[7] = 0xBE;
            n = 8;
        }
        interceptor.sendPacket(req, n, TO_MIXER);
    }
    if (!batteryHeard) {
        uint8_t batt[] = { 0xBE, 0xEF, 0x00, BATTERY_LEVEL, 0x00, 0xEF, 0xBE };
        interceptor.sendPacket(batt, sizeof(batt), TO_MIXER);
        interceptor.sendPacket(batt, sizeof(batt), TO_BASE);
    }
}

void MackieShowbox::forceEntityResync() {
    // Re-request the full dump on demand: a missed announcement window can
    // leave the cache desynced (the base announces state changes once), and
    // the boot-time dump request only ever ran while dumpGen was 0.
    if (!uartStarted) {
        return;
    }
    dumpGen = 0;
    dumpTries = 0;
    lastDumpReqMs = 0;
    dumpLinkLost = true;
}

void MackieShowbox::sendLooperButtonAction(looper_button_action action) {
    // The mixer's own gestures (from the diag capture):
    //   stop  = DOWN, UP, DOUBLE_PRESS (a full press, then the action 24 ms later)
    //   clear = DOWN, LONG_PRESS, UP   (press context for the long press)
    // A bare action packet without press context is intermittently ignored
    // by the base's looper handler.
    uint8_t buf[32];
    size_t n = 0;
    const uint8_t down[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, static_cast<uint8_t>(looper_button_action::DOWN), 0xEF, 0xBE };
    const uint8_t up[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, static_cast<uint8_t>(looper_button_action::UP), 0xEF, 0xBE };
    const uint8_t act[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, static_cast<uint8_t>(action), 0xEF, 0xBE };
    if (action == looper_button_action::DOUBLE_PRESS) {
        memcpy(buf + n, down, 8); n += 8;
        memcpy(buf + n, up, 8); n += 8;
        memcpy(buf + n, act, 8); n += 8;
    } else if (action == looper_button_action::LONG_PRESS) {
        memcpy(buf + n, down, 8); n += 8;
        memcpy(buf + n, act, 8); n += 8;
        memcpy(buf + n, up, 8); n += 8;
    } else {
        memcpy(buf + n, act, 8); n += 8;
    }
    interceptor.sendPacket(buf, n, TO_BASE);
}

void MackieShowbox::tapLooper(uint8_t times) {
    if (times < 1) {
        times = 1;
    }
    if (times > 2) {
        times = 2;
    }
    // Original form: DOWN/UP pairs back-to-back in one burst. This is what
    // the firmware sent for weeks without issue - record/play/overdub must
    // never be delayed.
    uint8_t buf[32];
    size_t n = 0;
    for (uint8_t i = 0; i < times; i++) {
        uint8_t down[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, static_cast<uint8_t>(looper_button_action::DOWN), 0xEF, 0xBE };
        uint8_t up[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, static_cast<uint8_t>(looper_button_action::UP), 0xEF, 0xBE };
        memcpy(buf + n, down, 8);
        n += 8;
        memcpy(buf + n, up, 8);
        n += 8;
    }
    interceptor.sendPacket(buf, n, TO_BASE);
}

void MackieShowbox::noteLooperOutgoing() {
    looperOutgoing = true;
}

bool MackieShowbox::takeLooperOutgoing() {
    bool sent = looperOutgoing;
    looperOutgoing = false;
    return sent;
}

void MackieShowbox::injectPacket(const uint8_t* data, size_t length, const char* dir) {
    if (data == nullptr || length < 2 || length > 280) {
        return;
    }
    uint8_t packet[280];
    memcpy(packet, data, length);
    bool toBase = dir == nullptr || dir[0] == '\0' || strcmp(dir, "both") == 0 || strcmp(dir, "base") == 0;
    bool toMixer = dir == nullptr || dir[0] == '\0' || strcmp(dir, "both") == 0 || strcmp(dir, "mixer") == 0;
    if (toBase) {
        interceptor.sendPacket(packet, length, TO_BASE);
    }
    if (toMixer) {
        interceptor.sendPacket(packet, length, TO_MIXER);
    }
}

/**
    void sendLooperButtonAction(looper_button_action action);
    void toggleSdCardRecord();
    void snapshotAction(snapshot_action action, snapshot_slot slot);
    void tunerAction(tuner_action action, tuner_chan chan);
    float getBatteryLevel();
    sd_card_state getSdCardState();
 */

void MackieShowbox::toggleSdCardRecord() {
    // The SD slot lives in the base unit. A copy to the mixer does nothing,
    // and a copy to the base is what actually flips its record state.
    uint8_t packet[] = { 0xBE, 0xEF, 0x00, SD_CARD_EVENT, sd_card_action::TOGGLE_RECORD, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
    // The base never confirms instantly; flip optimistically and let its
    // 2/3 announcements correct us.
    if (sdCardHeard && sdCardState != sd_card_state::NOT_DETECTED) {
        sdCardState = sdCardState == sd_card_state::RECORDING
            ? sd_card_state::READY
            : sd_card_state::RECORDING;
    }
}

void MackieShowbox::sendAutogain(uint8_t chan) {
    if (chan > 3) {
        chan = 0;
    }
    setSelectedChan(chan);
    uint8_t packet[] = { 0xBE, 0xEF, 0x00, AUTOGAIN, chan, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
    interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
}

void MackieShowbox::snapshotAction(snapshot_action action, snapshot_slot slot) {
    if (snapshotBusy()) {
        // The mixer needs seconds per snapshot op; saving again mid-write
        // loses the take. Queue the latest request, send it when idle.
        snapshotQueuedAction = static_cast<uint8_t>(action);
        snapshotQueuedSlot = static_cast<uint8_t>(slot);
        snapshotQueued = true;
        return;
    }
    sendSnapshot(action, slot);
}

void MackieShowbox::sendSnapshot(snapshot_action action, snapshot_slot slot) {
    uint8_t packet[] = { 0xBE, 0xEF, 0x01, SNAPSHOT, static_cast<uint8_t>(action), static_cast<uint8_t>(slot), 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
    armSnapshotDone(static_cast<uint8_t>(action), static_cast<uint8_t>(slot));
    // Measured on the wire: a save followed by another command within ~2s
    // captured the wrong state. Saves take longer than recalls.
    snapshotBusyUntil = millis()
        + (action == snapshot_action::SAVE ? 5000u : 3500u);
}

bool MackieShowbox::snapshotBusy() const {
    return (int32_t)(millis() - snapshotBusyUntil) < 0;
}

void MackieShowbox::snapshotMenuBack() {
    uint8_t packet[] = { 0xBE, 0xEF, 0x04, SNAPSHOT_MENU, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

void MackieShowbox::armSnapshotDone(uint8_t action, uint8_t slot) {
    snapshotPendingAction = action;
    snapshotPendingSlot = slot;
}

bool MackieShowbox::takeSnapshotDone(uint8_t& action, uint8_t& slot) {
    if (snapshotQueued && !snapshotBusy()) {
        snapshotQueued = false;
        sendSnapshot(static_cast<snapshot_action>(snapshotQueuedAction),
            static_cast<snapshot_slot>(snapshotQueuedSlot));
        return false;
    }
    if (snapshotBusyUntil == 0 || snapshotBusy()) {
        return false;
    }
    action = snapshotPendingAction;
    slot = snapshotPendingSlot;
    snapshotBusyUntil = 0;
    // The mixer restores its state on recall but does NOT re-send entity
    // values (verified 2026-10-19: gain stayed stale until a DATA_REQUEST
    // forced the dump). Two problems:
    // 1. The module's cache needs a fresh dump — re-request it.
    // 2. The mixerTouched flags (set when the user wrote entities via the
    //    webui) suppress the module from accepting the dump's values —
    //    every entity the user ever touched via the webui is gated off.
    //    Clear them ALL so the dump updates everything.
    for (uint8_t i = 0; i <= FX_BYPASS; i++) {
        mixerTouched[i] = false;
    }
    lastDumpReqMs = 0;
    dumpTries = 0;
    uint8_t req[] = { 0xBE, 0xEF, 0x00, DATA_REQUEST, ALL_ENTITIES, 0xEF, 0xBE };
    interceptor.sendPacket(req, sizeof(req), TO_MIXER);
    return true;
}

void MackieShowbox::sendTunerPacket(uint8_t action, uint8_t chan) {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x01, TUNER_TOGGLE, action, chan, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

void MackieShowbox::tunerAction(tuner_action action, tuner_chan chan) {
    uint8_t target = (chan <= tuner_chan::FOUR) ? static_cast<uint8_t>(chan) : tunerChannel;
    if (action == tuner_action::TURN_ON) {
        if (tunerOn && tunerChannel != target) {
            sendTunerPacket(tuner_action::TURN_OFF, tunerChannel);
        }
        tunerOn = true;
        tunerChannel = target;
        tunerHeard = false;
        lastTunerPitchMs = 0;
        tunerMidi = 0;
        tunerNoteId = 0;
        tunerCents = 0.0f;
        tunerHz = 0.0f;
        ignoreTunerOffUntil = millis() + 300;
        sendTunerPacket(tuner_action::TURN_ON, target);
        tunerRetryAt = millis() + 80;
        tunerRetryChan = target;
        return;
    }
    uint8_t offChan = tunerOn ? tunerChannel : target;
    tunerOn = false;
    tunerHeard = false;
    lastTunerPitchMs = 0;
    tunerMidi = 0;
    tunerNoteId = 0;
    tunerCents = 0.0f;
    tunerHz = 0.0f;
    sendTunerPacket(tuner_action::TURN_OFF, offChan);
    if (target != offChan) {
        sendTunerPacket(tuner_action::TURN_OFF, target);
    }
}

float MackieShowbox::getBatteryLevel() {
    return batteryLevel;
}

sd_card_state MackieShowbox::getSdCardState() {
    return sdCardState;
}

uint8_t MackieShowbox::getTunerState() {
    return tunerOn ? 1 : 0;
}

uint8_t MackieShowbox::getTunerChan() {
    return tunerChannel;
}

bool MackieShowbox::getTunerHeard() const {
    if (!tunerOn || !tunerHeard || lastTunerPitchMs == 0) {
        return false;
    }
    return (millis() - lastTunerPitchMs) < 2500;
}

uint8_t MackieShowbox::getTunerMidi() const {
    return tunerMidi;
}

uint8_t MackieShowbox::getTunerNoteId() const {
    return tunerNoteId;
}

float MackieShowbox::getTunerCents() const {
    return tunerCents;
}

float MackieShowbox::getTunerHz() const {
    return tunerHz;
}

static uint8_t tunerPitchClassMidi(uint8_t noteId) {
    static const uint8_t kMidi[] = {
        0, 69, 70, 71, 60, 61, 62, 63, 64, 65, 66, 67, 68
    };
    return noteId <= 12 ? kMidi[noteId] : 0;
}

void MackieShowbox::applyTunerFeedback(uint8_t chan, uint8_t noteId, float cents) {
    if (!tunerOn) {
        return;
    }
    if (chan <= 3 && chan != tunerChannel) {
        return;
    }
    if (noteId == 0 || noteId > 12) {
        tunerHeard = false;
        tunerNoteId = 0;
        tunerMidi = 0;
        tunerCents = 0.0f;
        tunerHz = 0.0f;
        lastTunerPitchMs = 0;
        return;
    }
    if (!std::isfinite(cents)) {
        return;
    }
    if (cents > 50.0f) {
        cents = 50.0f;
    }
    if (cents < -50.0f) {
        cents = -50.0f;
    }
    tunerNoteId = noteId;
    tunerMidi = tunerPitchClassMidi(noteId);
    tunerCents = cents;
    tunerHz = 0.0f;
    tunerHeard = true;
    lastTunerPitchMs = millis();
}

void MackieShowbox::parseTunerFeedback(const uint8_t* packet, size_t length) {
    if (packet == nullptr || length < 13) {
        return;
    }
    uint8_t chan = packet[5];
    uint8_t noteId = packet[6];
    float cents = 0.0f;
    memcpy(&cents, &packet[9], sizeof(float));
    applyTunerFeedback(chan, noteId, cents);
}

uint32_t MackieShowbox::linkAgeMs() const {
    uint32_t baseAge = interceptor.rxAgeMs(TO_MIXER);
    uint32_t mixerAge = interceptor.rxAgeMs(TO_BASE);
    if (baseAge == 0xFFFFFFFFu) {
        return mixerAge;
    }
    if (mixerAge == 0xFFFFFFFFu) {
        return baseAge;
    }
    return baseAge > mixerAge ? baseAge : mixerAge;
}

uint32_t MackieShowbox::heartbeatAgeMs() const {
    if (lastHeartbeatMs == 0) {
        return 0xFFFFFFFFu;
    }
    return millis() - lastHeartbeatMs;
}

uint32_t MackieShowbox::baseAgeMs() const {
    return interceptor.rxAgeMs(TO_MIXER);
}

uint32_t MackieShowbox::mixerAgeMs() const {
    return interceptor.rxAgeMs(TO_BASE);
}

uint32_t MackieShowbox::baseTxAgeMs() const {
    return interceptor.txAgeMs(TO_BASE);
}

uint32_t MackieShowbox::mixerTxAgeMs() const {
    return interceptor.txAgeMs(TO_MIXER);
}

uint32_t MackieShowbox::rxFromBase() const {
    return interceptor.rxFrom1();
}

uint32_t MackieShowbox::rxFromMixer() const {
    return interceptor.rxFrom2();
}

uint32_t MackieShowbox::droppedFromBase() const {
    return interceptor.droppedFrom1();
}

uint32_t MackieShowbox::droppedFromMixer() const {
    return interceptor.droppedFrom2();
}

uint32_t MackieShowbox::overflowFromBase() const {
    return interceptor.overflowFrom1();
}

uint32_t MackieShowbox::overflowFromMixer() const {
    return interceptor.overflowFrom2();
}

uint32_t MackieShowbox::frameErrFromBase() const {
    return interceptor.frameErrFrom1();
}

uint32_t MackieShowbox::frameErrFromMixer() const {
    return interceptor.frameErrFrom2();
}

uint32_t MackieShowbox::maxGapFromBase() const {
    return interceptor.maxGapFrom1();
}

uint32_t MackieShowbox::maxGapFromMixer() const {
    return interceptor.maxGapFrom2();
}

uint32_t MackieShowbox::hwRxFifoBase() const {
    return interceptor.hwRxFifo(UART_NUM_1);
}

uint32_t MackieShowbox::hwRxFifoMixer() const {
    return interceptor.hwRxFifo(UART_NUM_2);
}

uint32_t MackieShowbox::hwIntStBase() const {
    return interceptor.hwIntSt(UART_NUM_1);
}

uint32_t MackieShowbox::hwIntStMixer() const {
    return interceptor.hwIntSt(UART_NUM_2);
}

bool MackieShowbox::linkOk(uint32_t staleMs) const {
    return interceptor.bothSidesLive(staleMs);
}

bool MackieShowbox::uartHot(uint32_t windowMs) const {
    return interceptor.rxAgeMs(TO_MIXER) < windowMs || interceptor.rxAgeMs(TO_BASE) < windowMs;
}

size_t MackieShowbox::pendingToMixer() const {
    return interceptor.pendingTo2();
}

size_t MackieShowbox::pendingToBase() const {
    return interceptor.pendingTo1();
}

size_t MackieShowbox::ringFromBase() const {
    return interceptor.ringUsed1();
}

size_t MackieShowbox::ringFromMixer() const {
    return interceptor.ringUsed2();
}

size_t MackieShowbox::holdFromBase() const {
    return interceptor.holdUsed1();
}

size_t MackieShowbox::holdFromMixer() const {
    return interceptor.holdUsed2();
}

uint32_t MackieShowbox::cutThroughFromBase() const {
    return interceptor.cutThroughFrom1();
}

uint32_t MackieShowbox::cutThroughFromMixer() const {
    return interceptor.cutThroughFrom2();
}

bool MackieShowbox::txToBasePaused() const {
    return interceptor.txToBasePaused();
}

uint32_t MackieShowbox::txPauseCount() const {
    return interceptor.txPauseCount();
}

void MackieShowbox::waitQuiet(uint32_t maxMs) const {
    uint32_t started = millis();
    while (uartHot() && (int32_t)(millis() - started) < (int32_t)maxMs) {
        vTaskDelay(1);
    }
}

void MackieShowbox::wireUnplug(uint32_t ms) {
    if (!uartStarted) {
        return;
    }
    diagEventForce("uart", "wire unplug %u ms (baseAge=%u hbAge=%u)",
        (unsigned)ms, (unsigned)baseAgeMs(), (unsigned)heartbeatAgeMs());
    debugLog("uart wire unplug %ums baseAge=%u", (unsigned)ms, (unsigned)baseAgeMs());
    interceptor.wireUnplug(ms);
}

bool MackieShowbox::rememberMixerEntity(const uint8_t* packet, size_t length) {
    if (packet == nullptr || length < 10) {
        return false;
    }
    uint8_t id = packet[5];
    if (id > FX_BYPASS) {
        return false;
    }
    if (!mixerOwnsEntity(static_cast<entity_id>(id))) {
        return false;
    }
    if (ignoreTouchUntil != 0 && (int32_t)(millis() - ignoreTouchUntil) < 0) {
        return false;
    }
    auto it = entity_type_mapping.find(static_cast<entity_id>(id));
    if (it == entity_type_mapping.end()) {
        return false;
    }
    if (it->second == BOOL) {
        mixerTouched[id] = true;
        entityValues[static_cast<entity_id>(id)].boolValue = packet[9] != 0;
        entityValues[static_cast<entity_id>(id)].heard = true;
        return true;
    }
    if (it->second == UINT8) {
        mixerTouched[id] = true;
        entityValues[static_cast<entity_id>(id)].uint8Value = packet[9];
        entityValues[static_cast<entity_id>(id)].heard = true;
        return true;
    }
    if (length >= 13) {
        float value;
        memcpy(&value, &packet[9], sizeof(float));
        entity_id entityId = static_cast<entity_id>(id);
        const float snapped = snapFloat(entityId, value);
        if (ignoreAnalogJitter(entityId, snapped)) {
            return false;
        }
        mixerTouched[id] = true;
        const bool same = entityValues[entityId].heard
            && entityValues[entityId].floatValue == snapped;
        entityValues[entityId].floatValue = snapped;
        entityValues[entityId].heard = true;
        if (analogEntity(entityId)) {
            if (same) {
                return false;
            }
            if (entityId == MAIN_HEADPHONE_GAIN) {
                analogDirtyPhones = true;
            } else {
                analogDirtyLoop = true;
            }
        }
        return !same;
    }
    return false;
}

void MackieShowbox::restoreMixerEntities() {
    uint32_t n = 0;
    for (uint8_t i = 0; i <= FX_BYPASS; i++) {
        if (!mixerTouched[i]) {
            continue;
        }
        entity_id id = static_cast<entity_id>(i);
        if (!mixerOwnsEntity(id)) {
            mixerTouched[i] = false;
            continue;
        }
        auto it = entity_type_mapping.find(id);
        if (it == entity_type_mapping.end()) {
            continue;
        }
        if (it->second == BOOL) {
            setEntityValue(id, entityValues[id].boolValue, true);
        } else if (it->second == UINT8) {
            setEntityValue(id, entityValues[id].uint8Value, true);
        } else {
            setEntityValue(id, entityValues[id].floatValue, true);
        }
        n = n + 1;
    }
    if (n > 0) {
        restoreCount = restoreCount + 1;
        debugLog("CRITICAL uart restored %u mixer entities after base ALL_ENTITIES", (unsigned)n);
    }
}

UARTInterceptor::PacketHandlerResult MackieShowbox::handlePacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction) {
    const bool fromMixer = direction == TO_BASE;
    if (raw_packet == nullptr || length < 4) {
        return UARTInterceptor::PACKET_NOT_MODIFIED;
    }
    lastPacketMs = millis();
    packet_type packetType = static_cast<packet_type>(raw_packet[3]);
    if (packetType == HEARTBEAT) {
        lastHeartbeatMs = lastPacketMs;
    }
    if (!parseEnabled) {
        return UARTInterceptor::PACKET_NOT_MODIFIED;
    }

    if (packetType == ENTITY) {
        if (length < 6) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        entity_id entityId = static_cast<entity_id>(raw_packet[5]);
        auto entityTypeIt = entity_type_mapping.find(entityId);
        if (entityTypeIt == entity_type_mapping.end()) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        entity_data_type entityType = entityTypeIt->second;
        size_t valueSize = entityType == FLOAT ? sizeof(float) : 1;
        if (length < 9 + valueSize) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        bool mixerAccepted = false;
        if (fromMixer) {
            mixerAccepted = rememberMixerEntity(raw_packet, length);
        }
        const bool baseOwned = !mixerOwnsEntity(entityId);
        const bool looperHold = entityId == LOOPER_STATE && looperHoldActive();
        const bool looperEchoMatch = looperHold && !fromMixer && length >= 10
            && raw_packet[9] == entityValues[LOOPER_STATE].uint8Value;
        bool keepPredictedLooper = false;
        if (looperEchoMatch) {
            // The base confirmed the predicted transition: end the hold and
            // accept the announcement below, or the state-driven boost and
            // every later rule waits out the whole 4 s window.
            looperHoldUntil = 0;
        } else if (looperHold) {
            // Keep the predicted looper state: mixer display/status echoes
            // are not DSP truth, and a differing base value during the hold
            // is a stale echo of the old state.
            keepPredictedLooper = true;
        }
        if (!keepPredictedLooper && baseOwned && fromMixer) {
            // Mixer display/status echoes are not DSP truth (especially LOOPER_STATE).
        } else if (!keepPredictedLooper && entityType == BOOL) {
            bool value = raw_packet[9];
            setEntityValue(entityId, value, false);
            if (entityListener != nullptr) {
                entityListener(entityId, value ? "1" : "0");
            }
        } else if (entityType == UINT8) {
            uint8_t value = raw_packet[9];
            setEntityValue(entityId, value, false);
            if (entityListener != nullptr) {
                char heard[8];
                snprintf(heard, sizeof(heard), "%u", value);
                entityListener(entityId, heard);
            }
        } else if (entityType == FLOAT) {
            if (fromMixer && analogEntity(entityId)) {
                // Analog ENTITY from the mixer is stored on the UART path and
                // coalesced in tick(). Re-running jitter here used to reject
                // the same packet and skip the UI notify.
            } else {
                float value;
                memcpy(&value, &raw_packet[9], sizeof(float));
                if (fromMixer && analogHoldOn(entityId) && !mixerAccepted) {
                    // rememberMixerEntity left the store alone (knob jitter).
                } else if (!fromMixer && analogHoldOn(entityId)
                    && ignoreAnalogJitter(entityId, snapFloat(entityId, value))) {
                    // The base reports knob moves too (the phones knob lives on
                    // the base), so the jitter gate must cover both directions.
                } else {
                    float stored;
                    if (fromMixer && mixerAccepted) {
                        stored = getFloatEntityValue(entityId);
                    } else {
                        stored = snapFloat(entityId, value);
                        setEntityValue(entityId, stored, false);
                    }
                    if (entityListener != nullptr) {
                        char heard[24];
                        snprintf(heard, sizeof(heard), "%.6g", (double)stored);
                        entityListener(entityId, heard);
                    }
                }
            }
        }
    } else if (packetType == ACK) {
        if (length < 6) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        // Track which side confirms SD packets: BE EF 01 01 00 1E EF BE.
        if (raw_packet[5] == SD_CARD_EVENT && Debug != nullptr) {
            debugLog("uart SD-ACK[%s]", direction == TO_MIXER ? "to-mixer" : "to-base");
        }
    } else if (packetType == BATTERY_LEVEL) {
        // Value form is BE EF 08 16 .. <float at offset 9> EF BE. The short form has no float.
        if (length >= 13) {
            float value;
            memcpy(&value, &raw_packet[9], sizeof(float));
            batteryLevel = value;
            batteryHeard = true;
        }
    } else if (packetType == SD_CARD_EVENT) {
        // Two shapes share type 0x1E (see constants.h for the full dialog):
        //   BE EF 00 1E <code>     EF BE — 7-byte: mixer announce (1 = ready)
        //                                 or the record toggle echo (2 = command)
        //   BE EF 01 1E 00 <state> EF BE — 8-byte: base record state
        // Reading index 4 of the 8-byte shape as the state turned
        // "card available" into "no card" — the old NOT_DETECTED bug.
        if (length < 5) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        uint8_t code = raw_packet[4];
        if (length >= 8 && code == 0x00) {
            uint8_t state = raw_packet[5];
            sdCardHeard = true;
            if (state == 3) {
                sdCardPresent = true;
                if (recStartMs == 0) {
                    recStartMs = millis();
                    // If the module just booted and the first state is already
                    // RECORDING, the recording predates the module — the webui
                    // shows "~" for the unknown pre-boot duration.
                    recPreBoot = millis() < 10000;
                }
                sdCardState = sd_card_state::RECORDING;
            } else if (state == 2 || state == 1) {
                sdCardPresent = true;
                recStartMs = 0;
                recPreBoot = false;
                sdCardState = sd_card_state::READY;
            }
            // state 0 is the base's boot/idle announce; keep what we know.
        } else if (code == sd_card_state::RECORDING && length <= 7) {
            // 7-byte 2 is the toggle command echo, not a state report.
        } else if (code == sd_card_state::READY && length <= 7) {
            sdCardHeard = true;
            if (sdCardState == sd_card_state::NOT_DETECTED) {
                sdCardState = sd_card_state::READY;
            }
        }
        if (Debug != nullptr) {
            debugLog("uart SD[%s] len=%u %02X %02X %02X %02X %02X %02X %02X %02X -> st=%u",
                direction == TO_MIXER ? "to-mixer" : "to-base",
                (unsigned)length,
                length > 0 ? raw_packet[0] : 0, length > 1 ? raw_packet[1] : 0,
                length > 2 ? raw_packet[2] : 0, length > 3 ? raw_packet[3] : 0,
                length > 4 ? raw_packet[4] : 0, length > 5 ? raw_packet[5] : 0,
                length > 6 ? raw_packet[6] : 0, length > 7 ? raw_packet[7] : 0,
                (unsigned)sdCardState);
        }
    } else if (packetType == TUNER_TOGGLE) {
        if (length < 6) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        uint8_t action = raw_packet[4];
        uint8_t chan = raw_packet[5];
        if (action == tuner_action::TURN_ON) {
            if (chan <= 3) {
                if (tunerOn && chan != tunerChannel) {
                    // Stale ON from the channel we just left.
                } else {
                    tunerChannel = chan;
                    tunerOn = true;
                }
            } else {
                tunerOn = true;
            }
        } else if ((int32_t)(millis() - ignoreTunerOffUntil) >= 0) {
            if (chan > 3 || chan == tunerChannel || !tunerOn) {
                tunerOn = false;
                tunerHeard = false;
                lastTunerPitchMs = 0;
            }
        }
    } else if (packetType == TUNER_FEEDBACK) {
        parseTunerFeedback(raw_packet, length);
    } else if (packetType == ALL_ENTITIES) {
        if (length < 9) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        //printRawPacket("[Raw]: ", raw_packet);

        uint8_t* bodyStart = &raw_packet[9]; // Start of the body (it actually starts at 6 with 3 unknown bytes)
        size_t offset = 0; // Offset from the start of the body

        // iterate over enum entity_id

        for (uint8_t i = 0; i <= FX_BYPASS; i++) {
            entity_id entityId = static_cast<entity_id>(i);
            auto dataTypeIt = entity_type_mapping.find(entityId);
            if (dataTypeIt == entity_type_mapping.end()) {
                break;
            }
            entity_data_type dataType = dataTypeIt->second;
            size_t valueSize = dataType == FLOAT ? sizeof(float) : 1;
            if (9 + offset + valueSize > length) {
                break;
            }
            if (dataType == BOOL) {
                bool value = bodyStart[offset];
                if (!(entityId == LOOPER_STATE && looperHoldActive()) && (entityId == LOOPER_STATE || !mixerTouched[entityId])) {
                    setEntityValue(entityId, value, false);
                }
                offset += 1;
            } else if (dataType == UINT8) {
                uint8_t value = bodyStart[offset];
                if (!(entityId == LOOPER_STATE && looperHoldActive()) && (entityId == LOOPER_STATE || !mixerTouched[entityId])) {
                    setEntityValue(entityId, value, false);
                }
                offset += 1;
            } else if (dataType == FLOAT) {
                float value;
                memcpy(&value, &bodyStart[offset], sizeof(float));
                if (!(entityId == LOOPER_STATE && looperHoldActive()) && (entityId == LOOPER_STATE || !mixerTouched[entityId])) {
                    setEntityValue(entityId, value, false);
                }
                offset += 4;
            }
        }
        if (packetType == ALL_ENTITIES) {
            // A mixer dump during a recall means the restore landed; a save
            // still needs its flash write, so only shorten a recall wait.
            if (snapshotBusyUntil != 0 && snapshotPendingAction != static_cast<uint8_t>(snapshot_action::SAVE)) {
                snapshotBusyUntil = millis() + 500;
            }
        }
        dumpGen = dumpGen + 1;
        dumpUnplugDone = false;
        if (!fromMixer) {
            restoreAtMs = millis() + 120;
        }
    }
    if (packetLogEnabled) {
        logPacket(raw_packet, length, direction);
    }
    // Discrete/rare packets (snapshots, menu, looper, tuner, unknown) always
    // land in the diag ring: the wire format of snapshots has never been
    // captured from the real unit, and this is the only path that will.
    {
        static const uint8_t kSkip[] = { ENTITY, HEARTBEAT, SD_CARD_EVENT, DATA_REQUEST, ALL_ENTITIES, ACK, TUNER_FEEDBACK };
        bool common = false;
        for (uint8_t k : kSkip) {
            if (packetType == k) { common = true; break; }
        }
        // Dedupe: the tuner streams identical packets every 100 ms and would
        // evict everything else from the ring.
        static uint32_t lastSig = 0;
        uint32_t sig = (static_cast<uint32_t>(packetType) << 24) ^ (static_cast<uint32_t>(length) << 16) ^ (static_cast<uint32_t>(direction) << 8);
        for (size_t i = 4; i + 1 < length; i++) { sig ^= static_cast<uint32_t>(raw_packet[i]) << ((i % 4) * 8); }
        if (!common && length >= 7 && raw_packet[0] == 0xBE && raw_packet[1] == 0xEF && sig != lastSig) {
            lastSig = sig;
            char hex[3 * 32 + 4] = {};
            size_t hexN = 0;
            for (size_t i = 0; i < length && hexN + 3 < sizeof(hex); i++) {
                hexN += static_cast<size_t>(snprintf(hex + hexN, sizeof(hex) - hexN, "%s%02X", i ? " " : "", raw_packet[i]));
            }
            diagEventForce("uart", "%s packet: %s",
                direction == TO_MIXER ? "BASE->MIXER" : "MIXER->BASE", hex);
        }
    }
    return UARTInterceptor::PACKET_NOT_MODIFIED;
}

void MackieShowbox::enqueueParse(uint8_t* packet, size_t length, UARTInterceptor::Direction direction) {
    if (packet == nullptr || length == 0 || parseQ == nullptr) {
        return;
    }
    portENTER_CRITICAL(&parseMux);
    uint8_t next = static_cast<uint8_t>((parseQHead + 1) % kParseQ);
    if (next == parseQTail) {
        parseDrop = parseDrop + 1;
        portEXIT_CRITICAL(&parseMux);
        return;
    }
    ParseItem& item = parseQ[parseQHead];
    size_t n = length < sizeof(item.data) ? length : sizeof(item.data);
    memcpy(item.data, packet, n);
    item.length = static_cast<uint16_t>(n);
    item.direction = static_cast<uint8_t>(direction);
    parseQHead = next;
    portEXIT_CRITICAL(&parseMux);
}

void MackieShowbox::drainParse() {
    if (parseQ == nullptr) {
        return;
    }
    int n = 0;
    for (;;) {
        ParseItem item;
        portENTER_CRITICAL(&parseMux);
        if (parseQTail == parseQHead) {
            portEXIT_CRITICAL(&parseMux);
            return;
        }
        item = parseQ[parseQTail];
        parseQTail = static_cast<uint8_t>((parseQTail + 1) % kParseQ);
        portEXIT_CRITICAL(&parseMux);
        UARTInterceptor::Direction direction = static_cast<UARTInterceptor::Direction>(item.direction);
        UARTInterceptor::PacketHandlerResult result = handlePacket(item.data, item.length, direction);
        const bool analogEntityPkt = item.length >= 6
            && item.data[3] == ENTITY
            && analogEntity(static_cast<entity_id>(item.data[5]));
        if (packetListener != nullptr && !analogEntityPkt) {
            packetListener(item.data, item.length, direction == TO_BASE ? "mixer" : "base");
        }
        if (postHandlePacketCallback != nullptr) {
            postHandlePacketCallback(item.data, item.length, direction, result);
        }
        if (++n >= 12) {
            return;
        }
    }
}

void MackieShowbox::flushAnalogNotifies() {
    if (!analogDirtyPhones && !analogDirtyLoop) {
        return;
    }
    uint32_t now = millis();
    if ((int32_t)(now - lastAnalogNotifyMs) < (int32_t)kAnalogNotifyMs) {
        return;
    }
    lastAnalogNotifyMs = now;
    if (entityListener == nullptr) {
        analogDirtyPhones = false;
        analogDirtyLoop = false;
        return;
    }
    if (analogDirtyPhones) {
        analogDirtyPhones = false;
        char heard[24];
        snprintf(heard, sizeof(heard), "%.6g", (double)getFloatEntityValue(MAIN_HEADPHONE_GAIN));
        entityListener(MAIN_HEADPHONE_GAIN, heard);
    }
    if (analogDirtyLoop) {
        analogDirtyLoop = false;
        char heard[24];
        snprintf(heard, sizeof(heard), "%.6g", (double)getFloatEntityValue(LOOPER_LEVEL));
        entityListener(LOOPER_LEVEL, heard);
    }
}

void MackieShowbox::uartTask(void* arg) {
    MackieShowbox* self = static_cast<MackieShowbox*>(arg);
    uint32_t spins = 0;
    for (;;) {
        // Keep this loop off the heap lock and debug mutex. A flash/Wi-Fi
        // burst used to hold core 1 long enough for the 128-byte UART FIFO
        // to overflow; the Showbox base then went silent.
        self->interceptor.tick();
        // The looper press sequencing lives here: this task keeps running
        // through BLE-coex radio windows that stall the loop task for
        // seconds, and a delayed UP would read as a long press.
        {
            static uint32_t lastEmitFlushMs = 0;
            uint32_t now = millis();
            if ((int32_t)(now - lastEmitFlushMs) >= 15) {
                lastEmitFlushMs = now;
                self->flushEmits();
            }
        }
        // Cut-through already moved live mixer↔base bytes. Yield often so
        // HID notifies on this core still run; never spin without a delay.
        if (self->interceptor.wirePending()) {
            if (++spins >= 16) {
                spins = 0;
                vTaskDelay(1);
            } else {
                vTaskDelay(0);
            }
        } else {
            spins = 0;
            vTaskDelay(1);
        }
    }
}

void MackieShowbox::tick() {
    if (analogHoldDirty) {
        saveAnalogHold();
    }
    if (!uartTaskRunning) {
        interceptor.tick();
        flushEmits();
    }
    drainParse();
    flushAnalogNotifies();
    if (tunerRetryAt != 0 && (int32_t)(millis() - tunerRetryAt) >= 0) {
        tunerRetryAt = 0;
        if (tunerOn && tunerChannel == tunerRetryChan) {
            sendTunerPacket(tuner_action::TURN_ON, tunerRetryChan);
        }
    }
    if (restoreAtMs != 0 && (int32_t)(millis() - restoreAtMs) >= 0) {
        restoreAtMs = 0;
        restoreMixerEntities();
    }
    const bool live = parseEnabled && uartStarted && linkOk(1500);
    if (dumpLinkLive && !live) {
        dumpLinkLost = true;
    }
    if (live && dumpLinkLost) {
        dumpTries = 0;
        lastDumpReqMs = 0;
        dumpLinkLost = false;
        dumpUnplugDone = false;
        debugLog("uart dump retry after link restore initM=%u initB=%u dumpSeq=%u",
            (unsigned)initMix, (unsigned)initBase, (unsigned)dumpGen);
    }
    dumpLinkLive = live;
    if (live && millis() >= 2000 && (dumpGen == 0 || !batteryHeard)) {
        uint32_t nowDump = millis();
        const int32_t interval = dumpTries < 8 ? 2500 : 8000;
        if (lastDumpReqMs == 0 || (int32_t)(nowDump - lastDumpReqMs) >= interval) {
            lastDumpReqMs = nowDump;
            if (dumpTries < 250) {
                dumpTries = static_cast<uint8_t>(dumpTries + 1);
            }
            // The mixer refuses to re-dump after a module reboot it never
            // saw (its UI still believes the session is synced). Floating
            // the base UART for 300 ms does not make it re-init; 3 s does,
            // and the full entity dump follows. Do this once.
            if (dumpGen == 0 && dumpTries >= 6 && !dumpUnplugDone) {
                dumpUnplugDone = true;
                debugLog("uart dump never arrived after %u tries, mixer re-init via wire unplug", (unsigned)dumpTries);
                interceptor.wireUnplug(3000);
            }
            requestMixerDump();
            debugLog("uart request mixer dump try=%u initM=%u initB=%u dumpSeq=%u batt=%u",
                (unsigned)dumpTries, (unsigned)initMix, (unsigned)initBase,
                (unsigned)dumpGen, batteryHeard ? 1u : 0u);
        }
    }
    uint32_t dropB = interceptor.droppedFrom1();
    uint32_t dropM = interceptor.droppedFrom2();
    uint32_t ovfB = interceptor.overflowFrom1();
    uint32_t ovfM = interceptor.overflowFrom2();
    uint32_t frmB = interceptor.frameErrFrom1();
    uint32_t frmM = interceptor.frameErrFrom2();
    static uint32_t lastDropB = 0;
    static uint32_t lastDropM = 0;
    static uint32_t lastOvfB = 0;
    static uint32_t lastOvfM = 0;
    static uint32_t lastFrmB = 0;
    static uint32_t lastFrmM = 0;
    static uint32_t lastInitM = 0;
    if (initMix != lastInitM && lastInitM != 0) {
        debugLog("CRITICAL uart mixer re-init initM=%u->%u initB=%u hbM=%u hbB=%u",
            (unsigned)lastInitM, (unsigned)initMix, (unsigned)initBase,
            (unsigned)hbMix, (unsigned)hbBase);
    }
    lastInitM = initMix;
    if (dropB != lastDropB || dropM != lastDropM || ovfB != lastOvfB || ovfM != lastOvfM
        || frmB != lastFrmB || frmM != lastFrmM) {
        debugLog("CRITICAL uart dropB=%u->%u dropM=%u->%u ovfB=%u->%u ovfM=%u->%u frmB=%u->%u frmM=%u->%u ringB=%u ringM=%u holdB=%u holdM=%u pendB=%u pendM=%u cap=%u",
            (unsigned)lastDropB, (unsigned)dropB,
            (unsigned)lastDropM, (unsigned)dropM,
            (unsigned)lastOvfB, (unsigned)ovfB,
            (unsigned)lastOvfM, (unsigned)ovfM,
            (unsigned)lastFrmB, (unsigned)frmB,
            (unsigned)lastFrmM, (unsigned)frmM,
            (unsigned)interceptor.ringUsed1(),
            (unsigned)interceptor.ringUsed2(),
            (unsigned)interceptor.holdUsed1(),
            (unsigned)interceptor.holdUsed2(),
            (unsigned)interceptor.pendingTo1(),
            (unsigned)interceptor.pendingTo2(),
            (unsigned)interceptor.pendingCap());
        lastDropB = dropB;
        lastDropM = dropM;
        lastOvfB = ovfB;
        lastOvfM = ovfM;
        lastFrmB = frmB;
        lastFrmM = frmM;
    }
    static uint32_t lastLog = 0;
    uint32_t now = millis();
    // Base-wedge recovery ladder. A reboot mid-transaction can wedge the
    // base's UART state machine; packets never fixed it, but the user proved
    // a physical unplug of the base-side wires does. Emulate that: after a
    // silence threshold, nudge with packets, then tri-state the base-side
    // pins for 3 s and let the normal re-handshake run. The base normally
    // heartbeats within a second of module boot, so "silent 20 s after
    // boot" is also a wedge (a panic reboot cuts the UART mid-transaction
    // without draining), not just "silent after having been heard".
    if (uartStarted) {
        if (uartUpSinceMs == 0) {
            uartUpSinceMs = millis();
        }
        const bool gracePassed = (int32_t)(millis() - uartUpSinceMs) > 20000;
        const bool baseSilent = baseAgeMs() > 8000;
        if (wedgeStage == 0) {
            if (gracePassed && baseSilent && wedgeUnplugs < 3) {
                wedgeSinceMs = now;
                wedgeStage = 1;
                diagEvent("uart", "wedge suspected, base silent %u ms",
                    baseAgeMs() == 0xFFFFFFFFu ? 0u : (unsigned)baseAgeMs());
            }
        } else if (baseAgeMs() < 2000) {
            wedgeStage = 0;
            wedgeUnplugs = 0;
            diagEventForce("uart", "wedge cleared after stage %u", (unsigned)wedgeStage);
        } else {
            const uint32_t held = now - wedgeSinceMs;
            if (wedgeStage == 1 && held >= 6000) {
                wedgeStage = 2;
                uint8_t hb[] = { 0xBE, 0xEF, 0x00, HEARTBEAT, 0x00, 0xEF, 0xBE };
                interceptor.sendPacket(hb, sizeof(hb), TO_BASE);
                uint8_t req[] = { 0xBE, 0xEF, 0x00, DATA_REQUEST, ALL_ENTITIES, 0xEF, 0xBE };
                interceptor.sendPacket(req, sizeof(req), TO_BASE);
                diagEvent("uart", "wedge nudge packets sent (base silent %u ms)", (unsigned)held);
            }
            if (wedgeStage == 2 && held >= 14000
                && (wedgeLastAtMs == 0 || (int32_t)(now - wedgeLastAtMs) > 25000)) {
                wedgeStage = 3;
                wedgeLastAtMs = now;
                wedgeUnplugs = static_cast<uint8_t>(wedgeUnplugs + 1);
                wireUnplug(3000);
            }
            if (wedgeStage == 3 && !interceptor.wireUnplugActive()
                && baseAgeMs() > 4000
                && (int32_t)(now - wedgeLastAtMs) > 20000) {
                if (wedgeUnplugs >= 3) {
                    // The wire unplug cannot clear this wedge, but the user's
                    // physical replug provably does: it power-cycles the
                    // module, and a fresh boot re-handshakes the UART. Do the
                    // same thing safely (drain first), at most once per 5 min
                    // so a physical problem can't become a reboot loop.
                    if (lastUartRebootMs == 0 || (int32_t)(now - lastUartRebootMs) > 300000) {
                        lastUartRebootMs = now;
                        diagEventForce("uart", "wire unplug failed %u times: uart-safe restart", (unsigned)wedgeUnplugs);
                        debugLog("CRITICAL uart wedge: safe restart after %u failed unplug rounds", (unsigned)wedgeUnplugs);
                        waitQuiet(40);
                        delay(50);
                        ESP.restart();
                    } else {
                        diagEvent("uart", "wire unplug failed %u times, restart backoff active", (unsigned)wedgeUnplugs);
                        wedgeStage = 0;
                        wedgeUnplugs = 4;
                    }
                } else {
                    // Unplug done and base still silent: re-arm the ladder.
                    wedgeStage = 1;
                    wedgeSinceMs = now;
                }
            }
        }
    } else {
        uartUpSinceMs = 0;
    }
    if (lastLog == 0 || (int32_t)(now - lastLog) >= 5000) {
        lastLog = now;
        debugLog("uart rxB=%u rxM=%u cutB=%u cutM=%u fifo1=%u fifo2=%u int1=%08x int2=%08x raw1=%08x raw2=%08x ringB=%u ringM=%u holdB=%u holdM=%u pendB=%u pendM=%u dropB=%u dropM=%u ovfB=%u ovfM=%u frmB=%u frmM=%u initM=%u initB=%u hbM=%u hbB=%u heap=%u largest=%u",
            (unsigned)interceptor.rxFrom1(),
            (unsigned)interceptor.rxFrom2(),
            (unsigned)interceptor.cutThroughFrom1(),
            (unsigned)interceptor.cutThroughFrom2(),
            (unsigned)interceptor.hwRxFifo(UART_NUM_1),
            (unsigned)interceptor.hwRxFifo(UART_NUM_2),
            (unsigned)interceptor.hwIntSt(UART_NUM_1),
            (unsigned)interceptor.hwIntSt(UART_NUM_2),
            (unsigned)interceptor.hwIntRaw(UART_NUM_1),
            (unsigned)interceptor.hwIntRaw(UART_NUM_2),
            (unsigned)interceptor.ringUsed1(),
            (unsigned)interceptor.ringUsed2(),
            (unsigned)interceptor.holdUsed1(),
            (unsigned)interceptor.holdUsed2(),
            (unsigned)interceptor.pendingTo1(),
            (unsigned)interceptor.pendingTo2(),
            (unsigned)dropB,
            (unsigned)dropM,
            (unsigned)ovfB,
            (unsigned)ovfM,
            (unsigned)frmB,
            (unsigned)frmM,
            (unsigned)initMix,
            (unsigned)initBase,
            (unsigned)hbMix,
            (unsigned)hbBase,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}

// Set the debug serial
void MackieShowbox::setDebugSerial(Print* serial) {
    Debug = serial;
}

void MackieShowbox::setPacketLog(Print* serial) {
    packetLog = serial;
}

void MackieShowbox::setPacketLogging(bool enable) {
    packetLogEnabled = enable;
}

static const char* packetTypeName(packet_type type) {
    switch (type) {
        case UNKNOWN_00: return "UNKNOWN_00";
        case ACK: return "ACK";
        case ENTITY: return "ENTITY";
        case SNAPSHOT_MENU: return "SNAPSHOT_MENU";
        case ALL_ENTITIES: return "ALL_ENTITIES";
        case DATA_REQUEST: return "DATA_REQUEST";
        case UNKNOWN_15: return "UNKNOWN_15";
        case BATTERY_LEVEL: return "BATTERY_LEVEL";
        case LOOPER_BUTTON: return "LOOPER_BUTTON";
        case TUNER_TOGGLE: return "TUNER_TOGGLE";
        case TUNER_FEEDBACK: return "TUNER_FEEDBACK";
        case SNAPSHOT: return "SNAPSHOT";
        case SD_CARD_EVENT: return "SD_CARD_EVENT";
        case UNKNOWN_FE: return "UNKNOWN_FE";
        case HEARTBEAT: return "HEARTBEAT";
        default: return "UNKNOWN";
    }
}

static const char* entityName(entity_id entityId) {
#ifdef SHOWBOX_DEBUG
    auto it = entity_id_to_string.find(entityId);
    if (it != entity_id_to_string.end()) {
        return it->second.c_str();
    }
#endif
    return nullptr;
}

void MackieShowbox::logPacket(uint8_t* raw_packet, size_t length, UARTInterceptor::Direction direction) {
    if (packetLog == nullptr || raw_packet == nullptr || length < 4) {
        return;
    }
    char line[220];
    const char* dir = direction == TO_MIXER ? "BASE->MIXER" : "MIXER->BASE";
    packet_type packetType = static_cast<packet_type>(raw_packet[3]);
    int n = snprintf(line, sizeof(line), "[UART] %s %s", dir, packetTypeName(packetType));
    if (n < 0) {
        return;
    }
    size_t pos = static_cast<size_t>(n);
    if (packetType == ENTITY && length >= 10) {
        entity_id entityId = static_cast<entity_id>(raw_packet[5]);
        const char* name = entityName(entityId);
        auto entityTypeIt = entity_type_mapping.find(entityId);
        if (name != nullptr) {
            n = snprintf(line + pos, sizeof(line) - pos, " %s", name);
        } else {
            n = snprintf(line + pos, sizeof(line) - pos, " id=%u", static_cast<unsigned>(entityId));
        }
        if (n > 0) {
            pos += static_cast<size_t>(n);
        }
        if (entityTypeIt != entity_type_mapping.end() && pos + 1 < sizeof(line)) {
            entity_data_type entityType = entityTypeIt->second;
            if (entityType == BOOL) {
                n = snprintf(line + pos, sizeof(line) - pos, " %u", static_cast<unsigned>(raw_packet[9]));
            } else if (entityType == UINT8) {
                n = snprintf(line + pos, sizeof(line) - pos, " %u", static_cast<unsigned>(raw_packet[9]));
            } else if (entityType == FLOAT && length >= 13) {
                float value;
                memcpy(&value, &raw_packet[9], sizeof(float));
                n = snprintf(line + pos, sizeof(line) - pos, " %.3f", value);
            } else {
                n = 0;
            }
            if (n > 0) {
                pos += static_cast<size_t>(n);
            }
        }
    } else if (packetType == BATTERY_LEVEL && length >= 13) {
        float value;
        memcpy(&value, &raw_packet[9], sizeof(float));
        n = snprintf(line + pos, sizeof(line) - pos, " %.1f", value);
        if (n > 0) {
            pos += static_cast<size_t>(n);
        }
    } else if (packetType == ALL_ENTITIES) {
        n = snprintf(line + pos, sizeof(line) - pos, " %uB", static_cast<unsigned>(length));
        if (n > 0) {
            pos += static_cast<size_t>(n);
        }
    }
    size_t hexCount = length;
    if (hexCount > 24) {
        hexCount = 24;
    }
    if (pos + 1 < sizeof(line)) {
        line[pos++] = ' ';
    }
    for (size_t i = 0; i < hexCount && pos + 3 < sizeof(line); i++) {
        n = snprintf(line + pos, sizeof(line) - pos, "%02X ", raw_packet[i]);
        if (n <= 0) {
            break;
        }
        pos += static_cast<size_t>(n);
    }
    if (length > hexCount && pos + 4 < sizeof(line)) {
        n = snprintf(line + pos, sizeof(line) - pos, "...");
        if (n > 0) {
            pos += static_cast<size_t>(n);
        }
    }
    if (pos + 1 >= sizeof(line)) {
        pos = sizeof(line) - 2;
    }
    line[pos++] = '\n';
    packetLog->write(reinterpret_cast<const uint8_t*>(line), pos);
}

void MackieShowbox::printRawPacket(const char* message, uint8_t* raw_packet, size_t length) {
    if (raw_packet == nullptr || length < 3) {
        return;
    }
    Debug->printf("%s", message);
    size_t declared = static_cast<size_t>(raw_packet[2]) + 7;
    size_t size = declared < length ? declared : length;
    for (size_t i = 0; i < size; i++) {
        Debug->printf("%02X ", raw_packet[i]);
    }
    Debug->println();
}

// High Level Setters

void MackieShowbox::setFrontLed(bool state) {
    setEntityValue(FRONT_LED, state);
}

void MackieShowbox::setFeedbackElim(uint8_t state) {
    setEntityValue(FEEDBACK_ELIM, state);
}

void MackieShowbox::setAmpPaMode(uint8_t mode) {
    setEntityValue(AMP_PA_MODE, mode);
}

void MackieShowbox::setLocationMode(uint8_t mode) {
    setEntityValue(LOCATION_MODE, mode);
}

void MackieShowbox::setSelectedChan(uint8_t chan) {
    setEntityValue(SELECTED_CHAN, chan);
}

void MackieShowbox::setInputGain(uint8_t input, float gain) {
    if (input >= 4) {
        return; // The Stereo channel does not have gain
    }
    uint8_t inputOffset = INPUT1_GAIN + 17 * input;
    uint8_t inputGainOffset = 0;
    entity_id entityId = static_cast<entity_id>(inputOffset + inputGainOffset);
    setEntityValue(entityId, gain);
}

void MackieShowbox::setInputVolume(uint8_t input, float volume) {
    uint8_t inputOffset = INPUT1_VOLUME + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_VOLUME;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, volume);
}

void MackieShowbox::setInputMute(uint8_t input, bool mute) {
    uint8_t inputOffset = INPUT1_MUTE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_MUTE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, mute);
}

void MackieShowbox::setInputEffectMute(uint8_t input, effect_channel effect, bool mute) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_MUTE + 17 * input;
    uint8_t inputEffectOffset = effect*2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    setEntityValue(entityId, mute);
}

void MackieShowbox::setInputEffectAmount(uint8_t input, effect_channel effect, float amount) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_AMOUNT + 17 * input;
    uint8_t inputEffectOffset = effect*2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    setEntityValue(entityId, amount);
}

void MackieShowbox::setInputEqEnable(uint8_t input, bool enable) {
    uint8_t inputOffset = INPUT1_EQ_ENABLE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_ENABLE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, enable);
}

void MackieShowbox::setInputEqGain(uint8_t input, eq_band band, float gain) {
    uint8_t inputOffset = INPUT1_EQ_LOW_GAIN + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_LOW_GAIN;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset + band);
    setEntityValue(entityId, gain);
}

void MackieShowbox::setInputCompressorEnable(uint8_t input, bool enable) {
    if (input >= 4) {
        return; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_ENABLE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, enable);
}

void MackieShowbox::setInputCompressorAmount(uint8_t input, float amount) {
    if (input >= 4) {
        return; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_AMOUNT + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, amount);
}

void MackieShowbox::setInputExtFxMute(uint8_t input, bool mute) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_MUTE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, mute);
}

void MackieShowbox::setInputExtFxSends(uint8_t input, float sends) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_SENDS + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    setEntityValue(entityId, sends);
}

void MackieShowbox::setInputEffectType(uint8_t input, effect_channel effect, uint8_t type) {
    if (input >= 4) {
        return; // The Stereo channel does not have effects
    }
    uint8_t entityId;
    if (effect == effect_channel::EFFECT1) {
        entityId = 81 + input;
    } else if (effect == effect_channel::EFFECT2) {
        entityId = (input == 0 || input == 2) ? 85 : 86;
    } else {
        return;
    }
    setEntityValue(static_cast<entity_id>(entityId), type);
}

void MackieShowbox::setMainHeadphoneGain(float gain) {
    setEntityValue(MAIN_HEADPHONE_GAIN, gain);
}

void MackieShowbox::setMainMasterGain(float gain) {
    setEntityValue(MAIN_MASTER_GAIN, gain);
}

void MackieShowbox::setMainMute(bool mute) {
    setEntityValue(MAIN_MUTE, mute);
}

void MackieShowbox::setLooperLevel(float level) {
    setEntityValue(LOOPER_LEVEL, level);
}

void MackieShowbox::setLooperState(looper_state state) {
    entityValues[LOOPER_STATE].uint8Value = static_cast<uint8_t>(state);
    looperHoldUntil = millis() + 4000;
}

bool MackieShowbox::looperHoldActive() const {
    return looperHoldUntil != 0 && (int32_t)(millis() - looperHoldUntil) < 0;
}

float MackieShowbox::snapFloat(entity_id entityId, float value) const {
    if (entityId == MAIN_MASTER_GAIN) {
        return nearestStep(value, kMainMasterGain, sizeof(kMainMasterGain) / sizeof(kMainMasterGain[0]));
    }
    if (analogEntity(entityId)) {
        // Hardware analog: −120..+10. 0.5 dB steps keep the pot feel without
        // unique-float floods that overflow parseQ and stall the main loop.
        auto it = entityValues.find(entityId);
        const bool heard = it != entityValues.end() && it->second.heard;
        const float previous = heard ? it->second.floatValue : value;
        return snapAnalogHeld(value, previous, heard);
    }
    if (entityId == STEREO_INPUT1_VOLUME) {
        return nearestStep(value, kInputVolume, sizeof(kInputVolume) / sizeof(kInputVolume[0]));
    }
    if (entityId == STEREO_INPUT1_EQ_LOW_GAIN || entityId == STEREO_INPUT1_EQ_MID_GAIN
        || entityId == STEREO_INPUT1_EQ_HIGH_GAIN) {
        return nearestStep(value, kEqGain, sizeof(kEqGain) / sizeof(kEqGain[0]));
    }
    if (entityId >= INPUT1_GAIN && entityId <= INPUT4_EXT_FX_SENDS) {
        uint8_t off = static_cast<uint8_t>((entityId - INPUT1_GAIN) % 17);
        if (off == 0) {
            return nearestStep(value, kInputGain, sizeof(kInputGain) / sizeof(kInputGain[0]));
        }
        if (off == 1) {
            return nearestStep(value, kInputVolume, sizeof(kInputVolume) / sizeof(kInputVolume[0]));
        }
        if (off == 6 || off == 8) {
            return nearestStep(value, kUnitAmount, sizeof(kUnitAmount) / sizeof(kUnitAmount[0]));
        }
        if (off == 10 || off == 11 || off == 12) {
            return nearestStep(value, kEqGain, sizeof(kEqGain) / sizeof(kEqGain[0]));
        }
        if (off == 14) {
            return nearestStep(value, kUnitAmount, sizeof(kUnitAmount) / sizeof(kUnitAmount[0]));
        }
        if (off == 16) {
            return nearestStep(value, kExtSends, sizeof(kExtSends) / sizeof(kExtSends[0]));
        }
    }
    return value;
}

bool MackieShowbox::analogHoldOn(entity_id entityId) const {
    if (entityId == MAIN_HEADPHONE_GAIN) {
        return analogHoldPhonesFlag;
    }
    if (entityId == LOOPER_LEVEL) {
        return analogHoldLoopFlag;
    }
    return false;
}

bool MackieShowbox::ignoreAnalogJitter(entity_id entityId, float value) {
    if (!analogHoldOn(entityId)) {
        return false;
    }
    AnalogRest& rest = entityId == MAIN_HEADPHONE_GAIN ? phonesRest : loopRest;
    const float thresh = kAnalogHoldNorm * kAnalogSpan;
    if (!rest.have) {
        rest.value = value;
        rest.have = true;
        rest.dir = 0;
        return false;
    }
    const float delta = value - rest.value;
    const float absDelta = fabsf(delta);
    const bool sameDir = (delta > 0.0f && rest.dir > 0) || (delta < 0.0f && rest.dir < 0);
    const float allow = sameDir ? thresh * 0.35f : thresh;
    if (absDelta < allow) {
        rest.value = rest.value + delta * 0.35f;
        return true;
    }
    rest.value = value;
    rest.dir = delta > 0.0f ? 1 : (delta < 0.0f ? -1 : rest.dir);
    return false;
}

void MackieShowbox::setAnalogHold(bool phones, bool loop) {
    analogHoldPhonesFlag = phones;
    analogHoldLoopFlag = loop;
    phonesRest.have = false;
    loopRest.have = false;
    analogHoldDirty = true;
}

void MackieShowbox::loadAnalogHold() {
    Preferences prefs;
    if (!prefs.begin("showbox", true)) {
        return;
    }
    analogHoldPhonesFlag = prefs.getBool("holdPh", true);
    analogHoldLoopFlag = prefs.getBool("holdLp", true);
    prefs.end();
}

void MackieShowbox::saveAnalogHold() {
    Preferences prefs;
    if (!prefs.begin("showbox", false)) {
        return;
    }
    analogHoldDirty = false;
    prefs.putBool("holdPh", analogHoldPhonesFlag);
    prefs.putBool("holdLp", analogHoldLoopFlag);
    prefs.end();
}

bool MackieShowbox::mixerOwnsEntity(entity_id id) const {
    if (id == LOOPER_STATE || id == MAIN_CLIP_OL) {
        return false;
    }
    if (id == INPUT1_CLIP_OL_PRE || id == INPUT1_CLIP_OL_POST
        || id == INPUT2_CLIP_OL_PRE || id == INPUT2_CLIP_OL_POST
        || id == INPUT3_CLIP_OL_PRE || id == INPUT3_CLIP_OL_POST
        || id == INPUT4_CLIP_OL_PRE || id == INPUT4_CLIP_OL_POST
        || id == STEREO_INPUT1_CLIP_OL_PRE || id == STEREO_INPUT1_CLIP_OL_POST) {
        return false;
    }
    return true;
}

void MackieShowbox::setFxBypassState(bool state) {
    setEntityValue(FX_BYPASS, state);
}

// High Level Getters

bool MackieShowbox::getFrontLed() {
    return getBoolEntityValue(FRONT_LED);
}

uint8_t MackieShowbox::getFeedbackElim() {
    return getUint8EntityValue(FEEDBACK_ELIM);
}

bool MackieShowbox::getAmpPaMode() {
    return getBoolEntityValue(AMP_PA_MODE);
}

bool MackieShowbox::getLocationMode() {
    return getBoolEntityValue(LOCATION_MODE);
}

uint8_t MackieShowbox::getSelectedChan() {
    return getUint8EntityValue(SELECTED_CHAN);
}

float MackieShowbox::getInputGain(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have gain
    }
    uint8_t inputOffset = INPUT1_GAIN + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getFloatEntityValue(entityId);
}

float MackieShowbox::getInputVolume(uint8_t input) {
    uint8_t inputOffset = INPUT1_VOLUME + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_VOLUME;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputMute(uint8_t input) {
    uint8_t inputOffset = INPUT1_MUTE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_MUTE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getBoolEntityValue(entityId);
}

bool MackieShowbox::getInputEffectMute(uint8_t input, effect_channel effect) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_MUTE + 17 * input;
    uint8_t inputEffectOffset = effect * 2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputEffectAmount(uint8_t input, effect_channel effect) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have effects
    }
    uint8_t inputOffset = INPUT1_EFFECT_1_AMOUNT + 17 * input;
    uint8_t inputEffectOffset = effect * 2; // 0 or 2
    entity_id entityId = static_cast<entity_id>(inputOffset + inputEffectOffset);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputEqEnable(uint8_t input) {
    uint8_t inputOffset = INPUT1_EQ_ENABLE + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_ENABLE;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputEqGain(uint8_t input, eq_band band) {
    uint8_t inputOffset = INPUT1_EQ_LOW_GAIN + 17 * input;
    if (input == 4) {
        inputOffset = STEREO_INPUT1_EQ_LOW_GAIN;
    }
    entity_id entityId = static_cast<entity_id>(inputOffset + band);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputCompressorEnable(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_ENABLE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputCompressorAmount(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have compressor
    }
    uint8_t inputOffset = INPUT1_COMPRESSOR_AMOUNT + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getFloatEntityValue(entityId);
}

bool MackieShowbox::getInputExtFxMute(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have external effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_MUTE + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getBoolEntityValue(entityId);
}

float MackieShowbox::getInputExtFxSends(uint8_t input) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have external effects
    }
    uint8_t inputOffset = INPUT1_EXT_FX_SENDS + 17 * input;
    entity_id entityId = static_cast<entity_id>(inputOffset);
    return getFloatEntityValue(entityId);
}

uint8_t MackieShowbox::getInputEffectType(uint8_t input, effect_channel effect) {
    if (input >= 4) {
        return 0; // The Stereo channel does not have effects
    }
    uint8_t entityId;
    if (effect == effect_channel::EFFECT1) {
        entityId = 81 + input;
    } else if (effect == effect_channel::EFFECT2) {
        entityId = (input == 0 || input == 2) ? 85 : 86;
    } else {
        return 0;
    }
    return getUint8EntityValue(static_cast<entity_id>(entityId));
}

float MackieShowbox::getMainHeadphoneGain() {
    return getFloatEntityValue(MAIN_HEADPHONE_GAIN);
}

float MackieShowbox::getMainMasterGain() {
    return getFloatEntityValue(MAIN_MASTER_GAIN);
}

bool MackieShowbox::getMainMute() {
    return getBoolEntityValue(MAIN_MUTE);
}

float MackieShowbox::getLooperLevel() {
    return getFloatEntityValue(LOOPER_LEVEL);
}

looper_state MackieShowbox::getLooperState() {
    return static_cast<looper_state>(getUint8EntityValue(LOOPER_STATE));
}

bool MackieShowbox::getFxBypassState() {
    return getBoolEntityValue(FX_BYPASS);
}

