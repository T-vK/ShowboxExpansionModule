#pragma once

#include <Arduino.h>

enum class UsbBoot {
    SerialJtag,
    MidiDevice,
    MidiHost,
    HidPeripheral
};

UsbBoot usbBootMode();
void usbModeClear();
void usbRestoreJtag();
void usbRevertToJtag(const char* reason);
void usbArmSafetyTimer(uint32_t ms);
void usbLoadIsoAndFailCount();
void usbTickWatch();
bool usbStartSelected();

extern UsbBoot usbBoot;
extern bool usbWantMidi;
extern bool usbWantHid;
extern bool usbTestBoot;
extern bool usbFailCleared;
