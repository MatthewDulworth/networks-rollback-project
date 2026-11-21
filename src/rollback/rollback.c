#include "rollback/rollback.h"
#include <string.h>
#include <assert.h>

// --------------------------------------------------------
// Ring buffer layout (private to this file)
// --------------------------------------------------------

// The input ring has to hold two windows at once:
// - Rollback: from the oldest tick we might roll back to (next_tick -
//   MAX_ROLLBACK) up to the newest local input (next_tick + INPUT_DELAY).
// - The peer running ahead of us: its inputs can arrive up to about
//   MAX_ROLLBACK + INPUT_DELAY ticks past our own newest tick, and our
//   unacked local inputs (still needed for resending) stretch back about as
//   far.
// Both come to roughly 2 * (MAX_ROLLBACK + INPUT_DELAY) live ticks.
#define INPUT_BUFFER_SIZE 32
_Static_assert(INPUT_BUFFER_SIZE >= 2 * (MAX_ROLLBACK + INPUT_DELAY) + 1,
               "input ring too small");
_Static_assert((INPUT_BUFFER_SIZE & (INPUT_BUFFER_SIZE - 1)) == 0,
               "input ring size must be a power of 2");

typedef struct {
    Tick tick;
    Input input[MAX_PLAYERS];
    bool confirmed[MAX_PLAYERS];
    // True once the sim has run this tick on a guessed remote input.
    bool remote_predicted;
} InputSlot;

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

// --------------------------------------------------------
// Module state. Other code goes through the functions in rollback.h.
// --------------------------------------------------------
static InputSlot input_buffer[INPUT_BUFFER_SIZE];
static StateSlot state_buffer[STATE_BUFFER_SIZE];

static Tick newest_local_tick;
static Tick last_acked_local_tick;
static Tick last_confirmed_remote_tick;
static Tick earliest_misprediction;

// Which player index this machine controls. The remote is the other one.
static int local_player;

static size_t ring_index(Tick tick, size_t size) {
    assert(size > 0 && (size & (size - 1)) == 0); // Assert size is power of 2.
    return ((uint32_t) tick) & (size - 1);
}

void rollback_init(int player) {
    assert(player == PLAYER_1 || player == PLAYER_2);
    local_player = player;

    // Mark every slot in both rings empty.
    memset(input_buffer, 0, sizeof input_buffer);
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) {
        input_buffer[i].tick = NO_TICK;
    }

    memset(state_buffer, 0, sizeof state_buffer);
    for (size_t i = 0; i < STATE_BUFFER_SIZE; i++) {
        state_buffer[i].tick = NO_TICK;
    }

    // Nobody can send input for the first INPUT_DELAY ticks (local input for
    // tick 0 is written to tick INPUT_DELAY). Both peers fill these in the
    // same way: no buttons, already confirmed for both players.
    for (Tick t = 0; t < INPUT_DELAY; t++) {
        InputSlot *slot = &input_buffer[ring_index(t, INPUT_BUFFER_SIZE)];
        slot->tick = t;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            slot->input[p] = 0;
            slot->confirmed[p] = true;
        }
    }

    // Both sides already "have" the prefilled ticks, so the first input that
    // needs to be sent / received is tick INPUT_DELAY.
    newest_local_tick = INPUT_DELAY - 1;
    last_acked_local_tick = INPUT_DELAY - 1;
    last_confirmed_remote_tick = INPUT_DELAY - 1;

    earliest_misprediction = NO_TICK;
}

// --------------------------------------------------------
// Input
// --------------------------------------------------------

static int remote_player_index(void) {
    return 1 - local_player;
}

void get_inputs(Tick tick, Input out[MAX_PLAYERS]) {
    InputSlot *slot = &input_buffer[ring_index(tick, INPUT_BUFFER_SIZE)];
    int remote = remote_player_index();

    // Local input is written INPUT_DELAY ticks ahead (or prefilled), so any
    // tick we simulate must already own its slot.
    assert(slot->tick == tick);

    if (!slot->confirmed[remote]) {
        // Predict: the remote player repeats their last confirmed input.
        // The guess is stored in the slot so add_remote_input can compare
        // the real input against what the sim actually used.
        const size_t index = ring_index(last_confirmed_remote_tick, INPUT_BUFFER_SIZE);
        const InputSlot *last = &input_buffer[index];
        assert(last->tick == last_confirmed_remote_tick);
        slot->input[remote] = last->input[remote];
        slot->remote_predicted = true;
    }

    for (int p = 0; p < MAX_PLAYERS; p++) {
        out[p] = slot->input[p];
    }
}

