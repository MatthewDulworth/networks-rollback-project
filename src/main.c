#include "raylib.h"
#include <stdio.h>
#include "sim/sim.h"
#include "rollback/sync_check.h"
#include "net/discovery.h"
#include "net/net.h"
#include "net/netsim.h"
#include "net/session.h"
#include "render/render.h"
#include "args.h"
#include "input.h"
#include "match.h"

// The window, the keyboard and drawing. The netcode for each frame lives in
// match.c, which doesn't use raylib, so tests can run it without a window.

int main(int argc, char **argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) {
        print_usage(argv[0]);
        return 1;
    }
    // Before the window, so a bad address or busy port exits immediately.
    // The overlay shows peer_addr, so each player can read off the address
    // the other one needs to connect to.
    char peer_addr[80];
    if (args.lan) {
        // Discovery waits (in the terminal) until the other machine is
        // found, then tells us its address and which player we are.
        if (!net_open(args.local_port)) {
            return 1;
        }
        NetAddr peer;
        discovery_run(args.local_port, &peer, &args.player);
        if (!net_connect(peer)) {
            return 1;
        }
        net_addr_to_string(peer, peer_addr, sizeof peer_addr);
    } else {
        if (!net_init(args.local_port, args.peer_ip, args.peer_port)) {
            return 1;
        }
        snprintf(peer_addr, sizeof peer_addr, "%s:%u", args.peer_ip, args.peer_port);
    }
    netsim_init();
    netsim_set(args.netsim);

    char local_addr[80];
    net_local_addr(args.local_port, local_addr, sizeof local_addr);

    char title[64];
    snprintf(title, sizeof title, "Rollback Fighter - player %d (%s)%s",
             args.player + 1, args.player == PLAYER_1 ? "blue" : "red",
             args.delay_mode ? " - DELAY MODE" : "");
    InitWindow(MAP_PIXEL_WIDTH, MAP_PIXEL_HEIGHT, title);
    // Two local peers: put player 1 on the left edge, player 2 on the right,
    // so the windows don't start on top of each other.
    int monitor_width = GetMonitorWidth(GetCurrentMonitor());
    SetWindowPosition(args.player == PLAYER_1 ? 0 : monitor_width - MAP_PIXEL_WIDTH, 60);

    match_init(args.player, args.delay_mode, GetTime);

    while (!WindowShouldClose()) {
        // -------------------------------------------
        // Input
        // -------------------------------------------
        // Only the focused window gets key events, so with two local
        // windows click the one you want to play.
        Input local_input = read_local_input();
        handle_netsim_hotkeys();

        // -------------------------------------------
        // Receive, rollback, simulate, send
        // -------------------------------------------
        match_frame(local_input);

        // -------------------------------------------
        // Render
        // -------------------------------------------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        render_game(match_state());

        // Everything the overlay shows, gathered here so the renderer only
        // draws and never reaches into the network or rollback modules.
        MatchStats stats = match_stats_last_second();
        OverlayInfo info = {
            .rtt_ms = session_rtt_ms(),
            .rollbacks = stats.rollbacks,
            .max_depth = stats.max_depth,
            .stalled_frames = stats.stalled_frames,
            .netsim = netsim_get(),
            .netsim_dropped = netsim_dropped_count(),
            .local_addr = local_addr,
            .peer_addr = peer_addr,
            .sync_last_match = sync_check_last_match(),
            .sync_first_desync = sync_check_first_desync(),
            .peer_seen = match_started(),
            .disconnected = session_disconnected(GetTime()),
            .frame_advantage = match_frame_advantage(),
            .delay_mode = match_delay_mode(),
        };
        render_overlay(&info);

        EndDrawing();

        // -------------------------------------------
        // Wait
        // -------------------------------------------
        WaitTime(match_time_until_next_frame());
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
