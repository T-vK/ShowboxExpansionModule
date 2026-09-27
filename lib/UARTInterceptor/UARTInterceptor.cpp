#include "UARTInterceptor.h"

void UARTInterceptor::begin(int u1_rx, int u1_tx, int u2_rx, int u2_tx, uint32_t u1_baud, uint32_t u2_baud, size_t u1_max_packet_size, size_t u2_max_packet_size) {
    Serial1.begin(u1_baud, SERIAL_8N1, u1_rx, u1_tx);
    Serial2.begin(u2_baud, SERIAL_8N1, u2_rx, u2_tx);
    buffer1MaxSize = u1_max_packet_size;
    buffer2MaxSize = u2_max_packet_size;
    // resize, not reserve: the finder writes through data() and needs real elements
    buffer1.assign(u1_max_packet_size, 0);
    buffer2.assign(u2_max_packet_size, 0);
    buffer1Position = 0;
    buffer2Position = 0;
    framerState1 = FramerState();
    framerState2 = FramerState();
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
        if (activeFramer == nullptr || activeMaxPacketSize == 0 || startSignature.empty() || endSignature.empty()) {
            return;
        }

        FramerState& framer = *activeFramer;
        size_t i = 0;
        while (i < *newBytesLength) {
            uint8_t byte = newBytes[i];
            bool reprocess = false;

            if (!framer.packetInProgress) {
                if (byte == startSignature[framer.startSigMatchIndex]) {
                    framer.startSigMatchIndex++;
                    if (framer.startSigMatchIndex == startSignature.size()) {
                        framer.packetInProgress = true;
                        framer.startSigMatchIndex = 0;
                        *packetBufferPosition = 0;
                        if (startSignature.size() > activeMaxPacketSize) {
                            framer.packetInProgress = false;
                        } else {
                            for (size_t j = 0; j < startSignature.size(); ++j) {
                                packetBuffer[*packetBufferPosition] = startSignature[j];
                                (*packetBufferPosition)++;
                            }
                        }
                    }
                } else {
                    // The rejected byte may itself be the first byte of a new start signature (e.g. BE BE EF).
                    framer.startSigMatchIndex = 0;
                    if (startSignature.size() > 1 && byte == startSignature[0]) {
                        framer.startSigMatchIndex = 1;
                    }
                }
            } else if (*packetBufferPosition >= activeMaxPacketSize) {
                // No end signature before the buffer filled. Drop the partial packet and resync on this byte.
                *packetBufferPosition = 0;
                framer.packetInProgress = false;
                framer.startSigMatchIndex = 0;
                reprocess = true;
            } else {
                packetBuffer[*packetBufferPosition] = byte;
                (*packetBufferPosition)++;

                if (*packetBufferPosition >= endSignature.size() &&
                    memcmp(&packetBuffer[*packetBufferPosition - endSignature.size()], endSignature.data(), endSignature.size()) == 0) {
                    sendPacketCallback();
                    *packetBufferPosition = 0;
                    framer.packetInProgress = false;
                }
            }

            if (!reprocess) {
                i++;
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
        size_t availableBytes = serial.available();
        std::vector<uint8_t> newBytes(availableBytes);
        size_t newBytesLength = serial.readBytes(newBytes.data(), availableBytes);

        activeFramer = (direction == DEVICE1_TO_DEVICE2) ? &framerState1 : &framerState2;
        activeMaxPacketSize = (direction == DEVICE1_TO_DEVICE2) ? buffer1MaxSize : buffer2MaxSize;

        packetFinder(buffer.data(), &bufferPosition, newBytes.data(), &newBytesLength, [&]() {
            if (!packetHandler) {
                return;
            }
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
