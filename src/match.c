#include "match.h"
#include "rollback/rollback.h"
#include "rollback/sync_check.h"
#include "rollback/desync_check.h"
#include "net/netsim.h"
#include "net/session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CATCHUP_MARGIN (0.004)
#define TICK_THRESHOLD (STEP / 30.0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

// Frame advantage sync: when we're more than THRESHOLD ticks ahead of the
// peer, push start_time back by NUDGE each frame until we're not.
// - THRESHOLD 1: the estimate swings by about +-1 tick from jitter. Lower
//   values mostly react to that noise (twice the nudges, no better balance);
//   2 barely ever fires and leaves one peer doing most of the rollbacks.
// - NUDGE STEP/10: drops one tick every 10 frames while correcting, which is
//   barely visible, and closes a typical 3-tick lead in about half a second.
//   STEP/2 balanced just as well but visibly runs at half speed, and wasted
//   ~4x more time on nudges caused by noise.
#define FRAME_ADVANTAGE_THRESHOLD 1.0
#define FRAME_ADVANTAGE_NUDGE (STEP / 10.0)

// --------------------------------------------------------
// Module state
// --------------------------------------------------------

static MatchClock now;

// See match_init in match.h.
static bool delay_mode;

static GameState state;
static GameState next_state;
static Tick next_tick;

// Stand-in for the start handshake (see follow_ups.md, "Shared start
// time"): don't start ticking until the first packet from the peer
// arrives. Both sides send every frame, so each starts within about one
// one-way trip of the other. Tick times are measured from start_time.
static bool started;
static double start_time;

// Set when can_advance stops the simulate loop. Decides the wait after
// this frame (see match_time_until_next_frame).
static bool stalled;

static double frame_advantage;

// --------------------------------------------------------
// Stats
// --------------------------------------------------------
// Counted over one-second windows. The overlay shows the last finished
// window, so the numbers update once a second instead of flickering.

static MatchStats stats_current;
static MatchStats stats_shown;
static MatchStats stats_total;
static double stats_window_start;

static void stats_update(double t) {
    if (t - stats_window_start >= 1.0) {
        stats_shown = stats_current;
        memset(&stats_current, 0, sizeof stats_current);
        stats_window_start = t;
    }
}

static void stats_note_rollback(Tick depth) {
    stats_current.rollbacks++;
    stats_total.rollbacks++;
    if (depth > stats_current.max_depth) {
        stats_current.max_depth = depth;
    }
    if (depth > stats_total.max_depth) {
        stats_total.max_depth = depth;
    }
}

static void stats_note_stall(void) {
    stats_current.stalled_frames++;
    stats_total.stalled_frames++;
}

// --------------------------------------------------------
// Frame
// --------------------------------------------------------

// Run one tick: snapshot the state before it, look up both players' inputs
// from the rollback buffer, step the sim, and make the result current.
// Used by both the normal advance loop and rollback re-simulation.
static void process_tick(Tick tick) {
    Input tick_input[MAX_PLAYERS];

    save_state(tick, &state);
    get_inputs(tick, tick_input);
    sim(tick_input, &state, &next_state);

    // memcpy, not `state = next_state`: struct assignment may skip
    // padding bytes, and we want them to stay zero for hashing.
    memcpy(&state, &next_state, sizeof state);
}

// Whether `tick` may be simulated now, given the remote inputs that have
// arrived. The only place the two modes differ.
static bool may_simulate(Tick tick) {
    if (delay_mode) {
        // Every input for the tick is here: no guessing at all. Stricter
        // than can_advance (zero guessed ticks instead of MAX_ROLLBACK).
        return tick <= get_last_confirmed_remote_tick();
    }
    return can_advance(tick);
}

void match_init(int local_player, bool delay, bool skip_lobby, MatchClock clock) {
    now = clock;
    delay_mode = delay;
    next_tick = 0;
    sim_init(&state, skip_lobby);
    memcpy(&next_state, &state, sizeof next_state);
    rollback_init(local_player);
    desync_check_init(&state);
    sync_check_init();
    session_init();

    started = false;
    start_time = 0;
    stalled = false;
    frame_advantage = 0;

    memset(&stats_current, 0, sizeof stats_current);
    memset(&stats_shown, 0, sizeof stats_shown);
    memset(&stats_total, 0, sizeof stats_total);
    stats_window_start = now();
}

