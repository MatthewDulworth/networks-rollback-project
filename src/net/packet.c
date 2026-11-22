#include "net/packet.h"

// --------------------------------------------------------
// Byte helpers: big-endian (network byte order), built from shifts so the
// result doesn't depend on this machine's own byte order.
// --------------------------------------------------------

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t *p) {
    // Cast before shifting: p[0] is promoted to (signed) int, and shifting a
    // byte >= 0x80 left by 24 would overflow it.
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Field offsets, matching the wire format table in packet.h.
#define OFFSET_MAGIC       0
#define OFFSET_ACK         4
#define OFFSET_SENDER_TICK 8
#define OFFSET_HASH_TICK   12
#define OFFSET_HASH        16
#define OFFSET_FIRST_TICK  20
#define OFFSET_COUNT       24
#define OFFSET_INPUTS      PACKET_HEADER_SIZE

// --------------------------------------------------------
// Encode / decode
// --------------------------------------------------------

size_t packet_encode(const InputPacket *packet, uint8_t *buf, size_t capacity) {
    if (packet->count < 0 || packet->count > PACKET_MAX_INPUTS) {
        return 0;
    }
    size_t size = PACKET_HEADER_SIZE + 2 * (size_t)packet->count;
    if (capacity < size) {
        return 0;
    }

    write_u32(buf + OFFSET_MAGIC, PACKET_MAGIC);
    // Ticks are signed (NO_TICK is -1). Converting to uint32_t is well
    // defined (wraps mod 2^32) and decode converts back.
    write_u32(buf + OFFSET_ACK, (uint32_t)packet->ack);
    write_u32(buf + OFFSET_SENDER_TICK, (uint32_t)packet->sender_tick);
    write_u32(buf + OFFSET_HASH_TICK, (uint32_t)packet->hash_tick);
    write_u32(buf + OFFSET_HASH, packet->hash);
    write_u32(buf + OFFSET_FIRST_TICK, (uint32_t)packet->first_tick);
    buf[OFFSET_COUNT] = (uint8_t)packet->count;

    for (int i = 0; i < packet->count; i++) {
        write_u16(buf + OFFSET_INPUTS + 2 * i, packet->inputs[i]);
    }
    return size;
}

bool packet_decode(const uint8_t *buf, size_t len, InputPacket *out) {
    // Each check makes the next read safe: never touch a byte before
    // knowing it's inside the packet.
    if (len < PACKET_HEADER_SIZE) {
        return false;
    }
    if (read_u32(buf + OFFSET_MAGIC) != PACKET_MAGIC) {
        return false;
    }
    int count = buf[OFFSET_COUNT];
    if (count > PACKET_MAX_INPUTS) {
        return false;
    }
    if (len != PACKET_HEADER_SIZE + 2 * (size_t)count) {
        return false;
    }

    // uint32_t -> int32_t for values above INT32_MAX is implementation-
    // defined in C11, but every compiler we care about wraps it back, which
    // undoes the cast in encode.
    out->ack = (Tick)read_u32(buf + OFFSET_ACK);
    out->sender_tick = (Tick)read_u32(buf + OFFSET_SENDER_TICK);
    out->hash_tick = (Tick)read_u32(buf + OFFSET_HASH_TICK);
    out->hash = read_u32(buf + OFFSET_HASH);
    out->first_tick = (Tick)read_u32(buf + OFFSET_FIRST_TICK);
    out->count = count;
    for (int i = 0; i < count; i++) {
        out->inputs[i] = read_u16(buf + OFFSET_INPUTS + 2 * i);
    }
    return true;
}

size_t hello_encode(const HelloPacket *hello, uint8_t *buf, size_t capacity) {
    if (capacity < HELLO_SIZE) {
        return 0;
    }
    write_u32(buf, HELLO_MAGIC);
    write_u32(buf + 4, hello->id);
    write_u32(buf + 8, hello->seen_id);
    return HELLO_SIZE;
}

bool hello_decode(const uint8_t *buf, size_t len, HelloPacket *out) {
    if (len != HELLO_SIZE || read_u32(buf) != HELLO_MAGIC) {
        return false;
    }
    out->id = read_u32(buf + 4);
    out->seen_id = read_u32(buf + 8);
    return true;
}
