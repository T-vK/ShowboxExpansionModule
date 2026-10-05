#pragma once

#include <stddef.h>
#include <stdint.h>

void debugLog(const char* fmt, ...);
size_t debugLogCopy(char* out, size_t cap);

// UART isolation ladder (NVS debug/iso). 0 is the product.
// 3 UART+Wi-Fi only  4 +DIN only  5 +extras only  2 +DIN/extras, no BLE/Apple
// 1 +AppleMIDI, no BLE
// 10 BLE idle (no ads/scan/links)  11 +advertising  12 +AIRSTEP  13 +HID host
#ifndef UART_SERVICE_LOCK
#define UART_SERVICE_LOCK 0
#endif
extern int gIso;
extern volatile int gIsoPending;
extern volatile uint32_t gIsoRebootAt;
extern volatile int gSoakSeconds;
extern volatile bool gSoakRunning;
bool uartSoakResult(char* out, size_t cap);
