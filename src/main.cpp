#include <UARTInterceptor.h>
#include <BLEMidi.h>
#define SHOWBOX_DEBUG
#include <Entity.h>
#include <Looper.h>
#include <Packet.h>

// Define UART pins
constexpr uint8_t MIXER_RX = 16; // Mixer RX (Blue) | GPIO16 = TX2
constexpr uint8_t MIXER_TX = 17; // Mixer TX (Green) | GPIO17 = RX2
constexpr uint8_t BASE_RX = 10; // Base RX (Green) | GPIO10 = RX1 = SD3
constexpr uint8_t BASE_TX = 9; // Base TX (Blue) | GPIO09 = TX1 = SD2

const UARTInterceptor::Direction TO_MIXER = UARTInterceptor::Direction::TO_A;
const UARTInterceptor::Direction TO_BASE = UARTInterceptor::Direction::TO_B;

UARTInterceptor interceptor(MIXER_RX, MIXER_TX, BASE_RX, BASE_TX);


void handlePacket(uint8_t* packet, size_t& length, UARTInterceptor::Direction direction) {
    Showbox::Packet::General generalPacket(packet, length);
    // if (generalPacket.type != Showbox::Packet::Types::USER) {
    //     return;
    // }

    if (generalPacket.type == Showbox::Packet::Types::COMMAND_ACKNOWLEDGED) {
        Showbox::Packet::SystemCommandAcknowledged ackPacket(packet, length);
        // filter out heartbeat acks
        if (ackPacket.ackCmdType == Showbox::Packet::Types::HEARTBEAT) {
            return;
        }
    }

    if (generalPacket.type == Showbox::Packet::Types::HEARTBEAT) {
        return;
    }


    if (direction == TO_BASE) {
        Serial.print("<From Mixer>");
    } else if (direction == TO_MIXER) {
        Serial.print("<From Base>");
    }
    
    // Packet manipulation example (inverting with bitwise NOT)
    //for (size_t i = 0; i < length; ++i) {
    //    packet[i] ^= 0xFF;
    //}

    //Showbox::Packet::General generalPacket(packet, length);
    const char* typeName = Showbox::Packet::Types::getName(generalPacket.type);
    Serial.printf(" Decoded: { Type: %s, ", typeName);

    if (generalPacket.type == Showbox::Packet::Types::USER) {
        Showbox::Packet::User userPacket(packet, length);
        // utilize ShowBox::Entity::getName(entityId) to get the name of the entity
        const char* entityName = Showbox::Entity::getName(userPacket.entityId);
        Serial.printf("Entity: %s, ", entityName);
        Serial.print("Value: ");
        if (Showbox::Entity::getDataType(userPacket.entityId) == Showbox::Entity::DataTypes::BOOL) {
            bool value = userPacket.getBoolValue();
            Serial.print(value);
        } else if (Showbox::Entity::getDataType(userPacket.entityId) == Showbox::Entity::DataTypes::UINT8) {
            uint8_t value = userPacket.getUint8Value();
            Serial.print(value);
        } else if (Showbox::Entity::getDataType(userPacket.entityId) == Showbox::Entity::DataTypes::FLOAT_LE) {
            float value = userPacket.getFloatLeValue();
            //Serial.println(value);
            uint8_t unitId = Showbox::Entity::getUnit(userPacket.entityId);
            const char* unitName = Showbox::Entity::Units::getName(unitId);
            if (unitId == Showbox::Entity::Units::PERCENT) {
                Serial.print(value*100);
                Serial.print(" %");
            } else {
                Serial.print(value);
                Serial.print(" ");
                Serial.print(unitName);
            }
        }
        Serial.print(" ");
    } else if (generalPacket.type == Showbox::Packet::Types::COMMAND_ACKNOWLEDGED) {
        Showbox::Packet::SystemCommandAcknowledged ackPacket(packet, length);
        const char* ackCmdTypeName = Showbox::Packet::Types::getName(ackPacket.ackCmdType);
        Serial.printf("Unknown byte: %02X, ", generalPacket.unknown);
        Serial.printf("Acknowledged command type: %02X (%s) ", ackPacket.ackCmdType, ackCmdTypeName);
    } else {
        Serial.printf("Unknown byte: %02X, ", generalPacket.unknown);

        Serial.print("Body bytes: ");
        for (size_t i = 0; i < generalPacket.body.size(); ++i) {
            Serial.printf("%02X ", generalPacket.body[i]);
        }
    }
    Serial.print("} Raw: { ");

    // Print the incoming packet
    for (size_t i = 0; i < length; ++i) {
        Serial.printf("%02X ", packet[i]);
    }

    Serial.println("}");
}


