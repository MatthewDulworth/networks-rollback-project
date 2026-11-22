// Tests for net/packet.c: round trips, the exact wire bytes, and rejection of
// malformed input. Run with `ctest --test-dir build` (or ./build/packet_test).

#include "net/packet.h"
#include <stdio.h>
#include <string.h>

// Not assert(): that's compiled out when NDEBUG is set (Release builds), and
// it stops at the first failure. CHECK reports every failure with its line
// and keeps going; main's return value tells ctest whether anything failed.
static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",               \
                    __FILE__, __LINE__, #cond);                        \
            failures++;                                                \
        }                                                              \
    } while (0)

// A packet with every input slot used and values that exercise high bits.
static InputPacket full_packet(void) {
    InputPacket p;
    memset(&p, 0, sizeof p);
    p.ack = 1000;
    p.sender_tick = 1007;
    p.hash_tick = 995;
    p.hash = 0xDEADBEEFu;  // high bit set: must stay unsigned end to end
    p.first_tick = 123456;
    p.count = PACKET_MAX_INPUTS;
    for (int i = 0; i < p.count; i++) {
        p.inputs[i] = (Input)(0xA000 + i * 37);
    }
    return p;
}

static void test_round_trip_full(void) {
    InputPacket p = full_packet();
    uint8_t buf[PACKET_MAX_SIZE];

    size_t n = packet_encode(&p, buf, sizeof buf);
    CHECK(n == PACKET_MAX_SIZE);

    InputPacket q;
    memset(&q, 0, sizeof q);
    CHECK(packet_decode(buf, n, &q));
    CHECK(q.ack == p.ack);
    CHECK(q.sender_tick == p.sender_tick);
    CHECK(q.hash_tick == p.hash_tick);
    CHECK(q.hash == p.hash);
    CHECK(q.first_tick == p.first_tick);
    CHECK(q.count == p.count);
    CHECK(memcmp(q.inputs, p.inputs, sizeof p.inputs) == 0);
}

static void test_round_trip_empty(void) {
    // Sent every frame once everything is acked: just carries our ack.
    InputPacket p = { .ack = 5, .first_tick = 6, .count = 0 };
    uint8_t buf[PACKET_MAX_SIZE];

    size_t n = packet_encode(&p, buf, sizeof buf);
    CHECK(n == PACKET_HEADER_SIZE);

    InputPacket q;
    CHECK(packet_decode(buf, n, &q));
    CHECK(q.ack == 5);
    CHECK(q.first_tick == 6);
    CHECK(q.count == 0);
}

static void test_negative_tick(void) {
    // NO_TICK is -1; it must survive the signed -> unsigned -> signed trip.
    InputPacket p = { .ack = -1, .sender_tick = -1, .hash_tick = -1,
                      .first_tick = -1, .count = 0 };
    uint8_t buf[PACKET_MAX_SIZE];

    size_t n = packet_encode(&p, buf, sizeof buf);
    InputPacket q;
    CHECK(packet_decode(buf, n, &q));
    CHECK(q.ack == -1);
    CHECK(q.sender_tick == -1);
    CHECK(q.hash_tick == -1);
    CHECK(q.first_tick == -1);
}

static void test_wire_bytes(void) {
    // Pin the exact format from packet.h, so a change to it can't slip by
    // unnoticed: the other peer may be running the old build.
    InputPacket p = { .ack = -1, .sender_tick = 300, .hash_tick = -1,
                      .hash = 0x0A0B0C0Du, .first_tick = 123456, .count = 1 };
    p.inputs[0] = 0x0102;
    uint8_t buf[PACKET_MAX_SIZE];

    size_t n = packet_encode(&p, buf, sizeof buf);
    const uint8_t expected[] = {
        0x52, 0x42, 0x46, 0x32,  // magic "RBF2"
        0xff, 0xff, 0xff, 0xff,  // ack -1
        0x00, 0x00, 0x01, 0x2c,  // sender_tick 300
        0xff, 0xff, 0xff, 0xff,  // hash_tick -1 (NO_TICK)
        0x0a, 0x0b, 0x0c, 0x0d,  // hash, big-endian
        0x00, 0x01, 0xe2, 0x40,  // first_tick 123456
        0x01,                    // count
        0x01, 0x02,              // inputs[0], big-endian
    };
    CHECK(n == sizeof expected);
    CHECK(memcmp(buf, expected, sizeof expected) == 0);
}

static void test_encode_rejects(void) {
    InputPacket p = full_packet();
    uint8_t buf[PACKET_MAX_SIZE];

    CHECK(packet_encode(&p, buf, PACKET_MAX_SIZE - 1) == 0);  // buffer too small

    p.count = PACKET_MAX_INPUTS + 1;
    CHECK(packet_encode(&p, buf, sizeof buf) == 0);

    p.count = -1;
    CHECK(packet_encode(&p, buf, sizeof buf) == 0);
}

static void test_decode_rejects(void) {
    InputPacket p = full_packet();
    uint8_t buf[PACKET_MAX_SIZE];
    size_t n = packet_encode(&p, buf, sizeof buf);
    InputPacket q;

    CHECK(!packet_decode(buf, 0, &q));                       // empty
    CHECK(!packet_decode(buf, PACKET_HEADER_SIZE - 1, &q));  // shorter than header
    CHECK(!packet_decode(buf, n - 1, &q));                   // last input cut off

    uint8_t longer[PACKET_MAX_SIZE + 1];
    memcpy(longer, buf, n);
    longer[n] = 0;
    CHECK(!packet_decode(longer, n + 1, &q));                // extra trailing byte

    buf[0] ^= 0x01;
    CHECK(!packet_decode(buf, n, &q));                       // wrong magic
    buf[0] ^= 0x01;

    buf[24] = 200;  // the count byte
    CHECK(!packet_decode(buf, n, &q));                       // count too large
}

int main(void) {
    test_round_trip_full();
    test_round_trip_empty();
    test_negative_tick();
    test_wire_bytes();
    test_encode_rejects();
    test_decode_rejects();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all packet tests passed\n");
    return 0;
}
