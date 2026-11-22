#pragma once

// Cross-machine desync detection. Each peer hashes its game state once a tick
// is confirmed (both players' real inputs known, so it can never be rolled
// back again) and sends the newest hash to the other peer. If the two hashes
// for the same tick ever differ, the machines have drifted apart.
//
// The local desync check in main.c only proves rollback matches a
// confirmed-only replay on the same machine. This proves the two machines
// agree with each other.

#include <stdint.h>
#include "sim/sim.h"

// FNV-1a (32-bit) over the raw bytes of the state. Relies on padding bytes
// being zero, which sim_init (memset) and sim/process_tick (memcpy) keep true.
uint32_t sync_hash_state(const GameState *state);

// Forget every stored hash and any detected desync.
void sync_check_init(void);

// Hash every saved snapshot that has become final since the last call and
// record it as ours. Call after every tick that advances (like
// desync_check), so any rollback for this frame's inputs has already been
// applied and the snapshots are still in the state ring.
void sync_check_after_tick(Tick next_tick);

// A hash the peer sent. Ignored if tick is NO_TICK or too old to compare.
void sync_check_add_remote(Tick tick, uint32_t hash);

// Newest local hash, for sending. *tick is NO_TICK (and *hash 0) if none yet.
void sync_check_newest_local(Tick *tick, uint32_t *hash);

// Newest tick where both peers' hashes were compared and matched, or NO_TICK.
Tick sync_check_last_match(void);

// First tick whose hashes differed, or NO_TICK. Once set it stays set: after a
// desync every later state is suspect, so there's nothing to recover.
Tick sync_check_first_desync(void);
