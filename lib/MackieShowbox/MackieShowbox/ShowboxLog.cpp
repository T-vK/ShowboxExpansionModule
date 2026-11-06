#include "ShowboxLog.h"

#include <cstdarg>
#include <cstdio>

namespace {
ShowboxLogFn gLog = nullptr;
ShowboxDiagFn gDiag = nullptr;
ShowboxDiagFn gDiagForce = nullptr;
}

void showboxSetLog(ShowboxLogFn log) {
    gLog = log;
}

void showboxSetDiag(ShowboxDiagFn event, ShowboxDiagFn eventForce) {
    gDiag = event;
    gDiagForce = eventForce;
}

void showboxLog(const char* fmt, ...) {
    if (gLog == nullptr || fmt == nullptr) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gLog("%s", buf);
}

void showboxDiag(const char* kind, const char* fmt, ...) {
    if (gDiag == nullptr || fmt == nullptr) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gDiag(kind, "%s", buf);
}

void showboxDiagForce(const char* kind, const char* fmt, ...) {
    if (gDiagForce == nullptr || fmt == nullptr) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gDiagForce(kind, "%s", buf);
}