void match_frame(Input local_input) {
    double frame_start = now();

    // -------------------------------------------
    // Receive
    // -------------------------------------------
    // Before rollback, so the newest remote inputs are used to fix the
    // past before we build new ticks on top of it.
    // netsim first: it reads the socket and releases packets whose
    // simulated delay is over.
    netsim_update(frame_start);
    session_receive(frame_start);
    if (!started && session_peer_seen()) {
        started = true;
        start_time = frame_start;
        fprintf(stderr, "net: peer connected, starting match\n");
    }
    stats_update(frame_start);

    // Fresh every frame: whether we're stalled depends on what has arrived
    // this frame.
    stalled = false;

    if (started) {
        // Seconds since the match started on this machine.
        double total_time = frame_start - start_time;

        // -------------------------------------------
        // Rollback
        // -------------------------------------------
        Tick rollback_point = find_rollback_point();
        if (rollback_point != NO_TICK) {
            // can_advance keeps guessed ticks to MAX_ROLLBACK, so a deeper
            // rollback means that limit was broken somewhere.
            Tick depth = next_tick - rollback_point;
            assert(depth <= MAX_ROLLBACK);
            stats_note_rollback(depth);

            // Restore the state before the first wrong guess, then
            // re-simulate up to the present with the corrected inputs.
            load_state(rollback_point, &state);
            for (Tick t = rollback_point; t < next_tick; t++) {
                process_tick(t);
            }

            clear_rollback_point();
        }

        // -------------------------------------------
        // Simulate
        // -------------------------------------------
        double elapsed = 0;

        while (
            total_time >= next_tick * STEP - TICK_THRESHOLD
            && elapsed < (STEP - CATCHUP_MARGIN)) {
            // Too many ticks would be running on guessed remote input (in
            // delay mode: any at all): stop advancing this frame. `break`,
            // not `continue`: the time condition is still true, so
            // `continue` would spin here instead of going on to send and
            // render.
            if (!may_simulate(next_tick)) {
                stats_note_stall();
                stalled = true;
                break;
            }

            // Record this frame's sample for the tick it applies to. Every
            // tick run this frame records the same sample.
            write_local_input(next_tick + INPUT_DELAY, local_input);

            process_tick(next_tick);

            elapsed = now() - frame_start;
            next_tick += 1;

            // Every tick, not once per frame: after a long stall one frame
            // can run more ticks than the state ring holds, and the
            // snapshots the check still needs would be overwritten.
            desync_check(next_tick);
            sync_check_after_tick(next_tick);
        }

        // -------------------------------------------
        // Frame advantage
        // -------------------------------------------
        // After simulating, so next_tick means the same as the peer's
        // sender_tick (see session.h). Moving start_time only changes
        // *when* later ticks run, never what they compute, so it can't
        // cause a desync. Only the peer that's ahead slows down.
        frame_advantage = session_frame_advantage(now(), next_tick);

        if (frame_advantage > FRAME_ADVANTAGE_THRESHOLD) {
            start_time += FRAME_ADVANTAGE_NUDGE;
        }
    }

    // -------------------------------------------
    // Send
    // -------------------------------------------
    // Every frame, including while waiting for the peer (an empty packet
    // is what lets the peer see us) and while stalled.
    session_send(now(), next_tick);
    // Pushes out the packet just queued if it has no simulated delay,
    // instead of it waiting until next frame.
    netsim_update(now());
}

double match_time_until_next_frame(void) {
    // Normally: sleep until the next tick is due. That's 0 when we're
    // behind schedule, so the next frame starts right away and keeps
    // catching up.
    // Before the match starts, or while stalled, we're waiting for a
    // packet, not for the clock. The tick schedule is already in the
    // past then, so the normal wait would be 0 and the loop would spin
    // (and send a packet every spin). Wait one step instead.
    if (!started || stalled) {
        return STEP;
    }
    return MAX(0, next_tick * STEP - (now() - start_time));
}

// --------------------------------------------------------
// Accessors
// --------------------------------------------------------

const GameState *match_state(void) {
    return &state;
}

bool match_delay_mode(void) {
    return delay_mode;
}

bool match_started(void) {
    return started;
}

Tick match_next_tick(void) {
    return next_tick;
}

double match_frame_advantage(void) {
    return frame_advantage;
}

MatchStats match_stats_last_second(void) {
    return stats_shown;
}

MatchStats match_stats_total(void) {
    return stats_total;
}
