#pragma once

#include "MidiParser.h"
#include "MultiMidi/MidiInbound.h"

class FullMidiParser : public midi::MidiParser {
public:
    using midi::MidiParser::MidiParser;

    void onCommand(uint8_t channel, uint8_t status, uint8_t p1, uint8_t p2) override {
        uint8_t statusByte = static_cast<uint8_t>((status << 4) | (channel & 0x0F));
        if (status >= 0xF) {
            statusByte = static_cast<uint8_t>(0xF0 | (channel & 0x0F));
        }
        midiInboundDispatch(p_MidiAction, statusByte, p1, p2);
    }
};
