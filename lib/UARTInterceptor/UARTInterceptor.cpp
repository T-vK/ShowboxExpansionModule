#include "UARTInterceptor.h"

void UARTInterceptor::begin(int u1_rx, int u1_tx, int u2_rx, int u2_tx, uint32_t u1_baud, uint32_t u2_baud, size_t u1_max_packet_size, size_t u2_max_packet_size) {
    Serial1.begin(u1_baud, SERIAL_8N1, u1_rx, u1_tx);
    Serial2.begin(u2_baud, SERIAL_8N1, u2_rx, u2_tx);
    buffer1.reserve(u1_max_packet_size);
    buffer2.reserve(u2_max_packet_size);
}

void UARTInterceptor::setPacketHandler(PacketHandlerCallback callback) {
    packetHandler = callback;
}

void UARTInterceptor::setPacketFinder(PacketFinderCallback callback) {
    packetFinder = callback;
}

void UARTInterceptor::setPacketFinderStartEndSig(uint8_t* start_sig, size_t start_sig_length, uint8_t* end_sig, size_t end_sig_length) {
    startSignature.assign(start_sig, start_sig + start_sig_length);
    endSignature.assign(end_sig, end_sig + end_sig_length);

    packetFinder = [this](uint8_t* packetBuffer, size_t* packetBufferPosition, uint8_t* newBytes, size_t* newBytesLength, std::function<void()> sendPacketCallback) {
        static size_t startSigMatchIndex = 0;
        static bool packetInProgress = false;

        for (size_t i = 0; i < *newBytesLength; ++i) {
            if (!packetInProgress) {
                // Look for start signature
                if (newBytes[i] == startSignature[startSigMatchIndex]) {
                    startSigMatchIndex++;
                    if (startSigMatchIndex == startSignature.size()) {
                        // Start signature matched completely
                        packetInProgress = true;
                        startSigMatchIndex = 0;
                        // Add the start signature to the buffer
                        for (size_t j = 0; j < startSignature.size(); ++j) {
                            packetBuffer[*packetBufferPosition] = startSignature[j];
                            (*packetBufferPosition)++;
                        }
                    }
                } else {
                    startSigMatchIndex = 0; // Reset match index if any byte does not match
                }
            } else {
                // Collect packet bytes
                packetBuffer[*packetBufferPosition] = newBytes[i];
                (*packetBufferPosition)++;

                // Check if end signature is matched
                if (*packetBufferPosition >= endSignature.size()) {
                    if (memcmp(&packetBuffer[*packetBufferPosition - endSignature.size()], endSignature.data(), endSignature.size()) == 0) {
                        // End signature matched
                        size_t packetLength = *packetBufferPosition;
                        sendPacketCallback();
                        
                        *packetBufferPosition = 0; // Reset buffer position for the next packet
                        packetInProgress = false;
                    }
                }
            }
        }
    };
}

void UARTInterceptor::sendPacket(uint8_t* packet, size_t length, Direction direction) {
    if (direction == DEVICE2_TO_DEVICE1) {
        Serial1.write(packet, length);
    } else if (direction == DEVICE1_TO_DEVICE2) {
        Serial2.write(packet, length);
    }
}

void UARTInterceptor::tick() {
    _processSerial(Serial1, buffer1, buffer1Position, DEVICE1_TO_DEVICE2);
    _processSerial(Serial2, buffer2, buffer2Position, DEVICE2_TO_DEVICE1);
}

void UARTInterceptor::_processSerial(HardwareSerial& serial, std::vector<uint8_t>& buffer, size_t& bufferPosition, Direction direction) {
    if (serial.available()) {
        // Determine the number of bytes available
        size_t availableBytes = serial.available();
        
        // Allocate buffer dynamically based on available bytes
        std::vector<uint8_t> newBytes(availableBytes);
        size_t newBytesLength = serial.readBytes(newBytes.data(), availableBytes);
        
        // Call packetFinder with the dynamically allocated buffer
        packetFinder(buffer.data(), &bufferPosition, newBytes.data(), &newBytesLength, [&]() {
            PacketHandlerResult result = packetHandler(buffer.data(), bufferPosition, direction);
            
            if (result == PACKET_NOT_MODIFIED || result == PACKET_MODIFIED) {
                _sendPacketFromBuffer(direction);
            }
            // If result is PACKET_DROP, drop the packet
        });
    }
}


void UARTInterceptor::_sendPacketFromBuffer(Direction direction) {
    std::vector<uint8_t>& buffer = direction == DEVICE1_TO_DEVICE2 ? buffer1 : buffer2;
    size_t& bufferPosition = direction == DEVICE1_TO_DEVICE2 ? buffer1Position : buffer2Position;

    if (bufferPosition > 0) {
        sendPacket(buffer.data(), bufferPosition, direction);
        bufferPosition = 0; // Reset buffer position after sending
    }
}

#ifdef DEBUG_UART_INTERCEPTOR
void UARTInterceptor::debugPrint(const char* msg) {
    Serial.print(msg);
}

void UARTInterceptor::debugPrintPacket(uint8_t* packet, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        Serial.printf("%02X ", packet[i]);
    }
    Serial.println();
}
#endif
