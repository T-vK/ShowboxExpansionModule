#include "UsbMidiHost.h"

#include "MidiAction.h"
#include "MidiCommon.h"
#include "MidiParser.h"
#include <usb/usb_host.h>
#include <cstring>

namespace {
bool started = false;
bool deviceReady = false;
char statusText[64] = "USB MIDI host is on. No MIDI device yet.";
usb_host_client_handle_t client = nullptr;
usb_device_handle_t device = nullptr;
usb_transfer_t* inTransfer = nullptr;
usb_transfer_t* outTransfer = nullptr;
uint8_t inAddress = 0;
uint8_t outAddress = 0;
uint16_t inPacket = 64;
bool outBusy = false;
SemaphoreHandle_t outLock = nullptr;
uint8_t outBytes[64];
int outCount = 0;

uint8_t midiQueue[256];
int queueHead = 0;
int queueTail = 0;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
midi::MidiParser parser;
bool parserReady = false;

void pushMidi(const uint8_t* data, int len) {
    portENTER_CRITICAL(&queueMux);
    for (int i = 0; i < len; i++) {
        int next = (queueHead + 1) % (int)sizeof(midiQueue);
        if (next == queueTail) {
            break;
        }
        midiQueue[queueHead] = data[i];
        queueHead = next;
    }
    portEXIT_CRITICAL(&queueMux);
}

int cinLength(uint8_t cin) {
    switch (cin) {
        case 0x5:
        case 0xF:
            return 1;
        case 0x2:
        case 0x6:
        case 0xC:
        case 0xD:
            return 2;
        case 0x0:
        case 0x1:
            return 0;
        default:
            return 3;
    }
}

uint8_t cinFor(uint8_t status) {
    if (status >= 0xF8) {
        return 0xF;
    }
    uint8_t kind = status & 0xF0;
    if (kind == 0xC0) {
        return 0xC;
    }
    if (kind == 0xD0) {
        return 0xD;
    }
    if (kind == 0x80) {
        return 0x8;
    }
    if (kind == 0x90) {
        return 0x9;
    }
    if (kind == 0xA0) {
        return 0xA;
    }
    if (kind == 0xB0) {
        return 0xB;
    }
    if (kind == 0xE0) {
        return 0xE;
    }
    return 0xF;
}

void onOut(usb_transfer_t* transfer) {
    if (outLock == nullptr) {
        return;
    }
    xSemaphoreTake(outLock, portMAX_DELAY);
    outBusy = false;
    bool more = outCount > 0;
    int count = outCount;
    uint8_t packet[64];
    if (more) {
        if (count > (int)sizeof(packet)) {
            count = sizeof(packet);
        }
        memcpy(packet, outBytes, count);
        memmove(outBytes, outBytes + count, outCount - count);
        outCount -= count;
        outBusy = true;
    }
    xSemaphoreGive(outLock);
    if (more && transfer != nullptr) {
        memcpy(transfer->data_buffer, packet, count);
        transfer->num_bytes = count;
        transfer->bEndpointAddress = outAddress;
        usb_host_transfer_submit(transfer);
    }
}

void onIn(usb_transfer_t* transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 4 <= transfer->actual_num_bytes; i += 4) {
            uint8_t cin = transfer->data_buffer[i] & 0x0F;
            int len = cinLength(cin);
            if (len > 0) {
                pushMidi(transfer->data_buffer + i + 1, len);
            }
        }
    }
    if (deviceReady && inAddress != 0) {
        transfer->num_bytes = inPacket;
        transfer->bEndpointAddress = inAddress;
        usb_host_transfer_submit(transfer);
    }
}

bool findMidi(const usb_config_desc_t* config) {
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(config);
    int total = config->wTotalLength;
    int at = 0;
    bool streaming = false;
    inAddress = 0;
    outAddress = 0;
    while (at + 1 < total) {
        uint8_t len = raw[at];
        uint8_t type = raw[at + 1];
        if (len < 2 || at + len > total) {
            break;
        }
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            streaming = raw[at + 5] == 0x01 && raw[at + 6] == 0x03;
        } else if (streaming && type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7) {
            uint8_t addr = raw[at + 2];
            uint8_t attr = raw[at + 3];
            uint16_t packet = raw[at + 4] | (static_cast<uint16_t>(raw[at + 5]) << 8);
            if ((attr & 0x03) == 0x02) {
                if (addr & 0x80) {
                    inAddress = addr;
                    inPacket = packet == 0 ? 64 : packet;
                } else {
                    outAddress = addr;
                }
            }
        }
        at += len;
    }
    return inAddress != 0;
}

