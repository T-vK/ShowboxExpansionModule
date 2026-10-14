#pragma once

#include <stddef.h>
#include <stdint.h>

// Persistent, developer-facing diagnostics.
//
// diagEvent writes into a RAM ring that is mirrored into RTC no-init memory,
// so the last events survive a soft reset, a panic reboot, or deep sleep and
// are captured as "previous boot" after restart. Crash boots additionally
// store a compact tail into NVS (rate limited to crash boots only) so the
// story survives even a power cycle.
//
// diagStatsJson builds the task-manager JSON: per-core load, per-task CPU and
// stack headroom, memory, flash, network, UART wire health, and MIDI out.

void diagBegin();
void diagTick();

// The loop calls diagLoopBeat() every pass. A watchdog timer (esp_timer, runs
// outside the loop) detects a frozen loop within ~3 s and invokes the
// registered callback immediately — a pass can be stuck for minutes inside a
// single blocked write, so waiting for the pass to end is far too late.
void diagLoopBeat();
void diagOnLoopStall(void (*cb)(uint32_t stallMs));

// Rate limited per kind (2 s); use _force for state changes that must land.
void diagEvent(const char* kind, const char* fmt, ...);
void diagEventForce(const char* kind, const char* fmt, ...);

// Telemetry hooks from the hot paths (cheap atomics only).
void diagNoteWsTx(size_t bytes);
void diagNoteWsRetry();
void diagNoteMidiOut(size_t bytes);

uint32_t diagWsTxBytes();
uint32_t diagWsRetries();
uint32_t diagMidiOutMsgs();

// "stats" is the task-manager payload; the diag event ring rides along.
size_t diagStatsJson(char* out, size_t cap);