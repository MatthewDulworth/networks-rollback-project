#pragma once
#include "sim/sim.h"

#define MAX_ROLLBACK 8
#define INPUT_DELAY 3

// Marks "no tick": an empty ring slot, or "nothing to roll back".
#define NO_TICK (-1)

// --------------------------------------------------------
// Input
// --------------------------------------------------------

// Live ticks span from the oldest one we might roll back to
// (next_tick - MAX_ROLLBACK) to the newest local input (next_tick + INPUT_DELAY).
#define INPUT_BUFFER_SIZE 16
_Static_assert(INPUT_BUFFER_SIZE >= MAX_ROLLBACK + INPUT_DELAY + 1,
               "input ring too small");
_Static_assert((INPUT_BUFFER_SIZE & (INPUT_BUFFER_SIZE - 1)) == 0,
               "input ring size must be a power of 2");

typedef struct {
    Tick tick;
    Input input[MAX_PLAYERS];
    bool confirmed[MAX_PLAYERS];
} InputSlot;

extern InputSlot input_buffer[INPUT_BUFFER_SIZE];

extern Tick last_acked_local_tick;
extern Tick last_confirmed_remote_tick;
extern Tick earliest_misprediction;

// Empty both rings, prefill the input-delay ticks with confirmed zero input,
// and record which player index this machine controls (decided at startup,
// must be opposite on the two peers).
void rollback_init(int local_player);

// Fill out[] with both players' inputs for a tick, predicting the remote one
// if it isn't confirmed yet.
void get_inputs(Tick tick, Input out[MAX_PLAYERS]);

// Writes a local input into the buffer.
void write_local_input(Tick tick, Input input);

// Store a remote input received from the network: mark it confirmed, advance
// last_confirmed_remote_tick, and record a misprediction if it differs from
// what was predicted. Duplicates and ticks past a gap are ignored.
void add_remote_input(Tick tick, Input input);

// Peer reports it has all our inputs up to and including acked_tick.
// Older acks (out-of-order packets) are ignored.
void receive_ack(Tick acked_tick);

// False when next_tick is too far past the last confirmed remote input.
bool can_advance(Tick next_tick);

// Earliest tick whose remote input was mispredicted, or NO_TICK.
Tick find_rollback_point(void);

// Call after re-simulating from the rollback point.
void clear_rollback_point(void);

// Range of local inputs the peer hasn't acked yet.
void find_inputs_to_resend(Tick *first, int *count);


// --------------------------------------------------------
// Game State
// --------------------------------------------------------

// Holds MAX_ROLLBACK + 1 states: the present and everything we can roll back to.
#define STATE_BUFFER_SIZE 16
_Static_assert(STATE_BUFFER_SIZE >= MAX_ROLLBACK + 1,
               "state ring too small");
_Static_assert((STATE_BUFFER_SIZE & (STATE_BUFFER_SIZE - 1)) == 0,
               "state ring size must be a power of 2");

typedef struct {
    Tick tick;
    GameState state;
} StateSlot;

extern StateSlot state_buffer[STATE_BUFFER_SIZE];

void save_state(Tick tick, const GameState *state);

void load_state(Tick tick, GameState *state);
