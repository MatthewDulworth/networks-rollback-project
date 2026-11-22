#include "net/netsim.h"
#include "net/net.h"

#include <string.h>  // memcpy
#include <time.h>    // time, for the random seed

// --------------------------------------------------------
// Delay queues
// --------------------------------------------------------

// Bigger than any packet the game sends (PACKET_MAX_SIZE is 77 bytes). Kept
// separate so this module doesn't depend on the packet format.
#define NETSIM_MAX_PACKET 512

// At ~60 packets per second, 128 slots hold about 2 seconds of one-way delay.
// That's more than any setting allowed below, so a full queue only happens if
// something is badly wrong. Then the packet is dropped, which is what a real
// overloaded router does too.
#define QUEUE_SIZE 128

#define MAX_LAG_MS 1000
#define MAX_JITTER_MS 500

typedef struct {
    bool used;
    double release_time;  // When the packet is allowed to continue.
    size_t len;
    uint8_t data[NETSIM_MAX_PACKET];
} QueuedPacket;

// A queue here is just an unordered array of slots. Delivery picks the due
// packet with the earliest release time, so jitter can reorder packets, just
// like a real network.
typedef struct {
    QueuedPacket slots[QUEUE_SIZE];
} Queue;

static Queue outgoing;
static Queue incoming;

static NetsimSettings settings;
static double current_time;  // `now` from the last netsim_update.
static int dropped;

// --------------------------------------------------------
// Random numbers
// --------------------------------------------------------

// xorshift32: a tiny, fast generator. Not the C library's rand(), because the
// sim must never share random state with anything: if it did, a dropped
// packet here could change the game on one machine only. (The sim doesn't use
// randomness at all, but keeping this private makes that impossible to break.)
static uint32_t rng_state;

static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

// Uniform integer in [0, n). The modulo is very slightly biased, which doesn't
// matter for simulating a network.
static int rng_below(int n) {
    return (int)(rng_next() % (uint32_t)n);
}

// --------------------------------------------------------
// Queue helpers
// --------------------------------------------------------

// Delay for one packet in one direction, in seconds: half the round-trip lag,
// plus a random amount in [-jitter/2, +jitter/2]. Never negative.
static double one_way_delay(void) {
    int delay_ms = settings.lag_ms / 2;
    if (settings.jitter_ms > 0) {
        delay_ms += rng_below(settings.jitter_ms + 1) - settings.jitter_ms / 2;
    }
    if (delay_ms < 0) {
        delay_ms = 0;
    }
    return delay_ms / 1000.0;
}

// Decide whether to drop the packet; if not, store it with a delay. Returns
// false if it was dropped (on purpose or because the queue is full).
static bool enqueue(Queue *q, const uint8_t *data, size_t len) {
    if (len > NETSIM_MAX_PACKET) {
        return false;
    }
    if (settings.loss_pct > 0 && rng_below(100) < settings.loss_pct) {
        dropped++;
        return false;
    }
    for (int i = 0; i < QUEUE_SIZE; i++) {
        QueuedPacket *slot = &q->slots[i];
        if (!slot->used) {
            slot->used = true;
            slot->release_time = current_time + one_way_delay();
            slot->len = len;
            memcpy(slot->data, data, len);
            return true;
        }
    }
    dropped++;  // queue full
    return false;
}

// The due packet (release_time <= now) with the earliest release time, or
// NULL if none are due. A linear scan over 128 slots is cheap at 60 Hz.
static QueuedPacket *next_due(Queue *q) {
    QueuedPacket *best = NULL;
    for (int i = 0; i < QUEUE_SIZE; i++) {
        QueuedPacket *slot = &q->slots[i];
        if (slot->used && slot->release_time <= current_time
            && (best == NULL || slot->release_time < best->release_time)) {
            best = slot;
        }
    }
    return best;
}

// --------------------------------------------------------
// Public
// --------------------------------------------------------

void netsim_init(void) {
    memset(&outgoing, 0, sizeof outgoing);
    memset(&incoming, 0, sizeof incoming);
    memset(&settings, 0, sizeof settings);
    current_time = 0;
    dropped = 0;

    // Any nonzero seed works; xorshift gets stuck at 0.
    rng_state = (uint32_t)time(NULL) | 1u;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void netsim_set(NetsimSettings s) {
    settings.lag_ms = clamp_int(s.lag_ms, 0, MAX_LAG_MS);
    settings.jitter_ms = clamp_int(s.jitter_ms, 0, MAX_JITTER_MS);
    settings.loss_pct = clamp_int(s.loss_pct, 0, 100);
}

NetsimSettings netsim_get(void) {
    return settings;
}

void netsim_send(const uint8_t *data, size_t len) {
    enqueue(&outgoing, data, len);
}

int netsim_receive(uint8_t *buf, size_t capacity) {
    QueuedPacket *packet = next_due(&incoming);
    if (packet == NULL) {
        return 0;
    }
    // Same as recv on a datagram socket: a packet longer than the buffer is
    // cut off.
    size_t len = packet->len < capacity ? packet->len : capacity;
    memcpy(buf, packet->data, len);
    packet->used = false;
    return (int)len;
}

void netsim_update(double now) {
    current_time = now;

    // Pull everything the socket has into the incoming queue, where it waits
    // out its delay before netsim_receive hands it to the game.
    uint8_t buf[NETSIM_MAX_PACKET];
    int len;
    while ((len = net_receive(buf, sizeof buf)) > 0) {
        enqueue(&incoming, buf, (size_t)len);
    }

    // Send every outgoing packet whose delay is over, oldest first.
    QueuedPacket *packet;
    while ((packet = next_due(&outgoing)) != NULL) {
        net_send(packet->data, packet->len);
        packet->used = false;
    }
}

int netsim_dropped_count(void) {
    return dropped;
}