// Return the slot for `tick`, claiming it if it still holds an older tick
// (or NO_TICK). If it already holds `tick`, it's returned untouched, so
// whichever of local/remote input arrived first is kept.
static InputSlot *claim_slot(Tick tick) {
    InputSlot *slot = &input_buffer[ring_index(tick, INPUT_BUFFER_SIZE)];

    // A newer tick already owns this slot: the caller fell too far behind.
    assert(tick >= slot->tick);

    if (tick > slot->tick) {
        slot->tick = tick;
        slot->remote_predicted = false;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            slot->input[p] = 0;
            slot->confirmed[p] = false;
        }
    }
    return slot;
}

void write_local_input(Tick tick, Input input) {
    // Local ticks are contiguous: every tick gets exactly one input, in
    // order. That's what lets find_inputs_to_resend describe the unacked
    // inputs as a single range.
    assert(tick == newest_local_tick + 1);

    InputSlot *slot = claim_slot(tick);

    // Local input is sent to the peer, so it must never be rewritten.
    assert(!slot->confirmed[local_player]);
    slot->input[local_player] = input;
    slot->confirmed[local_player] = true;
    newest_local_tick = tick;
}

void add_remote_input(Tick tick, Input input) {
    // Accept remote inputs strictly in order. Anything else is either a
    // duplicate (already confirmed) or past a gap (an earlier packet was lost
    // or reordered). The peer resends every unacked input in each packet, so
    // dropping these loses nothing: they'll come again after the gap fills.
    if (tick != last_confirmed_remote_tick + 1) {
        return;
    }

    InputSlot *slot = claim_slot(tick);
    int remote = remote_player_index();
    assert(!slot->confirmed[remote]);

    // Only a tick that was actually simulated on a guess can be wrong. An
    // unsimulated slot just holds the placeholder 0, which means nothing.
    if (slot->remote_predicted && slot->input[remote] != input) {
        // Ticks arrive in increasing order, so the first one recorded since
        // the last clear is the earliest.
        if (earliest_misprediction == NO_TICK) {
            earliest_misprediction = tick;
        }
    }

    slot->input[remote] = input;
    slot->confirmed[remote] = true;
    last_confirmed_remote_tick = tick;
}

Tick get_last_confirmed_remote_tick(void) {
    return last_confirmed_remote_tick;
}

Tick find_rollback_point(void) {
    return earliest_misprediction;
}

void clear_rollback_point(void) {
    earliest_misprediction = NO_TICK;
}

void receive_ack(Tick acked_tick) {
    // Packets can arrive out of order, so an older ack may show up after a
    // newer one. Acks only ever move forward.
    if (acked_tick > last_acked_local_tick) {
        last_acked_local_tick = acked_tick;
    }
}

bool can_advance(Tick next_tick) {
    // Simulating next_tick means guessing every remote input from
    // last_confirmed_remote_tick + 1 through next_tick. Allow at most
    // MAX_ROLLBACK guessed ticks, so a rollback never needs a state older
    // than the state ring keeps.
    return next_tick - last_confirmed_remote_tick <= MAX_ROLLBACK;
}

void find_inputs_to_resend(Tick *first, int *count) {
    // The peer has everything up to last_acked_local_tick, and we've written
    // everything up to newest_local_tick, so the gap between them is exactly
    // what still needs sending.
    *first = last_acked_local_tick + 1;
    *count = newest_local_tick - last_acked_local_tick;

    // If the unacked range is longer than the ring, its oldest inputs have
    // already been overwritten and can never be resent.
    assert(*count >= 0 && *count <= INPUT_BUFFER_SIZE);
}

Input get_local_input(Tick tick) {
    const InputSlot *slot = &input_buffer[ring_index(tick, INPUT_BUFFER_SIZE)];
    assert(slot->tick == tick);
    assert(slot->confirmed[local_player]);
    return slot->input[local_player];
}

// --------------------------------------------------------
// Game State
// --------------------------------------------------------
void save_state(Tick tick, const GameState *state) {
    assert(tick >= 0);
    assert(state->tick == tick);

    // Find the slot 
    size_t index = ring_index(tick, STATE_BUFFER_SIZE);
    StateSlot *slot = &state_buffer[index];

    // Set the new tick
    slot->tick = tick;

    // Copy the state into the slot
    memcpy(&slot->state, state, sizeof *state);
}

void load_state(Tick tick, GameState *state) {
    size_t index = ring_index(tick, STATE_BUFFER_SIZE);
    const StateSlot *slot = &state_buffer[index];

    assert(tick >= 0);
    assert(tick == slot->tick);

    memcpy(state, &slot->state, sizeof *state);
}