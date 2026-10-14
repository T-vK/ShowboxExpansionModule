#pragma once

#include <stddef.h>
#include <stdint.h>
#include <Arduino.h>

void debugLog(const char* fmt, ...);
void debugLogUsbTick();
size_t debugLogCopy(char* out, size_t cap);
size_t consoleCopySince(uint32_t since, uint8_t* out, size_t cap, uint32_t* next);
uint32_t consoleDropped();
uint32_t consoleSeq();
void consoleClear();
extern Print* Debug;

// UART isolation ladder (NVS debug/iso). 0 is the product.
// 3 UART+Wi-Fi only  4 +DIN only  5 +extras only  2 +DIN/extras, no BLE/Apple
// 1 +AppleMIDI, no BLE
// 10 BLE idle (no ads/scan/links)  11 +advertising  12 +AIRSTEP  13 +HID host
#ifndef UART_SERVICE_LOCK
#define UART_SERVICE_LOCK 0
#endif
extern int gIso;
extern volatile int gIsoPending;
// Master gate for the WiFi watchdogs (gateway ping, TCP-listen probe) and
// the diag sampler. Elimination switch for the WiFi instability hunt:
// false restores the pre-diag WiFi behavior entirely.
extern volatile bool gDiagWatch;
extern volatile uint32_t gIsoRebootAt;
extern volatile int gSoakSeconds;
extern volatile bool gSoakRunning;
bool uartSoakResult(char* out, size_t cap);
void uartSoakStore(const char* json);
