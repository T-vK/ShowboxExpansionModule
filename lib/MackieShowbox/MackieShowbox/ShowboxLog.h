#pragma once

// Logging port for MackieShowbox — app wires these to DebugLog/Diag.
// Keeps the protocol class free of DebugLog.h / System/Diag.h includes.
using ShowboxLogFn = void (*)(const char* fmt, ...);
using ShowboxDiagFn = void (*)(const char* kind, const char* fmt, ...);

void showboxSetLog(ShowboxLogFn log);
void showboxSetDiag(ShowboxDiagFn event, ShowboxDiagFn eventForce);

void showboxLog(const char* fmt, ...);
void showboxDiag(const char* kind, const char* fmt, ...);
void showboxDiagForce(const char* kind, const char* fmt, ...);
