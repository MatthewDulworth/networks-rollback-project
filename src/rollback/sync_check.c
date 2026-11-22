#include "rollback/sync_check.h"
#include "rollback/rollback.h"  // NO_TICK, load_state, last confirmed tick
#include <string.h>

// --------------------------------------------------------
// Hash
// --------------------------------------------------------

// FNV-1a: for each byte, XOR it into the hash, then multiply by a fixed
// prime. Both constants come from the FNV spec. Not cryptographic, but fast,
// tiny, and any single changed byte changes the result, which is all a
// desync check needs. unsigned math wraps mod 2^32 by definition, so the
// multiply overflowing is intended and well defined.
#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME 16777619u

uint32_t sync_hash_state(const GameState *state) {
    const uint8_t *bytes = (const uint8_t *)state;
    uint32_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < sizeof *state; i++) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

// --------------------------------------------------------
// Hash ring
// --------------------------------------------------------
// One slot per tick, holding our hash and the peer's. Whichever arrives
// second triggers the comparison, so it works both when the peer is ahead
// of us (its hash waits for ours) and behind (ours waits for its).
//
// The two peers' confirmed ticks drift apart by roughly the one-way latency
// in ticks. 64 ticks is about a second at 60 Hz, far more than that. If a
// hash falls out of the ring before its partner arrives, that tick just
// isn't compared; later ones still are.
#define SYNC_RING_SIZE 64
_Static_assert((SYNC_RING_SIZE & (SYNC_RING_SIZE - 1)) == 0,
               "sync ring size must be a power of 2");

typedef struct {
    Tick tick;
    bool has_local;
    bool has_remote;
    uint32_t local_hash;
    uint32_t remote_hash;
} SyncSlot;

static SyncSlot ring[SYNC_RING_SIZE];
static Tick newest_local_tick;
static uint32_t newest_local_hash;
static Tick last_match;
static Tick first_desync;

// Oldest snapshot not hashed yet.
static Tick next_hash_tick;

void sync_check_init(void) {
    memset(ring, 0, sizeof ring);
    for (size_t i = 0; i < SYNC_RING_SIZE; i++) {
        ring[i].tick = NO_TICK;
    }
    newest_local_tick = NO_TICK;
    newest_local_hash = 0;
    last_match = NO_TICK;
    first_desync = NO_TICK;
    next_hash_tick = 0;
}

// Slot for `tick`, cleared if it held an older tick. NULL if a newer tick
// already owns it: this tick is too old to keep.
static SyncSlot *claim_slot(Tick tick) {
    SyncSlot *slot = &ring[(uint32_t)tick & (SYNC_RING_SIZE - 1)];
    if (slot->tick > tick) {
        return NULL;
    }
    if (slot->tick < tick) {
        memset(slot, 0, sizeof *slot);
        slot->tick = tick;
    }
    return slot;
}

static void compare(const SyncSlot *slot) {
    if (!slot->has_local || !slot->has_remote) {
        return;
    }
    if (slot->local_hash == slot->remote_hash) {
        // Remote hashes can arrive out of order, so only move forward.
        if (slot->tick > last_match) {
            last_match = slot->tick;
        }
    } else if (first_desync == NO_TICK || slot->tick < first_desync) {
        first_desync = slot->tick;
    }
}

// Our hash of the confirmed state before `tick` (the snapshot for `tick`).
static void add_local(Tick tick, uint32_t hash) {
    SyncSlot *slot = claim_slot(tick);
    if (slot == NULL) {
        return;
    }
    slot->has_local = true;
    slot->local_hash = hash;
    if (tick > newest_local_tick) {
        newest_local_tick = tick;
        newest_local_hash = hash;
    }
    compare(slot);
}

// --------------------------------------------------------
// Public interface
// --------------------------------------------------------

// Snapshot t (the state before tick t) is final once:
// - every input it depends on (ticks 0 .. t-1) is confirmed, so it can never
//   be rolled back again: t - 1 <= last confirmed remote tick, and
// - it has been saved, which happens when tick t is simulated: t < next_tick.
void sync_check_after_tick(Tick next_tick) {
    Tick last_confirmed = get_last_confirmed_remote_tick();
    while (next_hash_tick <= last_confirmed + 1 && next_hash_tick < next_tick) {
        GameState snapshot;
        load_state(next_hash_tick, &snapshot);
        add_local(next_hash_tick, sync_hash_state(&snapshot));
        next_hash_tick++;
    }
}

void sync_check_add_remote(Tick tick, uint32_t hash) {
    // Network data: the peer sends NO_TICK before it has a hash.
    if (tick < 0) {
        return;
    }
    SyncSlot *slot = claim_slot(tick);
    if (slot == NULL) {
        return;
    }
    // The peer resends its newest hash every frame until it has a newer
    // one, so the same (tick, hash) often arrives many times. Comparing
    // again is harmless.
    slot->has_remote = true;
    slot->remote_hash = hash;
    compare(slot);
}

void sync_check_newest_local(Tick *tick, uint32_t *hash) {
    *tick = newest_local_tick;
    *hash = newest_local_hash;
}

Tick sync_check_last_match(void) {
    return last_match;
}

Tick sync_check_first_desync(void) {
    return first_desync;
}
