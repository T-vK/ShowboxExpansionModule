// Looper.h
#ifndef LOOPER_H
#define LOOPER_H

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace Showbox {
    namespace Looper {
        constexpr uint8_t RECORD_INITIAL_LOOP = 1;
        constexpr uint8_t PLAY = 3;
        constexpr uint8_t STOP_PLAYING = 2;
        constexpr uint8_t RECORD_OVERDUB = 4;
        constexpr uint8_t DELETE = 0;

        #ifdef SHOWBOX_DEBUG
        inline const char* getName(uint8_t state) {
            switch (state) {
                case RECORD_INITIAL_LOOP: return "RECORD_INITIAL_LOOP";
                case PLAY: return "PLAY";
                case STOP_PLAYING: return "STOP_PLAYING";
                case RECORD_OVERDUB: return "RECORD_OVERDUB";
                case DELETE: return "DELETE";
                default: return "UNKNOWN";
            }
        }
        #endif // SHOWBOX_DEBUG
    } // namespace Looper
} // namespace Showbox

#endif // LOOPER_H