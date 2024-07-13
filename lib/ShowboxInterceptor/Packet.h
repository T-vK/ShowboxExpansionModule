// Packet.h
#ifndef PACKET_H
#define PACKET_H

#include <cstdint>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace Showbox {
    namespace Packet {
        constexpr uint8_t START_SEQUENCE[2] = {0xBE, 0xEF};
        constexpr uint8_t END_SEQUENCE[2] = {0xEF, 0xBE};
        namespace Types {
            constexpr uint8_t COMMAND_ACKNOWLEDGED = 0x01;
            constexpr uint8_t USER = 0x03;
            constexpr uint8_t UNKNOWN_04 = 0x04;
            constexpr uint8_t UNKNOWN_05 = 0x05;
            constexpr uint8_t DATA_REQUEST = 0x09;
            constexpr uint8_t UNKNOWN_15 = 0x15;
            constexpr uint8_t UNKNOWN_16 = 0x16;
            constexpr uint8_t TUNER_TOGGLE = 0x18;
            constexpr uint8_t TUNER_FEEDBACK = 0x19;
            constexpr uint8_t SNAPSHOT_RECALL = 0x1A;
            constexpr uint8_t SD_CARD_EVENT = 0x1E;
            constexpr uint8_t HEARTBEAT = 0xFF;

            #ifdef SHOWBOX_DEBUG
            inline const char* getName(uint8_t typeId) {
                switch (typeId) {
                    case COMMAND_ACKNOWLEDGED: return "COMMAND_ACKNOWLEDGED";
                    case USER: return "USER";
                    case UNKNOWN_04: return "UNKNOWN_04";
                    case UNKNOWN_05: return "UNKNOWN_05";
                    case DATA_REQUEST: return "DATA_REQUEST";
                    case UNKNOWN_15: return "UNKNOWN_15";
                    case UNKNOWN_16: return "UNKNOWN_16";
                    case TUNER_TOGGLE: return "TUNER_TOGGLE";
                    case SNAPSHOT_RECALL: return "SNAPSHOT_RECALL";
                    case SD_CARD_EVENT: return "SD_CARD_EVENT";
                    case HEARTBEAT: return "HEARTBEAT";
                    default: return "New Unknown";
                }
            }
            #endif // SHOWBOX_DEBUG
        } // namespace Types
        
        struct General {
            uint8_t startSequence[2];  // 2 bytes for START SEQUENCE
            uint8_t bodySize;          // 1 byte for BODY SIZE
            uint8_t type;              // 1 byte for TYPE
            uint8_t unknown;           // 1 byte for UNKNOWN
            std::vector<uint8_t> body; // Variable size for BODY
            uint8_t endSequence[2];    // 2 bytes for END SEQUENCE

            General(const uint8_t* data, size_t length) {
                if (length < 7) {
                    throw std::invalid_argument("Data too short to be a valid packet");
                }

                // Parse header
                startSequence[0] = data[0];
                startSequence[1] = data[1];
                bodySize = data[2];
                type = data[3];
                unknown = data[4];

                // Parse body
                body.assign(data + 5, data + length - 2);

                // Parse footer
                endSequence[0] = data[length - 2];
                endSequence[1] = data[length - 1];

                if (startSequence[0] != START_SEQUENCE[0] || startSequence[1] != START_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid start sequence");
                }

                if (endSequence[0] != END_SEQUENCE[0] || endSequence[1] != END_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid end sequence");
                }
            }
        };

        struct User {
            uint8_t startSequence[2];   // 2 bytes for START SEQUENCE
            uint8_t bodySize;           // 1 byte for BODY SIZE
            uint8_t type;               // 1 byte for TYPE
            uint8_t unknown;            // 1 byte for UNKNOWN
            uint8_t entityId;           // 1 byte for ENTITY ID
            uint8_t reserved[3];        // 3 bytes for RESERVED
            std::vector<uint8_t> value; // Variable size for VALUE
            uint8_t endSequence[2];     // 2 bytes for END SEQUENCE

            User(const uint8_t* data, size_t length) {
                if (length < 11) {
                    throw std::invalid_argument("Data too short to be a valid User packet");
                }

                // Parse header
                startSequence[0] = data[0];
                startSequence[1] = data[1];
                bodySize = data[2];
                type = data[3];
                unknown = data[4];

                if (type != Types::USER) {
                    throw std::invalid_argument("Invalid type for User packet");
                }

                // Parse body
                entityId = data[5];
                reserved[0] = data[6]; 
                reserved[1] = data[7];
                reserved[2] = data[8];
                value.assign(data + 9, data + length - 2);

                // Parse footer
                endSequence[0] = data[length - 2];
                endSequence[1] = data[length - 1];

                if (startSequence[0] != START_SEQUENCE[0] || startSequence[1] != START_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid start sequence");
                }

                if (endSequence[0] != END_SEQUENCE[0] || endSequence[1] != END_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid end sequence");
                }
            }

            /**
             * This function returns the value of the packet as a boolean.
             */
            bool getBoolValue() const {
                if (value.size() != 1) {
                    throw std::invalid_argument("Value size is not 1");
                }

                return value[0] == 0x01;
            }

            /**
             * This function returns the value of the packet as an unsigned 8-bit integer.
             */
            uint8_t getUint8Value() const {
                if (value.size() != 1) {
                    throw std::invalid_argument("Value size is not 1");
                }

                return value[0];
            }

            /**
             * This function returns the value of the packet as a 32-bit float in little-endian format.
             */
            float getFloatLeValue() const {
                if (value.size() != 4) {
                    throw std::invalid_argument("Value size is not 4");
                }

                // Reorder the bytes to form the correct little-endian format
                uint32_t temp = (value[0]) |
                                (value[1] << 8) |
                                (value[2] << 16) |
                                (value[3] << 24);
                float result;
                memcpy(&result, &temp, sizeof(result));
                return result;
            }
        };
        
        /**
            18 = tuner
            1A = snapshot
            01 18 01 02 - tuner chan 2 on
            01 18 00 02 - tuner chan 2 off
            01 1A 01 04 - snapshot recall chan 4
            01 01 00 1A - snapshot recall cmd received
            01 01 00 18 - tuner cmd received
         */
        struct SystemCommandAcknowledged {
            uint8_t startSequence[2];   // 2 bytes for START SEQUENCE
            uint8_t bodySize;           // 1 byte for BODY SIZE == 0x01
            uint8_t type;               // 1 byte for TYPE == 0x01
            uint8_t unknown;            // 1 byte for UNKNOWN == 0x00
            uint8_t ackCmdType;         // 1 byte for ACK CMD TYPE e.g. 0x18 for tuner toggle command acknowledged
            uint8_t endSequence[2];     // 2 bytes for END SEQUENCE

            SystemCommandAcknowledged(const uint8_t* data, size_t length) {
                if (length != 8) {
                    throw std::invalid_argument("Data length is not 8");
                }

                // Parse header
                startSequence[0] = data[0];
                startSequence[1] = data[1];
                bodySize = data[2];
                type = data[3];
                unknown = data[4];

                // Parse body
                ackCmdType = data[5];

                // Parse footer
                endSequence[0] = data[length - 2];
                endSequence[1] = data[length - 1];

                if (type != Types::COMMAND_ACKNOWLEDGED) {
                    throw std::invalid_argument("Invalid type for Command Acknowledge packet");
                }

                if (startSequence[0] != START_SEQUENCE[0] || startSequence[1] != START_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid start sequence");
                }

                if (endSequence[0] != END_SEQUENCE[0] || endSequence[1] != END_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid end sequence");
                }
            }
        };
   
        struct SystemCommand {
            uint8_t startSequence[2];   // 2 bytes for START SEQUENCE
            uint8_t bodySize;           // 1 byte for BODY SIZE == 0x01
            uint8_t type;               // 1 byte for TYPE == 0x01
            uint8_t unknown;            // 1 byte for UNKNOWN == 0x00
            uint8_t cmdType;            // 1 byte for CMD TYPE e.g. 0x18 for tuner toggle command
            uint8_t endSequence[2];     // 2 bytes for END SEQUENCE

            SystemCommand(const uint8_t* data, size_t length) {
                if (length != 8) {
                    throw std::invalid_argument("Data length is not 8");
                }

                // Parse header
                startSequence[0] = data[0];
                startSequence[1] = data[1];
                bodySize = data[2];
                type = data[3];
                unknown = data[4];

                // Parse body
                cmdType = data[5];

                // Parse footer
                endSequence[0] = data[length - 2];
                endSequence[1] = data[length - 1];

                if (type != Types::COMMAND_ACKNOWLEDGED) {
                    throw std::invalid_argument("Invalid type for Command packet");
                }

                if (startSequence[0] != START_SEQUENCE[0] || startSequence[1] != START_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid start sequence");
                }

                if (endSequence[0] != END_SEQUENCE[0] || endSequence[1] != END_SEQUENCE[1]) {
                    throw std::invalid_argument("Invalid end sequence");
                }
            }
        };
   
    } // namespace Packet
} // namespace Showbox

#endif // PACKET_H
