#pragma once

// Converts between packets the game uses (structs) and the bytes that go over
// the network. Knows nothing about sockets; see net.h for that.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sim/sim.h"

// Most inputs one packet can carry. Must be at least the rollback input ring
// size (INPUT_BUFFER_SIZE in rollback.c), since find_inputs_to_resend can
// return a range that long.
#define PACKET_MAX_INPUTS 32

// Wire format, all multi-byte fields big-endian (network byte order):
//
//   offset  size  field
//   0       4     magic        PACKET_MAGIC, rejects stray or foreign packets
//   4       4     ack          int32, newest tick we have all remote inputs for
//   8       4     sender_tick  int32, the tick the sender will simulate next
//   12      4     hash_tick    int32, tick of the state `hash` is for, or NO_TICK
//   16      4     hash         uint32, hash of the sender's confirmed state
//   20      4     first_tick   int32, tick of inputs[0]
//   24      1     count        number of inputs that follow (0..PACKET_MAX_INPUTS)
//   25      2*n   inputs       uint16 each
#define PACKET_MAGIC 0x52424632u  // "RBF2": rollback fighter, format 2
#define PACKET_HEADER_SIZE 25
#define PACKET_MAX_SIZE (PACKET_HEADER_SIZE + 2 * PACKET_MAX_INPUTS)

// A decoded packet, for use in the program. Never sent as-is: its layout in
// memory (padding, byte order) isn't the wire format above.
typedef struct {
    Tick ack;
    Tick sender_tick;
    Tick hash_tick;  // NO_TICK (-1) when there's no hash yet; hash is then 0.
    uint32_t hash;
    Tick first_tick;
    int count;
    Input inputs[PACKET_MAX_INPUTS];
} InputPacket;

// Write packet into buf in the wire format. Returns the number of bytes
// written, or 0 if buf is too small or count is out of range.
size_t packet_encode(const InputPacket *packet, uint8_t *buf, size_t capacity);

// Read a packet from len bytes of buf into out. Returns false (and leaves
// out unspecified) if the bytes aren't a valid packet: too short, wrong magic,
// count too large, or len doesn't match count.
bool packet_decode(const uint8_t *buf, size_t len, InputPacket *out);

// --------------------------------------------------------
// Hello: LAN discovery (discovery.c), sent before the match starts
// --------------------------------------------------------
//
//   offset  size  field
//   0       4     magic    HELLO_MAGIC, different from PACKET_MAGIC so neither
//                          decoder accepts the other's packets
//   4       4     id       uint32, random, picked by the sender at startup
//   8       4     seen_id  uint32, id of the peer the sender has heard, or 0
#define HELLO_MAGIC 0x52424831u  // "RBH1": rollback fighter hello, format 1
#define HELLO_SIZE 12

typedef struct {
    uint32_t id;
    uint32_t seen_id;
} HelloPacket;

// Same contract as packet_encode / packet_decode.
size_t hello_encode(const HelloPacket *hello, uint8_t *buf, size_t capacity);
bool hello_decode(const uint8_t *buf, size_t len, HelloPacket *out);
