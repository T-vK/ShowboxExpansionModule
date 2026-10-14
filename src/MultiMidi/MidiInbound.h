#ifndef MIDI_INBOUND_H
#define MIDI_INBOUND_H

#include "MidiAction.h"
#include <cstdint>

const char* midiInboundSource();
void midiInboundSetSource(const char* name);
const char* midiKindFromStatus(uint8_t status);
void midiInboundDispatch(midi::MidiAction* action, uint8_t status, uint8_t p1, uint8_t p2);

#endif // MIDI_INBOUND_H
