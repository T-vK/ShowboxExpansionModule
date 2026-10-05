#define SHOWBOX_DEBUG

#include "MackieShowbox.h"
#include "DebugLog.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#ifdef SHOWBOX_DEBUG
#include "string_mappings.h"
#endif

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
                rememberMixerEntity(packet, length);
            } else if (t == DATA_REQUEST && fromMixer) {
                reqMix = reqMix + 1;
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

void MackieShowbox::setEntityValue(entity_id entityId, bool value, bool emit) {
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    }
    entityValues[entityId].boolValue = value;
}
void MackieShowbox::setEntityValue(entity_id entityId, uint8_t value, bool emit) {
    if (emit) {
        uint8_t packet[12] = { 0xBE, 0xEF, 0x05, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, value, 0xEF, 0xBE };
        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    }
    entityValues[entityId].uint8Value = value;
}
void MackieShowbox::setEntityValue(entity_id entityId, float value, bool emit) {
    if (emit) {
        uint8_t floatBytes[4];
        memcpy(floatBytes, &value, sizeof(float));

        uint8_t packet[15] = { 0xBE, 0xEF, 0x08, 0x03, 0x00, entityId, 0x00, 0x00, 0x00, floatBytes[0], floatBytes[1], floatBytes[2], floatBytes[3], 0xEF, 0xBE };

        interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
        interceptor.sendPacket(packet, sizeof(packet), TO_MIXER);
    }
    entityValues[entityId].floatValue = value;
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

void MackieShowbox::sendLooperButtonAction(looper_button_action action) {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x01, 0x17, 0x00, action, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
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
    uint8_t packet[] = { 0xBE, 0xEF, 0x00, SD_CARD_EVENT, sd_card_action::TOGGLE_RECORD, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

void MackieShowbox::snapshotAction(snapshot_action action, snapshot_slot slot) {
    uint8_t packet[] = { 0xBE, 0xEF, 0x01, SNAPSHOT, static_cast<uint8_t>(action), static_cast<uint8_t>(slot), 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

void MackieShowbox::tunerAction(tuner_action action, tuner_chan chan) {
    uint8_t packet[8] = { 0xBE, 0xEF, 0x01, TUNER_TOGGLE, action, chan, 0xEF, 0xBE };
    interceptor.sendPacket(packet, sizeof(packet), TO_BASE);
}

float MackieShowbox::getBatteryLevel() {
    return batteryLevel;
}

sd_card_state MackieShowbox::getSdCardState() {
    return sdCardState;
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

void MackieShowbox::rememberMixerEntity(const uint8_t* packet, size_t length) {
    if (packet == nullptr || length < 10) {
        return;
    }
    uint8_t id = packet[5];
    if (id > FX_BYPASS) {
        return;
    }
    if (!mixerOwnsEntity(static_cast<entity_id>(id))) {
        return;
    }
    if (ignoreTouchUntil != 0 && (int32_t)(millis() - ignoreTouchUntil) < 0) {
        return;
    }
    auto it = entity_type_mapping.find(static_cast<entity_id>(id));
    if (it == entity_type_mapping.end()) {
        return;
    }
    mixerTouched[id] = true;
    if (it->second == BOOL) {
        entityValues[static_cast<entity_id>(id)].boolValue = packet[9] != 0;
    } else if (it->second == UINT8) {
        entityValues[static_cast<entity_id>(id)].uint8Value = packet[9];
    } else if (length >= 13) {
        float value;
        memcpy(&value, &packet[9], sizeof(float));
        entityValues[static_cast<entity_id>(id)].floatValue = value;
    }
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
        if (fromMixer) {
            rememberMixerEntity(raw_packet, length);
        }
        const bool baseOwned = !mixerOwnsEntity(entityId);
        if (baseOwned && fromMixer) {
            // Mixer display/status echoes are not DSP truth (especially LOOPER_STATE).
        } else if (entityType == BOOL) {
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
            float value;
            memcpy(&value, &raw_packet[9], sizeof(float));
            setEntityValue(entityId, value, false);
            if (entityListener != nullptr) {
                char heard[24];
                snprintf(heard, sizeof(heard), "%.4g", value);
                entityListener(entityId, heard);
            }
        }
    } else if (packetType == ACK) {
        if (length < 6) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
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
        // BE EF 00 1E <state> EF BE — the state byte is at index 4, not the end signature.
        if (length < 5) {
            return UARTInterceptor::PACKET_NOT_MODIFIED;
        }
        sdCardState = static_cast<sd_card_state>(raw_packet[4]);
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
                if (entityId == LOOPER_STATE || !mixerTouched[entityId]) {
                    setEntityValue(entityId, value, false);
                }
                offset += 1;
            } else if (dataType == UINT8) {
                uint8_t value = bodyStart[offset];
                if (entityId == LOOPER_STATE || !mixerTouched[entityId]) {
                    setEntityValue(entityId, value, false);
                }
                offset += 1;
            } else if (dataType == FLOAT) {
                float value;
                memcpy(&value, &bodyStart[offset], sizeof(float));
                if (entityId == LOOPER_STATE || !mixerTouched[entityId]) {
                    setEntityValue(entityId, value, false);
                }
                offset += 4;
            }
        }
        if (!fromMixer) {
            restoreAtMs = millis() + 120;
        }
    }
    if (packetLogEnabled) {
        logPacket(raw_packet, length, direction);
    }
    return UARTInterceptor::PACKET_NOT_MODIFIED;
}

void MackieShowbox::enqueueParse(uint8_t* packet, size_t length, UARTInterceptor::Direction direction) {
    if (packet == nullptr || length == 0) {
        return;
    }
    portENTER_CRITICAL(&parseMux);
    uint8_t next = static_cast<uint8_t>((parseQHead + 1) % kParseQ);
    if (next == parseQTail) {
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
        if (postHandlePacketCallback != nullptr) {
            postHandlePacketCallback(item.data, item.length, direction, result);
        }
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
    if (!uartTaskRunning) {
        interceptor.tick();
    }
    drainParse();
    if (restoreAtMs != 0 && (int32_t)(millis() - restoreAtMs) >= 0) {
        restoreAtMs = 0;
        restoreMixerEntities();
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

