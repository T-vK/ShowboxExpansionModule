#ifndef MIDI_INBOUND_H
#define MIDI_INBOUND_H

#include "MidiAction.h"
#include <cstdint>

// App registers ActionRules (or tests) here — MultiMidi must not include Actions.
using MidiRulesFn = void (*)(const char* kind, uint8_t channel, uint8_t number, uint8_t value);

const char* midiInboundSource();
void midiInboundSetSource(const char* name);
void midiInboundSetRules(MidiRulesFn fn);
const char* midiKindFromStatus(uint8_t status);
void midiInboundDispatch(midi::MidiAction* action, uint8_t status, uint8_t p1, uint8_t p2);

#endif // MIDI_INBOUND_H
