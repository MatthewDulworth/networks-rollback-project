#include "raylib.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "sim/sim.h"
#include "rollback/rollback.h"
#include "rollback/sync_check.h"
#include "rollback/desync_check.h"
#include "net/net.h"
#include "net/netsim.h"
#include "net/session.h"
#include "render/render.h"
#include "args.h"
#include "input.h"

#define STEP (1.0 / 60.0)
#define CATCHUP_MARGIN (0.004)
#define TICK_THRESHOLD (STEP / 30.0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

// --------------------------------------------------------
// Stats for the overlay
// --------------------------------------------------------
// Counted over one-second windows. The overlay shows the last finished
// window, so the numbers update once a second instead of flickering.

typedef struct {
    int rollbacks;       // Rollbacks performed.
    int max_depth;       // Deepest rollback, in ticks.
    int stalled_frames;  // Frames that hit the rollback limit and stopped.
} StatWindow;

static StatWindow stats_current;
static StatWindow stats_shown;
static double stats_window_start;

static void stats_update(double now) {
    if (now - stats_window_start >= 1.0) {
        stats_shown = stats_current;
        memset(&stats_current, 0, sizeof stats_current);
        stats_window_start = now;
    }
}

static void stats_note_rollback(Tick depth) {
    stats_current.rollbacks++;
    if (depth > stats_current.max_depth) {
        stats_current.max_depth = depth;
    }
}

// Run one tick: snapshot the state before it, look up both players' inputs
// from the rollback buffer, step the sim, and make the result current.
// Used by both the normal advance loop and rollback re-simulation.
static void process_tick(Tick tick, GameState *state, GameState *next_state) {
    // Scratch space for this tick only, so it can't overwrite the keyboard
    // sample in main().
    Input tick_input[MAX_PLAYERS];

    save_state(tick, state);
    get_inputs(tick, tick_input);
    sim(tick_input, state, next_state);

    // memcpy, not `*state = *next_state`: struct assignment may skip
    // padding bytes, and we want them to stay zero for hashing.
    memcpy(state, next_state, sizeof *state);
}

