#include "raylib.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "sim/sim.h"
#include "rollback/rollback.h"
#include "net/net.h"
#include "net/packet.h"

#define STEP (1.0 / 60.0)
#define CATCHUP_MARGIN (0.004)
#define TICK_THRESHOLD (STEP / 30.0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

// --------------------------------------------------------
// Command line
// --------------------------------------------------------

typedef struct {
    uint16_t local_port;
    const char *peer_ip;
    uint16_t peer_port;
    int player;  // PLAYER_1 or PLAYER_2; must be opposite on the two peers
} Args;

static bool parse_port(const char *text, uint16_t *out) {
    char *end;
    long value = strtol(text, &end, 10);
    // end == text: no digits at all. *end != '\0': junk after the number.
    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool parse_args(int argc, char **argv, Args *args) {
    if (argc != 5) {
        return false;
    }
    args->peer_ip = argv[2];
    if (!parse_port(argv[1], &args->local_port)
        || !parse_port(argv[3], &args->peer_port)) {
        return false;
    }
    if (strcmp(argv[4], "0") == 0) {
        args->player = PLAYER_1;
    } else if (strcmp(argv[4], "1") == 0) {
        args->player = PLAYER_2;
    } else {
        return false;
    }
    return true;
}

// --------------------------------------------------------
// Network glue: rollback module <-> packets <-> net module
// --------------------------------------------------------

// Drain every waiting packet into the rollback module. Returns true if at
// least one valid packet arrived.
static bool receive_packets(void) {
    uint8_t buf[PACKET_MAX_SIZE];
    bool got_any = false;
    int len;

    while ((len = net_receive(buf, sizeof buf)) > 0) {
        InputPacket packet;
        if (!packet_decode(buf, (size_t)len, &packet)) {
            TraceLog(LOG_WARNING, "net: dropped malformed packet (%d bytes)", len);
            continue;
        }
        got_any = true;

        receive_ack(packet.ack);
        // Every packet repeats all inputs the peer hasn't seen acked, so
        // most of these are duplicates. add_remote_input ignores those (and
        // anything past a gap), so no filtering is needed here.
        for (int i = 0; i < packet.count; i++) {
            add_remote_input(packet.first_tick + i, packet.inputs[i]);
        }
    }
    return got_any;
}

// Send our ack and every local input the peer hasn't acked. Called once per
// frame, even when nothing advanced: after a lost packet this resend is the
// only way the peer ever gets those inputs.
static void send_packet(void) {
    InputPacket packet;
    packet.ack = get_last_confirmed_remote_tick();
    find_inputs_to_resend(&packet.first_tick, &packet.count);

    // Keep the oldest inputs if there are too many: the peer needs them in
    // order, and the rest go out once these are acked.
    if (packet.count > PACKET_MAX_INPUTS) {
        packet.count = PACKET_MAX_INPUTS;
    }
    for (int i = 0; i < packet.count; i++) {
        packet.inputs[i] = get_local_input(packet.first_tick + i);
    }

    uint8_t buf[PACKET_MAX_SIZE];
    size_t len = packet_encode(&packet, buf, sizeof buf);
    assert(len > 0);  // count is clamped and buf is max size, so can't fail
    net_send(buf, len);
}

// --------------------------------------------------------
// Desync check (testing only)
// --------------------------------------------------------
// A second copy of the game that never guesses: it only runs a tick once both
// players' inputs for it are confirmed. Rollback promises that once the inputs
// are known, its snapshot for a tick is exactly what you'd get by simulating
// with the real inputs from the start. This checks that promise byte for byte.
static GameState reference_state;

static void desync_check_init(const GameState *start) {
    memcpy(&reference_state, start, sizeof reference_state);
}

// Call after every tick that advances (never before this frame's rollback), so
// every saved snapshot reflects all remote inputs received so far, and the
// reference never falls further behind than the state ring reaches.
static void desync_check(Tick next_tick) {
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
            TraceLog(LOG_ERROR, "DESYNC at tick %d", t);
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

// Sim units are subpixels; raylib draws in pixels. Floats are fine here:
// rendering only reads the state, so it can't cause a desync.
static float to_px(int32_t subpixels) {
    return (float)subpixels / SUB_PIXEL_FACTOR;
}

static void render(const GameState *state) {
    for (int i = 0; i < NUM_MAP_OBJ; i++) {
        const Collision *obj = &MapCollision[i];
        Rectangle r = {
            to_px(obj->origin.x), to_px(obj->origin.y),
            to_px(obj->bounds.x), to_px(obj->bounds.y),
        };
        DrawRectangleRec(r, DARKGRAY);
    }

    const Color player_colors[MAX_PLAYERS] = { BLUE, RED };
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &state->players[i];
        Rectangle r = {
            to_px(p->position.x), to_px(p->position.y),
            to_px(PLAYER_WIDTH), to_px(PLAYER_HEIGHT),
        };
        DrawRectangleRec(r, player_colors[i]);

        // Small mark on the facing side so direction is visible.
        float mark_x = p->facing_right ? r.x + r.width : r.x - 3;
        DrawRectangleRec((Rectangle){ mark_x, r.y + 2, 3, 3 }, BLACK);
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
        fprintf(stderr,
                "usage: %s <local_port> <peer_ip> <peer_port> <player 0|1>\n"
                "  e.g. %s 5000 127.0.0.1 5001 0   (and on the peer: 5001 127.0.0.1 5000 1)\n",
                argv[0], argv[0]);
        return 1;
    }
    // Before the window, so a bad address or busy port exits immediately.
    if (!net_init(args.local_port, args.peer_ip, args.peer_port)) {
        return 1;
    }

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

    // Stand-in for the start handshake (see follow_ups.md, "Shared start
    // time"): don't start ticking until the first packet from the peer
    // arrives. Both sides send every frame, so each starts within about one
    // one-way trip of the other. Tick times are measured from start_time.
    bool peer_seen = false;
    double start_time = 0;

    Input local_input = 0;

    while (!WindowShouldClose()) {
        double frame_start = GetTime();

        // -------------------------------------------
        // Capture Input
        // -------------------------------------------
        // Each peer reads one keyboard, whichever player it controls. Only
        // the focused window gets key events, so with two local windows
        // click the one you want to play.
        local_input = 0;
        if (IsKeyDown(KEY_A)) local_input |= INPUT_MOVE_LEFT;
        if (IsKeyDown(KEY_D)) local_input |= INPUT_MOVE_RIGHT;
        if (IsKeyDown(KEY_W)) local_input |= INPUT_MOVE_UP;
        if (IsKeyDown(KEY_S)) local_input |= INPUT_MOVE_DOWN;
        if (IsKeyDown(KEY_SPACE)) local_input |= INPUT_JUMP;

        // -------------------------------------------
        // Receive
        // -------------------------------------------
        // Before rollback, so the newest remote inputs are used to fix the
        // past before we build new ticks on top of it.
        if (receive_packets() && !peer_seen) {
            peer_seen = true;
            start_time = frame_start;
            TraceLog(LOG_INFO, "net: peer connected, starting match");
        }

        if (peer_seen) {
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
                TraceLog(LOG_INFO, "rollback: to tick %d, depth %d", rollback_point, depth);
                assert(depth <= MAX_ROLLBACK);

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
            }
        }

        // -------------------------------------------
        // Send
        // -------------------------------------------
        // Every frame, including while waiting for the peer (an empty packet
        // is what lets the peer see us) and while stalled.
        send_packet();

        // -------------------------------------------
        // Render
        // -------------------------------------------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        render(&state);
        if (!peer_seen) {
            DrawText("Waiting for peer...", 20, 20, 20, GRAY);
        }

        EndDrawing();

        // Before the match starts there are no ticks to line up with, so
        // just wait one step between frames.
        double time_til_next_tick = peer_seen
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