void injectPacket(const uint8_t* packet, size_t length, UARTInterceptor::Direction direction) {
    interceptor.injectPacket(packet, length, direction);
}

/////////

void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint16_t timestamp) {
  Serial.printf("Note on : channel %d, note %d, velocity %d (timestamp %dms)\n", channel, note, velocity, timestamp);
}

void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, uint16_t timestamp) {
  Serial.printf("Note off : channel %d, note %d, velocity %d (timestamp %dms)\n", channel, note, velocity, timestamp);
}

void onAfterTouchPoly(uint8_t channel, uint8_t note, uint8_t pressure, uint16_t timestamp) {
  Serial.printf("Polyphonic after touch : channel %d, note %d, pressure %d (timestamp %dms)\n", channel, note, pressure, timestamp);
}

void onControlChange(uint8_t channel, uint8_t controller, uint8_t value, uint16_t timestamp) {
    Serial.printf("Control change : channel %d, controller %d, value %d (timestamp %dms)\n", channel, controller, value, timestamp);
}

void onProgramChange(uint8_t channel, uint8_t program, uint16_t timestamp) {
    Serial.printf("Program change : channel %d, program %d (timestamp %dms)\n", channel, program, timestamp);
}

void onAfterTouch(uint8_t channel, uint8_t pressure, uint16_t timestamp) {
    Serial.printf("After touch : channel %d, pressure %d (timestamp %dms)\n", channel, pressure, timestamp);
}

void onPitchbend(uint8_t channel, uint16_t value, uint16_t timestamp) {
    Serial.printf("Pitch bend : channel %d, value %d (timestamp %dms)\n", channel, value, timestamp);
}

/////////



void setup() {
    Serial.begin(230400);
    interceptor.begin(115200, 115200); // Baud rates for Base and Mixer
    interceptor.setStartBytes(0xBE, 0xEF);
    interceptor.setEndBytes(0xEF, 0xBE);
    interceptor.setPacketHandler(handlePacket);
    interceptor.setPacketInjector(injectPacket);
    Serial.println("\nUART Interceptor ready\n");

    /*
    BLEMidiServer.begin("Showbox Interceptor");
      BLEMidiServer.setOnConnectCallback([]() {
        Serial.println("Connected");
    });
    BLEMidiServer.setOnDisconnectCallback([]() {
        Serial.println("Disconnected");
    });
    BLEMidiServer.setNoteOnCallback(onNoteOn);
    BLEMidiServer.setNoteOffCallback(onNoteOff);
    BLEMidiServer.setAfterTouchPolyCallback(onAfterTouchPoly);
    BLEMidiServer.setControlChangeCallback(onControlChange);
    BLEMidiServer.setProgramChangeCallback(onProgramChange);
    BLEMidiServer.setAfterTouchCallback(onAfterTouch);
    BLEMidiServer.setPitchBendCallback(onPitchbend);
    BLEMidiServer.enableDebugging();

    Serial.println("Bluetooth MIDI ready");
    */
}

unsigned long previousMillis = 0; 
unsigned long interval = 1000; // interval between note on and note off
bool noteOnSent = false;

void loop() {
    interceptor.loop();
    /*if (BLEMidiServer.isConnected()) {
        unsigned long currentMillis = millis();

        if (!noteOnSent) {
            BLEMidiServer.noteOn(0, 69, 127);
            previousMillis = currentMillis;
            noteOnSent = true;
        }

        if (noteOnSent && (currentMillis - previousMillis >= interval)) {
            BLEMidiServer.noteOff(0, 69, 127);
            previousMillis = currentMillis;
            noteOnSent = false;
        }
    }*/
}