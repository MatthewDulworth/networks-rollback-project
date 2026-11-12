#include "rollback/rollback.h"
#include <string.h>
#include <assert.h>

// Definitions for the extern declarations in rollback.h.
InputSlot input_buffer[INPUT_BUFFER_SIZE];
StateSlot state_buffer[STATE_BUFFER_SIZE];

Tick last_acked_local_tick;
Tick last_confirmed_remote_tick;
Tick earliest_misprediction;

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
    last_acked_local_tick = INPUT_DELAY - 1;
    last_confirmed_remote_tick = INPUT_DELAY - 1;

    earliest_misprediction = NO_TICK;
}


// --------------------------------------------------------
// Game State
// --------------------------------------------------------
void save_state(Tick tick, const GameState *state) {
    assert(tick >= 0);

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