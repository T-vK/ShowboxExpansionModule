#include "UsbMidi.h"

#include "MidiAction.h"
#include "MidiCommon.h"
#include "MidiParser.h"
#include "USB.h"
#include "esp32-hal-tinyusb.h"

namespace {
bool started = false;
midi::MidiParser parser;
bool parserReady = false;

uint16_t loadMidiDescriptor(uint8_t* dst, uint8_t* itf) {
    uint8_t name = tinyusb_add_string_descriptor("ShowboxMod");
    uint8_t descriptor[TUD_MIDI_DESC_LEN] = {
        TUD_MIDI_DESCRIPTOR(*itf, name, 0x01, 0x81, 64)
    };
    *itf += 2;
    memcpy(dst, descriptor, sizeof(descriptor));
    return sizeof(descriptor);
}

int messageLength(uint8_t status) {
    if (status >= 0xF8) {
        return 1;
    }
    uint8_t kind = status & 0xF0;
    if (kind == 0xC0 || kind == 0xD0) {
        return 2;
    }
    return 3;
}
}

bool UsbMidi::start() {
    if (started) {
        return true;
    }
    if (tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN, loadMidiDescriptor) != ESP_OK) {
        return false;
    }
    USB.manufacturerName("Showbox");
    USB.productName("ShowboxMod");
    USB.VID(0x303A);
    USB.PID(0x0002);
    started = USB.begin();
    return started;
}

bool UsbMidi::running() {
    return started;
}

void UsbMidi::tick(midi::MidiAction* action) {
    if (!started || action == nullptr) {
        return;
    }
    if (!parserReady) {
        parser.begin(action);
        parserReady = true;
    }
    uint8_t buffer[64];
    while (tud_midi_n_available(0, 0) > 0) {
        uint32_t count = tud_midi_n_stream_read(0, 0, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        parser.parse(buffer, static_cast<uint8_t>(count > 255 ? 255 : count));
    }
}

void UsbMidi::write(midi::MidiMessage* msg, int len) {
    if (!started || msg == nullptr || len <= 0) {
        return;
    }
    for (int i = 0; i < len; i++) {
        uint8_t bytes[3] = {msg[i].status, msg[i].arg1, msg[i].arg2};
        int count = messageLength(msg[i].status);
        tud_midi_n_stream_write(0, 0, bytes, count);
    }
}
