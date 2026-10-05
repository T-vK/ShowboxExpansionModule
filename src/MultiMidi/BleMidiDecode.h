#ifndef BLE_MIDI_DECODE_H
#define BLE_MIDI_DECODE_H

#include "MidiAction.h"
#include "MultiMidi/MidiInbound.h"
#include <BLECharacteristic.h>
#include <cstddef>
#include <cstdint>

// BLE MIDI running-status packets omit the status byte. The stock parser
// treats the timestamp (0x80) as Note Off, so a second CC 102=127 is dropped.
class BleMidiDecode : public BLECharacteristicCallbacks {
public:
    explicit BleMidiDecode(midi::MidiAction* action = nullptr) : action(action) {}

    void setAction(midi::MidiAction* next) {
        action = next;
    }

    void feed(const uint8_t* data, size_t len) {
        if (action == nullptr || data == nullptr || len < 2) {
            return;
        }
        midiInboundSetSource("ble");
        size_t i = 1;
        bool wantTimestamp = true;
        while (i < len) {
            uint8_t byte = data[i];
            if (byte >= 0xF8) {
                i++;
                continue;
            }
            if (wantTimestamp && (byte & 0x80) != 0) {
                i++;
                wantTimestamp = false;
                continue;
            }
            uint8_t statusByte;
            if ((byte & 0x80) != 0) {
                statusByte = byte;
                runningStatus = byte;
                i++;
            } else if (runningStatus >= 0x80) {
                statusByte = runningStatus;
            } else {
                i++;
                continue;
            }
            int needed = dataBytesFor(statusByte);
            uint8_t p1 = 0;
            uint8_t p2 = 0;
            if (needed >= 1) {
                if (i >= len || (data[i] & 0x80) != 0) {
                    wantTimestamp = true;
                    continue;
                }
                p1 = data[i++];
            }
            if (needed >= 2) {
                if (i >= len || (data[i] & 0x80) != 0) {
                    wantTimestamp = true;
                    continue;
                }
                p2 = data[i++];
            }
            emit(statusByte, p1, p2);
            wantTimestamp = true;
        }
    }

    void onWrite(BLECharacteristic* characteristic) override {
        if (characteristic == nullptr) {
            return;
        }
        feed(reinterpret_cast<const uint8_t*>(characteristic->getValue().c_str()),
            characteristic->getValue().length());
    }

private:
    midi::MidiAction* action = nullptr;
    uint8_t runningStatus = 0;

    static int dataBytesFor(uint8_t statusByte) {
        uint8_t kind = statusByte & 0xF0;
        if (kind == 0xC0 || kind == 0xD0) {
            return 1;
        }
        if (kind >= 0x80 && kind <= 0xE0) {
            return 2;
        }
        return 0;
    }

    void emit(uint8_t statusByte, uint8_t p1, uint8_t p2) {
        uint8_t kind = statusByte >> 4;
        uint8_t channel = statusByte & 0x0F;
        if (kind == 0xB) {
            action->onControlChange(channel, p1, p2);
        } else if (kind == 0x9) {
            if (p2 == 0) {
                action->onNoteOff(channel, p1, p2);
            } else {
                action->onNoteOn(channel, p1, p2);
            }
        } else if (kind == 0x8) {
            action->onNoteOff(channel, p1, p2);
        } else if (kind == 0xE) {
            action->onPitchBend(channel, p1);
        }
    }
};

#endif // BLE_MIDI_DECODE_H
