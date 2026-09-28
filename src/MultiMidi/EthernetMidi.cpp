#include "EthernetMidi.h"

#include "MidiAction.h"
#include "MidiCommon.h"
#include "MidiParser.h"
#include "pin_definitions.h"
#include <SPI.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstring>

namespace {
constexpr uint16_t kControlPort = 5004;
constexpr uint16_t kDataPort = 5005;
constexpr uint16_t kBufferMask = 0x07FF;
constexpr int kDhcpSocket = 0;
constexpr int kControlSocket = 1;
constexpr int kDataSocket = 2;

constexpr uint8_t SN_MR = 0x00;
constexpr uint8_t SN_CR = 0x01;
constexpr uint8_t SN_IR = 0x02;
constexpr uint8_t SN_SR = 0x03;
constexpr uint8_t SN_PORT = 0x04;
constexpr uint8_t SN_DHAR = 0x06;
constexpr uint8_t SN_DIPR = 0x0C;
constexpr uint8_t SN_DPORT = 0x10;
constexpr uint8_t SN_TX_FSR = 0x20;
constexpr uint8_t SN_TX_WR = 0x24;
constexpr uint8_t SN_RX_RSR = 0x26;
constexpr uint8_t SN_RX_RD = 0x28;
constexpr uint8_t SN_MR_UDP = 0x02;
constexpr uint8_t SN_CR_OPEN = 0x01;
constexpr uint8_t SN_CR_CLOSE = 0x10;
constexpr uint8_t SN_CR_SEND = 0x20;
constexpr uint8_t SN_CR_RECV = 0x40;
constexpr uint8_t SN_SR_CLOSED = 0x00;
constexpr uint8_t SN_SR_UDP = 0x22;
constexpr uint8_t SN_IR_TIMEOUT = 0x08;
constexpr uint8_t SN_IR_SENDOK = 0x10;

constexpr int kDhcpIdle = 0;
constexpr int kDhcpWaitOffer = 1;
constexpr int kDhcpWaitAck = 2;
constexpr int kDhcpBound = 3;

SemaphoreHandle_t spiLock = nullptr;
midi::MidiParser midiParser;
bool parserReady = false;

uint8_t socketBlock(int sock) { return static_cast<uint8_t>((sock * 4) + 1); }
uint8_t txBlock(int sock) { return static_cast<uint8_t>((sock * 4) + 2); }
uint8_t rxBlock(int sock) { return static_cast<uint8_t>((sock * 4) + 3); }

void lockSpi() {
    if (spiLock == nullptr) {
        spiLock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(spiLock, portMAX_DELAY);
}

void unlockSpi() {
    xSemaphoreGiveRecursive(spiLock);
}

void transfer(uint16_t addr, uint8_t block, bool write, uint8_t* data, uint16_t len) {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(W5500_nSS, LOW);
    SPI.transfer(addr >> 8);
    SPI.transfer(addr & 0xFF);
    SPI.transfer(static_cast<uint8_t>((block << 3) | (write ? 0x04 : 0x00)));
    for (uint16_t i = 0; i < len; i++) {
        uint8_t next = write ? data[i] : 0;
        uint8_t got = SPI.transfer(next);
        if (!write) {
            data[i] = got;
        }
    }
    digitalWrite(W5500_nSS, HIGH);
    SPI.endTransaction();
}

uint8_t read8(uint8_t block, uint16_t addr) {
    uint8_t value = 0;
    transfer(addr, block, false, &value, 1);
    return value;
}

void write8(uint8_t block, uint16_t addr, uint8_t value) {
    transfer(addr, block, true, &value, 1);
}

uint16_t read16(uint8_t block, uint16_t addr) {
    uint8_t raw[2] = {};
    transfer(addr, block, false, raw, 2);
    return (static_cast<uint16_t>(raw[0]) << 8) | raw[1];
}

void write16(uint8_t block, uint16_t addr, uint16_t value) {
    uint8_t raw[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    transfer(addr, block, true, raw, 2);
}

void writeBuf(uint8_t block, uint16_t addr, const uint8_t* data, uint16_t len) {
    transfer(addr, block, true, const_cast<uint8_t*>(data), len);
}

void readBuf(uint8_t block, uint16_t addr, uint8_t* data, uint16_t len) {
    transfer(addr, block, false, data, len);
}

void command(int sock, uint8_t cmd) {
    uint8_t block = socketBlock(sock);
    write8(block, SN_CR, cmd);
    for (int i = 0; i < 20; i++) {
        if (read8(block, SN_CR) == 0) {
            return;
        }
        delay(1);
    }
}

bool waitStatus(int sock, uint8_t status) {
    uint8_t block = socketBlock(sock);
    for (int i = 0; i < 30; i++) {
        if (read8(block, SN_SR) == status) {
            return true;
        }
        delay(1);
    }
    return false;
}

void put32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

uint32_t get32(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
        (static_cast<uint32_t>(src[2]) << 8) | src[3];
}

void copyWrapped(uint8_t block, uint16_t offset, uint8_t* data, uint16_t len, bool write) {
    uint16_t start = offset & kBufferMask;
    uint16_t first = (kBufferMask + 1) - start;
    if (first > len) {
        first = len;
    }
    if (write) {
        writeBuf(block, start, data, first);
        if (len > first) {
            writeBuf(block, 0, data + first, len - first);
        }
    } else {
        readBuf(block, start, data, first);
        if (len > first) {
            readBuf(block, 0, data + first, len - first);
        }
    }
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

void EthernetMidi::setDebug(Print* serial) {
    Debug = serial;
}

void EthernetMidi::setEnabled(bool enabled) {
    enabledFlag = enabled;
    if (!enabled) {
        setStatus("Off");
    }
}

void EthernetMidi::setStatus(const char* text) {
    strncpy(statusText, text, sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
}

void EthernetMidi::tick(midi::MidiAction* action) {
    if (!enabledFlag) {
        return;
    }
    if (!chipUp) {
        startChip();
        return;
    }
    struct Pending {
        uint8_t data[256];
        int length;
        uint8_t from[4];
        uint16_t port;
        bool dataPort;
        bool sessionPacket;
    };
    static Pending pending[6];
    int pendingCount = 0;
    lockSpi();
    pollLink();
    if (linkUp && !haveIp) {
        pollDhcp();
    } else if (linkUp && haveIp && renewAt != 0 && (int32_t)(millis() - renewAt) > 0) {
        dhcpState = kDhcpIdle;
        haveIp = false;
        session = false;
        socketsOpen = false;
        setStatus("Renewing DHCP");
    }
    if (haveIp && !socketsOpen) {
        openMidiSockets();
    }
    if (socketsOpen) {
        for (int sockIndex = 0; sockIndex < 2 && pendingCount < 6; sockIndex++) {
            int sock = sockIndex == 0 ? kControlSocket : kDataSocket;
            bool dataPortPacket = sockIndex == 1;
            for (int n = 0; n < 3 && pendingCount < 6; n++) {
                Pending& item = pending[pendingCount];
                item.length = recvFrom(sock, item.from, &item.port, item.data, sizeof(item.data));
                if (item.length <= 0) {
                    break;
                }
                item.dataPort = dataPortPacket;
                item.sessionPacket = item.length >= 4 && item.data[0] == 0xFF && item.data[1] == 0xFF;
                pendingCount++;
            }
        }
    }
    unlockSpi();
    for (int i = 0; i < pendingCount; i++) {
        if (pending[i].sessionPacket) {
            handleSession(pending[i].data, pending[i].length, pending[i].from, pending[i].port, pending[i].dataPort);
        } else if (pending[i].dataPort) {
            handleMidi(pending[i].data, pending[i].length, action);
        }
    }
}

void EthernetMidi::startChip() {
    if (chipTriedAt != 0 && millis() - chipTriedAt < 5000) {
        return;
    }
    chipTriedAt = millis();
    setStatus("Starting the W5500");
    pinMode(W5500_nSS, OUTPUT);
    digitalWrite(W5500_nSS, HIGH);
    pinMode(W5500_nRESET, OUTPUT);
    digitalWrite(W5500_nRESET, HIGH);
    delay(2);
    digitalWrite(W5500_nRESET, LOW);
    delay(10);
    digitalWrite(W5500_nRESET, HIGH);
    delay(50);
    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI, W5500_nSS);

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        mac[0] = 0x02;
        mac[1] = 0x53;
        mac[2] = 0x58;
        mac[3] = 0x4D;
        mac[4] = 0x00;
        mac[5] = 0x31;
    }
    mac[0] = (mac[0] | 0x02) & 0xFE;

    lockSpi();
    write8(0, 0x0000, 0x80);
    delay(10);
    uint8_t version = read8(0, 0x0039);
    if (version != 0x04) {
        version = read8(0, 0x0039);
    }
    if (version == 0x04) {
        writeBuf(0, 0x0009, mac, 6);
        chipUp = true;
        setStatus("W5500 is up, waiting for a link");
        if (Debug != nullptr) {
            Debug->println("W5500 answered");
        }
    } else {
        setStatus("The W5500 did not answer on SPI");
        if (Debug != nullptr) {
            Debug->printf("W5500 version 0x%02X\n", version);
        }
    }
    unlockSpi();
}

void EthernetMidi::pollLink() {
    bool up = (read8(0, 0x002E) & 0x01) != 0;
    if (up == linkUp) {
        return;
    }
    linkUp = up;
    if (!linkUp) {
        haveIp = false;
        socketsOpen = false;
        session = false;
        controlPort = 0;
        dataPort = 0;
        dhcpState = kDhcpIdle;
        command(kDhcpSocket, SN_CR_CLOSE);
        command(kControlSocket, SN_CR_CLOSE);
        command(kDataSocket, SN_CR_CLOSE);
        setStatus("W5500 is up, waiting for a link");
        return;
    }
    dhcpState = kDhcpIdle;
    setStatus("Waiting for DHCP");
}

void EthernetMidi::pollDhcp() {
    uint8_t packet[320];
    uint8_t from[4];
    uint16_t fromPort = 0;
    if (dhcpState == kDhcpIdle) {
        command(kDhcpSocket, SN_CR_CLOSE);
        waitStatus(kDhcpSocket, SN_SR_CLOSED);
        write8(socketBlock(kDhcpSocket), SN_MR, SN_MR_UDP);
        write16(socketBlock(kDhcpSocket), SN_PORT, 68);
        command(kDhcpSocket, SN_CR_OPEN);
        if (!waitStatus(kDhcpSocket, SN_SR_UDP)) {
            setStatus("DHCP socket did not open");
            return;
        }
        memset(packet, 0, 240);
        packet[0] = 1;
        packet[1] = 1;
        packet[2] = 6;
        dhcpXid = esp_random();
        put32(packet + 4, dhcpXid);
        packet[10] = 0x80;
        memcpy(packet + 28, mac, 6);
        packet[236] = 99;
        packet[237] = 130;
        packet[238] = 83;
        packet[239] = 99;
        int opt = 240;
        packet[opt++] = 53;
        packet[opt++] = 1;
        packet[opt++] = 1;
        packet[opt++] = 61;
        packet[opt++] = 7;
        packet[opt++] = 1;
        memcpy(packet + opt, mac, 6);
        opt += 6;
        packet[opt++] = 55;
        packet[opt++] = 3;
        packet[opt++] = 1;
        packet[opt++] = 3;
        packet[opt++] = 6;
        packet[opt++] = 255;
        uint8_t broadcast[4] = {255, 255, 255, 255};
        uint8_t macBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        writeBuf(socketBlock(kDhcpSocket), SN_DHAR, macBroadcast, 6);
        if (sendTo(kDhcpSocket, broadcast, 67, packet, opt)) {
            dhcpState = kDhcpWaitOffer;
            dhcpDeadline = millis() + 3000;
            setStatus("Waiting for DHCP");
        }
        return;
    }

    int got = recvFrom(kDhcpSocket, from, &fromPort, packet, sizeof(packet));
    if (got >= 240 && packet[0] == 2 && get32(packet + 4) == dhcpXid &&
        packet[236] == 99 && packet[237] == 130 && packet[238] == 83 && packet[239] == 99) {
        uint8_t msgType = 0;
        int i = 240;
        while (i < got) {
            uint8_t code = packet[i++];
            if (code == 255) {
                break;
            }
            if (code == 0 || i >= got) {
                continue;
            }
            uint8_t optLen = packet[i++];
            if (i + optLen > got) {
                break;
            }
            if (code == 53 && optLen >= 1) {
                msgType = packet[i];
            } else if (code == 54 && optLen >= 4) {
                memcpy(dhcpServer, packet + i, 4);
            } else if (code == 1 && optLen >= 4) {
                memcpy(mask, packet + i, 4);
            } else if (code == 3 && optLen >= 4) {
                memcpy(gateway, packet + i, 4);
            } else if (code == 51 && optLen >= 4) {
                uint32_t seconds = get32(packet + i);
                leaseMs = seconds > 120 ? seconds * 1000UL : 120000UL;
            }
            i += optLen;
        }
        memcpy(offeredIp, packet + 16, 4);
        if (msgType == 2 && dhcpState == kDhcpWaitOffer) {
            memset(packet, 0, 240);
            packet[0] = 1;
            packet[1] = 1;
            packet[2] = 6;
            put32(packet + 4, dhcpXid);
            packet[10] = 0x80;
            memcpy(packet + 28, mac, 6);
            packet[236] = 99;
            packet[237] = 130;
            packet[238] = 83;
            packet[239] = 99;
            int opt = 240;
            packet[opt++] = 53;
            packet[opt++] = 1;
            packet[opt++] = 3;
            packet[opt++] = 50;
            packet[opt++] = 4;
            memcpy(packet + opt, offeredIp, 4);
            opt += 4;
            packet[opt++] = 54;
            packet[opt++] = 4;
            memcpy(packet + opt, dhcpServer, 4);
            opt += 4;
            packet[opt++] = 61;
            packet[opt++] = 7;
            packet[opt++] = 1;
            memcpy(packet + opt, mac, 6);
            opt += 6;
            packet[opt++] = 255;
            uint8_t broadcast[4] = {255, 255, 255, 255};
            if (sendTo(kDhcpSocket, broadcast, 67, packet, opt)) {
                dhcpState = kDhcpWaitAck;
                dhcpDeadline = millis() + 3000;
            }
            return;
        }
        if (msgType == 5 && dhcpState == kDhcpWaitAck) {
            memcpy(ip, offeredIp, 4);
            if (mask[0] == 0) {
                mask[0] = 255;
                mask[1] = 255;
                mask[2] = 255;
                mask[3] = 0;
            }
            if (gateway[0] == 0) {
                memcpy(gateway, ip, 4);
                gateway[3] = 1;
            }
            writeBuf(0, 0x0001, gateway, 4);
            writeBuf(0, 0x0005, mask, 4);
            writeBuf(0, 0x000F, ip, 4);
            haveIp = true;
            dhcpState = kDhcpBound;
            renewAt = millis() + (leaseMs / 2);
            char note[96];
            snprintf(note, sizeof(note), "%u.%u.%u.%u, ports 5004 and 5005, no session",
                ip[0], ip[1], ip[2], ip[3]);
            setStatus(note);
            if (Debug != nullptr) {
                Debug->println(note);
            }
            return;
        }
        if (msgType == 6) {
            dhcpState = kDhcpIdle;
            setStatus("DHCP refused the request");
            return;
        }
    }
    if ((int32_t)(millis() - dhcpDeadline) > 0) {
        dhcpState = kDhcpIdle;
        setStatus("Waiting for DHCP");
    }
}

void EthernetMidi::openMidiSockets() {
    command(kControlSocket, SN_CR_CLOSE);
    command(kDataSocket, SN_CR_CLOSE);
    waitStatus(kControlSocket, SN_SR_CLOSED);
    write8(socketBlock(kControlSocket), SN_MR, SN_MR_UDP);
    write16(socketBlock(kControlSocket), SN_PORT, kControlPort);
    command(kControlSocket, SN_CR_OPEN);
    write8(socketBlock(kDataSocket), SN_MR, SN_MR_UDP);
    write16(socketBlock(kDataSocket), SN_PORT, kDataPort);
    command(kDataSocket, SN_CR_OPEN);
    socketsOpen = waitStatus(kControlSocket, SN_SR_UDP) && waitStatus(kDataSocket, SN_SR_UDP);
    if (!socketsOpen) {
        setStatus("MIDI sockets did not open");
    }
}

void EthernetMidi::handleSession(const uint8_t* data, int length, const uint8_t remote[4], uint16_t port, bool dataPortPacket) {
    if (length < 4) {
        return;
    }
    uint16_t cmd = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    int sock = dataPortPacket ? kDataSocket : kControlSocket;
    if (cmd == 0x494E && length >= 16) {
        uint32_t token = get32(data + 8);
        memcpy(peerIp, remote, 4);
        if (dataPortPacket) {
            dataPort = port;
        } else {
            controlPort = port;
        }
        session = controlPort != 0 && dataPort != 0;
        uint8_t reply[16] = {0xFF, 0xFF, 0x4F, 0x4B, 0, 0, 0, 2};
        put32(reply + 8, token);
        put32(reply + 12, ourSsrc);
        sendTo(sock, remote, port, reply, sizeof(reply));
        if (session) {
            char note[96];
            snprintf(note, sizeof(note), "%u.%u.%u.%u, session open", ip[0], ip[1], ip[2], ip[3]);
            setStatus(note);
        }
        return;
    }
    if (cmd == 0x4259) {
        session = false;
        controlPort = 0;
        dataPort = 0;
        char note[96];
        snprintf(note, sizeof(note), "%u.%u.%u.%u, ports 5004 and 5005, no session",
            ip[0], ip[1], ip[2], ip[3]);
        setStatus(note);
        return;
    }
    if (cmd == 0x434B && length >= 36) {
        uint8_t count = data[8];
        if (count > 1) {
            return;
        }
        uint8_t reply[36];
        memcpy(reply, data, 36);
        reply[8] = static_cast<uint8_t>(count + 1);
        uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 100);
        int stampAt = count == 0 ? 20 : 28;
        for (int i = 7; i >= 0; i--) {
            reply[stampAt + i] = static_cast<uint8_t>(now);
            now >>= 8;
        }
        sendTo(sock, remote, port, reply, sizeof(reply));
    }
}

void EthernetMidi::handleMidi(const uint8_t* data, int length, midi::MidiAction* action) {
    if (action == nullptr || length < 13 || (data[0] & 0x80) == 0) {
        return;
    }
    if (!parserReady) {
        midiParser.begin(action);
        parserReady = true;
    }
    int count = data[12] & 0x0F;
    if (count <= 0 || 13 + count > length) {
        return;
    }
    uint8_t body[16];
    if (count > static_cast<int>(sizeof(body))) {
        count = sizeof(body);
    }
    memcpy(body, data + 13, count);
    midiParser.parse(body, static_cast<uint8_t>(count));
}

void EthernetMidi::write(midi::MidiMessage* msg, int len) {
    if (!enabledFlag || !session || dataPort == 0 || msg == nullptr || len <= 0) {
        return;
    }
    lockSpi();
    for (int i = 0; i < len; i++) {
        int count = messageLength(msg[i].status);
        uint8_t packet[16] = {};
        packet[0] = 0x80;
        packet[1] = 0x61;
        packet[2] = static_cast<uint8_t>(sequence >> 8);
        packet[3] = static_cast<uint8_t>(sequence);
        sequence++;
        put32(packet + 4, millis());
        put32(packet + 8, ourSsrc);
        packet[12] = static_cast<uint8_t>(count);
        packet[13] = msg[i].status;
        if (count > 1) {
            packet[14] = msg[i].arg1;
        }
        if (count > 2) {
            packet[15] = msg[i].arg2;
        }
        sendTo(kDataSocket, peerIp, dataPort, packet, 13 + count);
    }
    unlockSpi();
}

bool EthernetMidi::sendTo(int sock, const uint8_t dest[4], uint16_t port, const uint8_t* data, int len) {
    if (len <= 0 || len > 512) {
        return false;
    }
    lockSpi();
    uint8_t block = socketBlock(sock);
    uint16_t freeBytes = read16(block, SN_TX_FSR);
    if (freeBytes < len) {
        unlockSpi();
        return false;
    }
    uint16_t wr = read16(block, SN_TX_WR);
    copyWrapped(txBlock(sock), wr, const_cast<uint8_t*>(data), static_cast<uint16_t>(len), true);
    write16(block, SN_TX_WR, static_cast<uint16_t>(wr + len));
    writeBuf(block, SN_DIPR, dest, 4);
    write16(block, SN_DPORT, port);
    command(sock, SN_CR_SEND);
    for (int i = 0; i < 40; i++) {
        uint8_t ir = read8(block, SN_IR);
        if (ir & SN_IR_SENDOK) {
            write8(block, SN_IR, SN_IR_SENDOK);
            unlockSpi();
            return true;
        }
        if (ir & SN_IR_TIMEOUT) {
            write8(block, SN_IR, SN_IR_TIMEOUT);
            unlockSpi();
            return false;
        }
        delay(1);
    }
    unlockSpi();
    return false;
}

int EthernetMidi::recvFrom(int sock, uint8_t from[4], uint16_t* port, uint8_t* data, int maxLen) {
    lockSpi();
    uint8_t block = socketBlock(sock);
    uint16_t available = read16(block, SN_RX_RSR);
    if (available < 8) {
        unlockSpi();
        return 0;
    }
    uint16_t rd = read16(block, SN_RX_RD);
    uint8_t head[8];
    copyWrapped(rxBlock(sock), rd, head, 8, false);
    uint16_t size = (static_cast<uint16_t>(head[6]) << 8) | head[7];
    if (size > 512 || available < static_cast<uint16_t>(8 + size)) {
        write16(block, SN_RX_RD, static_cast<uint16_t>(rd + available));
        command(sock, SN_CR_RECV);
        unlockSpi();
        return 0;
    }
    int copy = size;
    if (copy > maxLen) {
        copy = maxLen;
    }
    if (copy > 0) {
        copyWrapped(rxBlock(sock), static_cast<uint16_t>(rd + 8), data, static_cast<uint16_t>(copy), false);
    }
    write16(block, SN_RX_RD, static_cast<uint16_t>(rd + 8 + size));
    command(sock, SN_CR_RECV);
    memcpy(from, head, 4);
    *port = (static_cast<uint16_t>(head[4]) << 8) | head[5];
    unlockSpi();
    return copy;
}
