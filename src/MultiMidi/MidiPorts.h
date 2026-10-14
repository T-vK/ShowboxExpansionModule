#pragma once

#include <ArduinoJson.h>
#include <Arduino.h>
#include <cstdint>
#include <cstring>

constexpr uint16_t kMidiPortDin = 1 << 0;
constexpr uint16_t kMidiPortBle = 1 << 1;
constexpr uint16_t kMidiPortApple = 1 << 2;
constexpr uint16_t kMidiPortUsb = 1 << 3;
constexpr uint16_t kMidiPortUsbHost = 1 << 4;
constexpr uint16_t kMidiPortEth = 1 << 5;
constexpr uint16_t kMidiPortAll = 0;

inline uint16_t midiPortBit(const char* name) {
    if (name == nullptr || name[0] == '\0' || strcmp(name, "*") == 0 || strcmp(name, "all") == 0) {
        return kMidiPortAll;
    }
    if (strcmp(name, "din") == 0) {
        return kMidiPortDin;
    }
    if (strcmp(name, "ble") == 0 || strcmp(name, "bluetooth") == 0) {
        return kMidiPortBle;
    }
    if (strcmp(name, "apple") == 0) {
        return kMidiPortApple;
    }
    if (strcmp(name, "usb") == 0) {
        return kMidiPortUsb;
    }
    if (strcmp(name, "usbhost") == 0) {
        return kMidiPortUsbHost;
    }
    if (strcmp(name, "eth") == 0 || strcmp(name, "ethernet") == 0) {
        return kMidiPortEth;
    }
    return kMidiPortAll;
}

inline uint16_t midiPortMask(JsonVariantConst ports) {
    if (ports.isNull()) {
        return kMidiPortAll;
    }
    if (ports.is<const char*>() || ports.is<String>()) {
        return midiPortBit(ports.as<const char*>());
    }
    if (!ports.is<JsonArrayConst>()) {
        return kMidiPortAll;
    }
    uint16_t mask = 0;
    for (JsonVariantConst item : ports.as<JsonArrayConst>()) {
        mask |= midiPortBit(item.as<const char*>());
    }
    return mask;
}

inline bool midiPortWanted(uint16_t mask, uint16_t bit) {
    return mask == kMidiPortAll || (mask & bit) != 0;
}

inline bool midiPortNameMatches(JsonVariantConst ports, const char* source) {
    if (ports.isNull() || source == nullptr || source[0] == '\0') {
        return true;
    }
    if (ports.is<const char*>() || ports.is<String>()) {
        const char* name = ports.as<const char*>();
        if (name == nullptr || name[0] == '\0' || strcmp(name, "*") == 0 || strcmp(name, "all") == 0) {
            return true;
        }
        return strcmp(name, source) == 0;
    }
    if (!ports.is<JsonArrayConst>()) {
        return true;
    }
    JsonArrayConst list = ports.as<JsonArrayConst>();
    if (list.size() == 0) {
        return true;
    }
    for (JsonVariantConst item : list) {
        const char* name = item.as<const char*>();
        if (name != nullptr && (strcmp(name, source) == 0 || strcmp(name, "*") == 0 || strcmp(name, "all") == 0)) {
            return true;
        }
    }
    return false;
}
