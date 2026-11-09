#pragma once

#include <cstdint>

// Port for Wi-Fi / HTTP coexistence hints used by BleMidi.
// App code (LanKeep + HttpsGate) supplies the implementation via main.
// MultiMidi must not #include LanKeep or HttpsGate.
struct LanCoex {
    void (*noteBleLive)(bool on) = nullptr;
    void (*preferWifiWithBle)() = nullptr;
    bool (*wsLive)() = nullptr;
    bool (*holdMidi)() = nullptr;
    uint32_t (*httpQuietMs)() = nullptr;
    bool (*holdOutgoingHttps)() = nullptr;
    void (*kickArp)() = nullptr;
    void (*recoverUnicast)() = nullptr;
    void (*restoreBleSleepIfIdle)() = nullptr;
};

inline void lanCoexNoteBleLive(const LanCoex* c, bool on) {
    if (c != nullptr && c->noteBleLive != nullptr) {
        c->noteBleLive(on);
    }
}

inline void lanCoexPreferWifiWithBle(const LanCoex* c) {
    if (c != nullptr && c->preferWifiWithBle != nullptr) {
        c->preferWifiWithBle();
    }
}

inline bool lanCoexWsLive(const LanCoex* c) {
    return c != nullptr && c->wsLive != nullptr && c->wsLive();
}

inline bool lanCoexHoldMidi(const LanCoex* c) {
    return c != nullptr && c->holdMidi != nullptr && c->holdMidi();
}

inline uint32_t lanCoexHttpQuietMs(const LanCoex* c) {
    return (c != nullptr && c->httpQuietMs != nullptr) ? c->httpQuietMs() : 0;
}

inline bool lanCoexHoldOutgoingHttps(const LanCoex* c) {
    return c != nullptr && c->holdOutgoingHttps != nullptr && c->holdOutgoingHttps();
}

inline void lanCoexKickArp(const LanCoex* c) {
    if (c != nullptr && c->kickArp != nullptr) {
        c->kickArp();
    }
}

inline void lanCoexRecoverUnicast(const LanCoex* c) {
    if (c != nullptr && c->recoverUnicast != nullptr) {
        c->recoverUnicast();
    }
}

inline void lanCoexRestoreBleSleepIfIdle(const LanCoex* c) {
    if (c != nullptr && c->restoreBleSleepIfIdle != nullptr) {
        c->restoreBleSleepIfIdle();
    }
}
