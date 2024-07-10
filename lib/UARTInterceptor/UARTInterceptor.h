#ifndef UARTINTERCEPTOR_H
#define UARTINTERCEPTOR_H

#include <Arduino.h>

class UARTInterceptor {
public:
    // Enum for directions of packet injection
    enum class Direction {
        TO_A,  // Inject packets to UART A
        TO_B,  // Inject packets to UART B
        BOTH   // Inject packets to both UARTs
    };

    UARTInterceptor(uint8_t uartARX, uint8_t uartATX, uint8_t uartBRX, uint8_t uartBTX);
    
    void begin(unsigned long baudA, unsigned long baudB);
    
    // Configure the start and end bytes
    void setStartBytes(uint8_t startByte1, uint8_t startByte2);
    void setEndBytes(uint8_t endByte1, uint8_t endByte2);

    void setPacketHandler(void (*handler)(uint8_t* packet, size_t& length, Direction direction));
    void setPacketInjector(void (*injector)(const uint8_t* packet, size_t length, Direction direction));
    void loop();
    void injectPacket(const uint8_t* packet, size_t length, Direction direction);

private:
    // UART pins
    uint8_t _uartARX, _uartATX, _uartBRX, _uartBTX;

    // UART handles
    HardwareSerial _uartA, _uartB;

    // Packet handling
    void (*_packetHandler)(uint8_t* packet, size_t& length, Direction direction) = nullptr;
    void (*_packetInjector)(const uint8_t* packet, size_t length, Direction direction) = nullptr;

    // Default start and end byte sequences
    uint8_t _startByte1 = 0xBE;
    uint8_t _startByte2 = 0xEF;
    uint8_t _endByte1 = 0xEF;
    uint8_t _endByte2 = 0xBE;

    void parsePacket(HardwareSerial& serial, uint8_t* buffer, size_t& bufferIndex, Direction direction);
    void handleUartA();
    void handleUartB();

    uint8_t _uartABuffer[256];
    uint8_t _uartBBuffer[256];
    size_t _uartABufferIndex = 0;
    size_t _uartBBufferIndex = 0;

    bool _isInjecting = false;
    Direction _injectDirection;
    uint8_t _injectBuffer[256];
    size_t _injectBufferLength = 0;
};

#endif // UARTINTERCEPTOR_H
