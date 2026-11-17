#pragma once
#include "sim/sim.h"

#define MAX_ROLLBACK 8
#define INPUT_DELAY 3

// Marks "no tick": an empty ring slot, or "nothing to roll back".
#define NO_TICK (-1)

// --------------------------------------------------------
// Input
// --------------------------------------------------------

// Empty both rings, prefill the input-delay ticks with confirmed zero input,
// and record which player index this machine controls (decided at startup,
// must be opposite on the two peers).
void rollback_init(int local_player);

// Fill out[] with both players' inputs for a tick, predicting the remote one
// if it isn't confirmed yet. The prediction is stored, so this changes the
// buffer.
void get_inputs(Tick tick, Input out[MAX_PLAYERS]);

// Writes a local input into the buffer. Ticks must be written in order with
// no gaps, and each one only once.
void write_local_input(Tick tick, Input input);

// Store a remote input received from the network: mark it confirmed, advance
// last_confirmed_remote_tick, and record a misprediction if it differs from
// what was predicted. Duplicates and ticks past a gap are ignored.
void add_remote_input(Tick tick, Input input);

// Newest tick for which we have every remote input. This is what we ack back
// to the peer.
Tick get_last_confirmed_remote_tick(void);

// Peer reports it has all our inputs up to and including acked_tick.
// Older acks (out-of-order packets) are ignored.
void receive_ack(Tick acked_tick);

// False when next_tick is too far past the last confirmed remote input.
bool can_advance(Tick next_tick);

// Earliest tick whose remote input was mispredicted, or NO_TICK.
Tick find_rollback_point(void);

// Call after re-simulating from the rollback point.
void clear_rollback_point(void);

// Range of local inputs the peer hasn't acked yet: ticks
// first .. first + count - 1. count is 0 when everything is acked.
void find_inputs_to_resend(Tick *first, int *count);

// Our own input for a tick that has been written and not yet dropped from
// the ring (any tick in the find_inputs_to_resend range qualifies).
Input get_local_input(Tick tick);


// --------------------------------------------------------
// Game State
// --------------------------------------------------------

void save_state(Tick tick, const GameState *state);

void load_state(Tick tick, GameState *state);
