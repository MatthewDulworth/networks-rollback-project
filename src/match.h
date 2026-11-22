#pragma once

// One peer's side of a match: everything that happens each frame between
// sampling the keyboard and drawing. Receive packets, roll back if a
// prediction was wrong, simulate the ticks that are due, keep pace with the
// peer (frame advantage), send.
//
// No raylib: the caller passes in the input and a clock, and draws whatever
// match_state() returns. That's what lets tests/netcode_test.c run real peers
// without a window.
//
// Needs net_init and netsim_init to have been called first.

#include <stdbool.h>
#include "sim/sim.h"

// Seconds since some fixed point (raylib's GetTime, or a monotonic clock in
// tests). Only differences between readings are used.
typedef double (*MatchClock)(void);

// Counts for the overlay and for tests.
typedef struct {
    int rollbacks;       // Rollbacks performed.
    int max_depth;       // Deepest rollback, in ticks.
    int stalled_frames;  // Frames that hit the rollback limit and stopped.
} MatchStats;

// Start a new match on this machine as local_player (PLAYER_1 or PLAYER_2,
// opposite on the two peers). Resets the sim, rollback, sync check and
// session modules.
void match_init(int local_player, MatchClock clock);

// Run one frame with this frame's keyboard sample.
void match_frame(Input local_input);

// How long to sleep before the next frame, in seconds.
double match_time_until_next_frame(void);

// The state to draw (the newest simulated tick, predictions included).
const GameState *match_state(void);

// True once the peer's first packet arrived and ticks started running.
bool match_started(void);

// The tick that will be simulated next.
Tick match_next_tick(void);

// Ticks ahead of the peer as of the last frame (see session.h).
double match_frame_advantage(void);

// Counts from the last finished one-second window (what the overlay shows).
MatchStats match_stats_last_second(void);

// Counts since match_init. max_depth is the deepest rollback overall.
MatchStats match_stats_total(void);
