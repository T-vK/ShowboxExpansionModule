#include "UARTInterceptor.h"

// Constructor
UARTInterceptor::UARTInterceptor(uint8_t uartARX, uint8_t uartATX, uint8_t uartBRX, uint8_t uartBTX)
    : _uartARX(uartARX), _uartATX(uartATX), _uartBRX(uartBRX), _uartBTX(uartBTX),
      _uartA(1), _uartB(2) {}

// Initialize UARTs
void UARTInterceptor::begin(unsigned long baudA, unsigned long baudB) {
    _uartA.begin(baudA, SERIAL_8N1, _uartARX, _uartATX);
    _uartB.begin(baudB, SERIAL_8N1, _uartBRX, _uartBTX);

    // Attach interrupt handlers
    _uartA.onReceive([this]() { handleUartA(); });
    _uartB.onReceive([this]() { handleUartB(); });
}

// Set start and end byte sequences
void UARTInterceptor::setStartBytes(uint8_t startByte1, uint8_t startByte2) {
    _startByte1 = startByte1;
    _startByte2 = startByte2;
}

void UARTInterceptor::setEndBytes(uint8_t endByte1, uint8_t endByte2) {
    _endByte1 = endByte1;
    _endByte2 = endByte2;
}

// Set the packet handler callback
void UARTInterceptor::setPacketHandler(PacketHandler handler, void* context) {
    _packetHandler = handler;
    _packetHandlerContext = context;
}

// Main loop
void UARTInterceptor::loop() {
    if (_isInjecting && _uartA.availableForWrite()) {
        if (_injectDirection == Direction::TO_A || _injectDirection == Direction::BOTH) {
            _uartA.write(_injectBuffer, _injectBufferLength);
        }
        if (_injectDirection == Direction::TO_B || _injectDirection == Direction::BOTH) {
            _uartB.write(_injectBuffer, _injectBufferLength);
        }
        _isInjecting = false;
        _injectBufferLength = 0;
    }
}

// Handle incoming data from UART A
void UARTInterceptor::handleUartA() {
    parsePacket(_uartA, _uartABuffer, _uartABufferIndex, Direction::TO_B);
}

// Handle incoming data from UART B
void UARTInterceptor::handleUartB() {
    parsePacket(_uartB, _uartBBuffer, _uartBBufferIndex, Direction::TO_A);
}

// Parse packets and handle start/end byte sequences
void UARTInterceptor::parsePacket(HardwareSerial& serial, uint8_t* buffer, size_t& bufferIndex, Direction direction) {
    while (serial.available()) {
        uint8_t byte = serial.read();

        if (bufferIndex == 0 && byte == _startByte1) {
            buffer[bufferIndex++] = byte;
        } else if (bufferIndex == 1 && byte == _startByte2) {
            buffer[bufferIndex++] = byte;
        } else if (bufferIndex > 1) {
            buffer[bufferIndex++] = byte;

            if (bufferIndex >= 4 && buffer[bufferIndex - 2] == _endByte1 && buffer[bufferIndex - 1] == _endByte2) {
                size_t length = bufferIndex;
                bool drop = false;
                if (_packetHandler) {
                    drop = _packetHandler(_packetHandlerContext, buffer, length, direction);
                }

                // Always forward the packet even if it was not modified, unless it should get dropped
                if (!drop) {
                    if (direction == Direction::TO_B) {
                        _uartB.write(buffer, length);
                    } else if (direction == Direction::TO_A) {
                        _uartA.write(buffer, length);
                    } else if (direction == Direction::BOTH) {
                        _uartA.write(buffer, length);
                        _uartB.write(buffer, length);
                    }
                }

                bufferIndex = 0; // Reset buffer index
            } else if (bufferIndex >= 256) {
                bufferIndex = 0; // Reset buffer index for oversized packets
            }
        }
    }
}

// Inject a packet into the specified UART direction
void UARTInterceptor::injectPacket(const uint8_t* packet, size_t length, Direction direction) {
    if (length > 256) {
        return; // Packet is too large
    }

    memcpy(_injectBuffer, packet, length);
    _injectBufferLength = length;
    _injectDirection = direction;
    _isInjecting = true;
}