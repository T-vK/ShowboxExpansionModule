#ifndef PIN_DEFINITIONS_H
#define PIN_DEFINITIONS_H

#include <Arduino.h>

// Mackie Showbox Base-Unit
constexpr uint8_t SHOWBOX_BASE_TX = 11; // Base TX (Blue)
constexpr uint8_t SHOWBOX_BASE_RX = 12; // Base RX (Green)

// Mackie Showbox Breakaway Mixer
constexpr uint8_t SHOWBOX_MIXER_RX = 13; // Mixer RX (Blue)
constexpr uint8_t SHOWBOX_MIXER_TX = 14; // Mixer TX (Green)

// TRS Jacks (Footswitches / Expression Pedals)
constexpr uint8_t TRS1_TIP = 8;
constexpr uint8_t TRS1_RING = 2;
constexpr uint8_t TRS2_TIP = 21;
constexpr uint8_t TRS2_RING = 1;
constexpr uint8_t TRS3_TIP = 47;
constexpr uint8_t TRS3_RING = 4;
constexpr uint8_t TRS4_TIP = 48;
constexpr uint8_t TRS4_RING = 5;

// MIDI UART Module (Arduino MIDI Shield)
constexpr uint8_t MIDI_TX = 9;
constexpr uint8_t MIDI_RX = 10;

// RGB LED Controller (P9813)
constexpr uint8_t RGB_DATA_IN = 42;
constexpr uint8_t RGB_CLOCK_IN = 41;

// Buttons
constexpr uint8_t GREEN_BUTTON = 40;
constexpr uint8_t RED_BUTTON = 39;
constexpr uint8_t BLUE_BUTTON = 38;

// SPI Ethernet Module (Mini W5500)
constexpr uint8_t W5500_nRESET = 6;
constexpr uint8_t W5500_MISO = 7;
constexpr uint8_t W5500_nINT = 15;
constexpr uint8_t W5500_nSS = 16;
constexpr uint8_t W5500_SCLK = 17;
constexpr uint8_t W5500_MOSI = 18;

#endif // PIN_DEFINITIONS_H
