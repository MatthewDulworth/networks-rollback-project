#include "rollback/desync_check.h"
#include "rollback/rollback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static GameState reference_state;

void desync_check_init(const GameState *start) {
    memcpy(&reference_state, start, sizeof reference_state);
}

void desync_check(Tick next_tick) {
    Tick last_confirmed = get_last_confirmed_remote_tick();

    // Tick T can be checked and stepped once its inputs are confirmed
    // (T <= last_confirmed) and it has been simulated (T < next_tick).
    while (reference_state.tick <= last_confirmed
           && reference_state.tick < next_tick) {
        Tick t = reference_state.tick;

        // Snapshot t is the rollback side's state before tick t.
        GameState snapshot;
        load_state(t, &snapshot);
        if (memcmp(&snapshot, &reference_state, sizeof snapshot) != 0) {
            fprintf(stderr, "DESYNC at tick %d\n", t);
            assert(false && "rollback state differs from confirmed-only state");
        }

        // Tick t is confirmed for both players, so get_inputs returns the
        // real inputs and doesn't predict anything.
        Input tick_input[MAX_PLAYERS];
        GameState next;
        get_inputs(t, tick_input);
        sim(tick_input, &reference_state, &next);
        memcpy(&reference_state, &next, sizeof reference_state);
    }
}