void openDevice(uint8_t address) {
    if (usb_host_device_open(client, address, &device) != ESP_OK) {
        strncpy(statusText, "USB device did not open", sizeof(statusText) - 1);
        return;
    }
    const usb_config_desc_t* config = nullptr;
    if (usb_host_get_active_config_descriptor(device, &config) != ESP_OK || config == nullptr || !findMidi(config)) {
        usb_host_device_close(client, device);
        device = nullptr;
        strncpy(statusText, "USB device has no MIDI interface", sizeof(statusText) - 1);
        return;
    }
    const usb_intf_desc_t* intf = nullptr;
    int offset = 0;
    const usb_standard_desc_t* desc = reinterpret_cast<const usb_standard_desc_t*>(config);
    while ((desc = usb_parse_next_descriptor_of_type(desc, config->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != nullptr) {
        const usb_intf_desc_t* candidate = reinterpret_cast<const usb_intf_desc_t*>(desc);
        if (candidate->bInterfaceClass == 0x01 && candidate->bInterfaceSubClass == 0x03) {
            intf = candidate;
            break;
        }
    }
    if (intf == nullptr || usb_host_interface_claim(client, device, intf->bInterfaceNumber, intf->bAlternateSetting) != ESP_OK) {
        usb_host_device_close(client, device);
        device = nullptr;
        strncpy(statusText, "USB MIDI interface could not be claimed", sizeof(statusText) - 1);
        return;
    }
    if (usb_host_transfer_alloc(inPacket, 0, &inTransfer) != ESP_OK || usb_host_transfer_alloc(64, 0, &outTransfer) != ESP_OK || inTransfer == nullptr || outTransfer == nullptr) {
        usb_host_device_close(client, device);
        device = nullptr;
        strncpy(statusText, "USB MIDI transfers could not be allocated", sizeof(statusText) - 1);
        return;
    }
    inTransfer->device_handle = device;
    inTransfer->bEndpointAddress = inAddress;
    inTransfer->callback = onIn;
    inTransfer->context = nullptr;
    inTransfer->num_bytes = inPacket;
    outTransfer->device_handle = device;
    outTransfer->callback = onOut;
    outTransfer->context = nullptr;
    deviceReady = true;
    usb_host_transfer_submit(inTransfer);
    strncpy(statusText, "USB MIDI device connected", sizeof(statusText) - 1);
}

void onClient(const usb_host_client_event_msg_t* event, void* /*arg*/) {
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        openDevice(event->new_dev.address);
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        deviceReady = false;
        device = nullptr;
        inAddress = 0;
        outAddress = 0;
        strncpy(statusText, "USB MIDI host is on. No MIDI device yet.", sizeof(statusText) - 1);
    }
}

void hostTask(void* /*arg*/) {
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
        usb_host_client_handle_events(client, 0);
    }
}
}

bool UsbMidiHost::start() {
    if (started) {
        return true;
    }
    usb_host_config_t host = {};
    host.skip_phy_setup = false;
    host.intr_flags = ESP_INTR_FLAG_LEVEL1;
    if (usb_host_install(&host) != ESP_OK) {
        return false;
    }
    usb_host_client_config_t config = {};
    config.is_synchronous = false;
    config.max_num_event_msg = 5;
    config.async.client_event_callback = onClient;
    config.async.callback_arg = nullptr;
    if (usb_host_client_register(&config, &client) != ESP_OK) {
        return false;
    }
    outLock = xSemaphoreCreateMutex();
    xTaskCreate(hostTask, "usb-midi-host", 8192, nullptr, 4, nullptr);
    started = true;
    return true;
}

bool UsbMidiHost::running() {
    return started;
}

const char* UsbMidiHost::status() {
    return statusText;
}

void UsbMidiHost::tick(midi::MidiAction* action) {
    if (!started || action == nullptr) {
        return;
    }
    if (!parserReady) {
        parser.begin(action);
        parserReady = true;
    }
    uint8_t local[64];
    int count = 0;
    portENTER_CRITICAL(&queueMux);
    while (queueTail != queueHead && count < (int)sizeof(local)) {
        local[count++] = midiQueue[queueTail];
        queueTail = (queueTail + 1) % (int)sizeof(midiQueue);
    }
    portEXIT_CRITICAL(&queueMux);
    if (count > 0) {
        parser.parse(local, static_cast<uint8_t>(count > 255 ? 255 : count));
    }
}

void UsbMidiHost::write(midi::MidiMessage* msg, int len) {
    if (!started || !deviceReady || outTransfer == nullptr || outLock == nullptr || msg == nullptr || len <= 0 || outAddress == 0) {
        return;
    }
    xSemaphoreTake(outLock, portMAX_DELAY);
    for (int i = 0; i < len && outCount + 4 <= (int)sizeof(outBytes); i++) {
        uint8_t cin = cinFor(msg[i].status);
        outBytes[outCount++] = cin;
        outBytes[outCount++] = msg[i].status;
        outBytes[outCount++] = cinLength(cin) > 1 ? msg[i].arg1 : 0;
        outBytes[outCount++] = cinLength(cin) > 2 ? msg[i].arg2 : 0;
    }
    bool start = !outBusy && outCount > 0;
    int count = 0;
    uint8_t packet[64];
    if (start) {
        count = outCount > (int)sizeof(packet) ? (int)sizeof(packet) : outCount;
        memcpy(packet, outBytes, count);
        memmove(outBytes, outBytes + count, outCount - count);
        outCount -= count;
        outBusy = true;
    }
    xSemaphoreGive(outLock);
    if (start) {
        memcpy(outTransfer->data_buffer, packet, count);
        outTransfer->num_bytes = count;
        outTransfer->bEndpointAddress = outAddress;
        outTransfer->device_handle = device;
        usb_host_transfer_submit(outTransfer);
    }
}
