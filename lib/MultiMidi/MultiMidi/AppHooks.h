#pragma once

#include <cstddef>
#include <cstdint>

// App-side hooks so MultiMidi does not include WiredNet / Diag.
struct MidiAppHooks {
    bool (*hasIp)() = nullptr;
    void (*noteMidiOut)(size_t bytes) = nullptr;
};

inline bool midiAppHasIp(const MidiAppHooks* h) {
    return h != nullptr && h->hasIp != nullptr && h->hasIp();
}

inline void midiAppNoteMidiOut(const MidiAppHooks* h, size_t bytes) {
    if (h != nullptr && h->noteMidiOut != nullptr) {
        h->noteMidiOut(bytes);
    }
}
