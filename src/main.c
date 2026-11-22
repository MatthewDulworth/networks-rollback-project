#include "raylib.h"
#include <stdio.h>
#include "sim/sim.h"
#include "rollback/sync_check.h"
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

    match_init(args.player, GetTime);

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
