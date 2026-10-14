#pragma once

#include "DebugLog.h"

// UART isolation ladder lives in NVS debug/iso. 0 is the product path.
inline bool isoBle(bool btRadioOn) {
    return (gIso == 0 || gIso >= 10) && btRadioOn;
}
inline bool isoApple() {
    return gIso == 0 || gIso == 1 || gIso >= 10;
}
inline bool isoDin() {
    return gIso == 0 || gIso == 1 || gIso == 2 || gIso == 4 || gIso >= 10;
}
inline bool isoExtras() {
    return gIso == 0 || gIso == 1 || gIso == 2 || gIso == 5 || gIso >= 10;
}
inline bool isoOutgoing() {
    return gIso == 0 || gIso == 12;
}
inline bool isoHid() {
    return gIso == 0 || gIso == 13;
}
