#ifndef UART_INTERCEPTOR_H
#define UART_INTERCEPTOR_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <functional>
#include <vector>

// Define DEBUG_UART_INTERCEPTOR to enable debug output
//#define DEBUG_UART_INTERCEPTOR

class UARTInterceptor {
public:
    enum Direction {
        DEVICE1_TO_DEVICE2 = 1,
        DEVICE2_TO_DEVICE1 = 2
    };

    enum PacketHandlerResult {
        PACKET_NOT_MODIFIED = 0,
        PACKET_MODIFIED = 1,
        PACKET_DROP = -1
    };

    using PacketHandlerCallback = std::function<PacketHandlerResult(uint8_t* packet, size_t& length, Direction direction)>;
    using PacketFinderCallback = std::function<void(uint8_t* packetBuffer, size_t* packetBufferPosition, uint8_t* newBytes, size_t* newBytesLength, std::function<void()> sendPacketCallback)>;

    void begin(int u1_rx, int u1_tx, int u2_rx, int u2_tx, uint32_t u1_baud, uint32_t u2_baud, size_t u1_max_packet_size, size_t u2_max_packet_size);
    void setPacketHandler(PacketHandlerCallback callback);
    void setPacketFinder(PacketFinderCallback callback);
    void setPacketFinderStartEndSig(uint8_t* start_sig, size_t start_sig_length, uint8_t* end_sig, size_t end_sig_length);
    void sendPacket(uint8_t* packet, size_t length, Direction direction);
    void tick();

private:
    HardwareSerial Serial1 = HardwareSerial(1);
    HardwareSerial Serial2 = HardwareSerial(2);

    std::vector<uint8_t> buffer1;
    std::vector<uint8_t> buffer2;
    size_t buffer1Position = 0;
    size_t buffer2Position = 0;
    std::vector<uint8_t> startSignature;
    std::vector<uint8_t> endSignature;
    PacketHandlerCallback packetHandler;
    PacketFinderCallback packetFinder;

    void _processSerial(HardwareSerial& serial, std::vector<uint8_t>& buffer, size_t& bufferPosition, Direction direction);
    void _sendPacketFromBuffer(Direction direction);

#ifdef DEBUG_UART_INTERCEPTOR
    void debugPrint(const char* msg);
    void debugPrintPacket(uint8_t* packet, size_t length);
#endif
};

#endif // UART_INTERCEPTOR_H
