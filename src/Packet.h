// Packet.h
#ifndef PACKET_H
#define PACKET_H

template <uint8_t type, typename T>
struct Packet {
    uint8_t start_seq[2] {0xBE, 0xEF};
    uint8_t body_size {sizeof(T) - 1};
    uint8_t packet_type {type};
    T p;
    uint8_t end_seq[2] {0xEF, 0xBE};

    void importP(const uint8_t *raw_packet) {
        memcpy(&p, raw_packet + 4, sizeof(T));
    }
    void exportP(uint8_t *raw_packet) {
        memcpy(raw_packet + 4, &p, sizeof(T));
    }
    virtual void intercept() = 0;
    void handle(uint8_t *raw_packet) {
        // printRawPacket("before", this);
        importP(raw_packet);
        // printRawPacket("import", this);
        intercept();
        // printRawPacket("export", this);
        exportP(raw_packet);
    }
} __attribute__((packed));

struct StateOnlyNoBody {
    uint8_t state;
} __attribute__((packed));

// Example: BE EF 00 1E 02 EF BE
typedef Packet<SD_CARD_EVENT, StateOnlyNoBody> SdCardEventPacket;

// Example: BE EF 01 18 01 02 EF BE
struct ToggleTunerBody {
    uint8_t state;
    uint8_t channel;
} __attribute__((packed));

typedef Packet<TUNER_TOGGLE, ToggleTunerBody> ToggleTunerPacket;

// Example: BE EF 01 1A 01 04 EF BE
struct SnapshotBody {
    uint8_t action;
    uint8_t slot;
} __attribute__((packed));

typedef Packet<SNAPSHOT, SnapshotBody> SnapshotPacket;

template <uint8_t id, typename T>
struct EntityBody {
    uint8_t header_byte {0};
    uint8_t entity_id {id};
    uint8_t reserved[3] {0};
    T value;
} __attribute__((packed));

// Example: BE EF 05 03 00 59 00 00 00 01 EF BE
typedef Packet<ENTITY, EntityBody<BOOL, bool>> BoolEntityPacket;

// Example: BE EF 05 03 00 5C 00 00 00 04 EF BE
typedef Packet<ENTITY, EntityBody<UINT8, uint8_t>> Uint8EntityPacket;

// Example: BE EF 08 03 00 58 00 00 00 00 00 C0 40 EF BE
typedef Packet<ENTITY, EntityBody<FLOAT, float>> FloatEntityPacket;

#endif // PACKET_H