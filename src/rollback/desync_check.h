#pragma once

// Local desync check (testing only).
//
// A second copy of the game that never guesses: it only runs a tick once both
// players' inputs for it are confirmed. Rollback promises that once the inputs
// are known, its snapshot for a tick is exactly what you'd get by simulating
// with the real inputs from the start. This checks that promise byte for byte,
// and asserts if it's ever broken.
//
// It only compares against this machine. sync_check.h compares the two
// machines with each other.

#include "sim/sim.h"

// Start the reference copy from the match's starting state.
void desync_check_init(const GameState *start);

// Call after every tick that advances (never before this frame's rollback), so
// every saved snapshot reflects all remote inputs received so far, and the
// reference never falls further behind than the state ring reaches.
void desync_check(Tick next_tick);