int main(int argc, char **argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) {
        print_usage(argv[0]);
        return 1;
    }
    // Before the window, so a bad address or busy port exits immediately.
    if (!net_init(args.local_port, args.peer_ip, args.peer_port)) {
        return 1;
    }
    netsim_init();
    netsim_set(args.netsim);

    // For the overlay, so each player can read off the address the other
    // one needs to connect to.
    char local_ip[64];
    net_local_ip(local_ip, sizeof local_ip);
    char local_addr[80];
    char peer_addr[80];
    snprintf(local_addr, sizeof local_addr, "%s:%u", local_ip, args.local_port);
    snprintf(peer_addr, sizeof peer_addr, "%s:%u", args.peer_ip, args.peer_port);

    char title[64];
    snprintf(title, sizeof title, "Rollback Fighter - player %d (%s)",
             args.player + 1, args.player == PLAYER_1 ? "blue" : "red");
    InitWindow(MAP_PIXEL_WIDTH, MAP_PIXEL_HEIGHT, title);
    // Two local peers: put player 1 on the left edge, player 2 on the right,
    // so the windows don't start on top of each other.
    int monitor_width = GetMonitorWidth(GetCurrentMonitor());
    SetWindowPosition(args.player == PLAYER_1 ? 0 : monitor_width - MAP_PIXEL_WIDTH, 60);

    Tick next_tick = 0;

    GameState state;
    GameState next_state;
    sim_init(&state);
    rollback_init(args.player);
    desync_check_init(&state);
    sync_check_init();
    session_init();

    // Stand-in for the start handshake (see follow_ups.md, "Shared start
    // time"): don't start ticking until the first packet from the peer
    // arrives. Both sides send every frame, so each starts within about one
    // one-way trip of the other. Tick times are measured from start_time.
    bool started = false;
    double start_time = 0;

    Input local_input = 0;

    while (!WindowShouldClose()) {
        double frame_start = GetTime();

        // -------------------------------------------
        // Capture Input
        // -------------------------------------------
        // Only the focused window gets key events, so with two local
        // windows click the one you want to play.
        local_input = read_local_input();
        handle_netsim_hotkeys();

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
            TraceLog(LOG_INFO, "net: peer connected, starting match");
        }
        stats_update(frame_start);

        // Set when can_advance stops the simulate loop. Fresh every frame:
        // whether we're stalled depends on what has arrived this frame.
        bool stalled = false;

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
                // LOG_DEBUG (hidden by default): with simulated lag this
                // happens many times a second. The overlay shows the counts.
                TraceLog(LOG_DEBUG, "rollback: to tick %d, depth %d", rollback_point, depth);
                assert(depth <= MAX_ROLLBACK);
                stats_note_rollback(depth);

                load_state(rollback_point, &state); // Load the state from the rollback point into current state

                // sim back up to the current tick
                for (Tick t = rollback_point; t < next_tick; t++) {
                    process_tick(t, &state, &next_state);
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
                // Too many ticks would be running on guessed remote input: stop
                // advancing this frame. `break`, not `continue`: the time
                // condition is still true, so `continue` would spin here instead
                // of going on to receive, send and render.
                if (!can_advance(next_tick)) {
                    stats_current.stalled_frames++;
                    stalled = true;
                    break;
                }

                // Record this frame's sample for the tick it applies to. Every
                // tick run this frame records the same sample.
                write_local_input(next_tick + INPUT_DELAY, local_input);

                process_tick(next_tick, &state, &next_state);

                elapsed = GetTime() - frame_start;
                next_tick += 1;

                // Every tick, not once per frame: after a long stall one frame
                // can run more ticks than the state ring holds, and the
                // snapshots the check still needs would be overwritten.
                desync_check(next_tick);
                sync_check_after_tick(next_tick);
            }
        }

        // -------------------------------------------
        // Send
        // -------------------------------------------
        // Every frame, including while waiting for the peer (an empty packet
        // is what lets the peer see us) and while stalled.
        session_send(GetTime(), next_tick);
        // Pushes out the packet just queued if it has no simulated delay,
        // instead of it waiting until next frame.
        netsim_update(GetTime());

        // -------------------------------------------
        // Render
        // -------------------------------------------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        render_game(&state);

        // Everything the overlay shows, gathered here so the renderer only
        // draws and never reaches into the network or rollback modules.
        OverlayInfo info = {
            .rtt_ms = session_rtt_ms(),
            .rollbacks = stats_shown.rollbacks,
            .max_depth = stats_shown.max_depth,
            .stalled_frames = stats_shown.stalled_frames,
            .netsim = netsim_get(),
            .netsim_dropped = netsim_dropped_count(),
            .local_addr = local_addr,
            .peer_addr = peer_addr,
            .sync_last_match = sync_check_last_match(),
            .sync_first_desync = sync_check_first_desync(),
            .peer_seen = started,
            .disconnected = session_disconnected(frame_start),
        };
        render_overlay(&info);

        EndDrawing();

        // Normally: sleep until the next tick is due. That's 0 when we're
        // behind schedule, so the next frame starts right away and keeps
        // catching up.
        // Before the match starts, or while stalled, we're waiting for a
        // packet, not for the clock. The tick schedule is already in the
        // past then, so the normal wait would be 0 and the loop would spin
        // (and send a packet every spin). Wait one step instead.
        double time_til_next_tick = started && !stalled
            ? MAX(0, next_tick * STEP - (GetTime() - start_time))
            : STEP;
        WaitTime(time_til_next_tick);
        // EndDrawing() already polled input, but that was before the wait.
        // Poll again so input read at the top of the loop is fresh.
        // Polling twice per frame breaks IsKeyPressed/IsKeyReleased (the
        // second poll overwrites the "previous" key state), so only use
        // IsKeyDown and derive press/release in the sim from previous_input.
        PollInputEvents();
    }

    net_shutdown();
    CloseWindow();
    return 0;
}
