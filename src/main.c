#include "raylib.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "sim/sim.h"
#include "rollback/rollback.h"
#include "rollback/sync_check.h"
#include "net/net.h"
#include "net/netsim.h"
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
    NetsimSettings netsim;  // Optional --lag/--jitter/--loss, default all 0
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

// A non-negative whole number with nothing after it.
static bool parse_non_negative(const char *text, int *out) {
    char *end;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 100000) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_args(int argc, char **argv, Args *args) {
    // Four positional arguments, then any number of "--flag value" pairs.
    if (argc < 5) {
        return false;
    }
    memset(&args->netsim, 0, sizeof args->netsim);
    for (int i = 5; i < argc; i += 2) {
        if (i + 1 >= argc) {
            return false;  // flag with no value after it
        }
        int *field;
        if (strcmp(argv[i], "--lag") == 0) {
            field = &args->netsim.lag_ms;
        } else if (strcmp(argv[i], "--jitter") == 0) {
            field = &args->netsim.jitter_ms;
        } else if (strcmp(argv[i], "--loss") == 0) {
            field = &args->netsim.loss_pct;
        } else {
            return false;
        }
        if (!parse_non_negative(argv[i + 1], field)) {
            return false;
        }
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

// --------------------------------------------------------
// Round-trip time, measured from acks
// --------------------------------------------------------
// When a local input goes out for the first time, remember when. When the
// peer's ack first covers that tick, the time since then is one round trip.
//
// This measures more than the pure network delay: the peer only acks when it
// next sends (end of its frame), and we only read the ack at the start of our
// next frame, so up to a frame on each side is included. A lost packet also
// shows up as a spike, because the ack waits for the resend. That's fine for
// an overlay: it's the delay the game actually experiences.

// Must cover every unacked tick, and the rollback input ring is 32 slots, so
// 64 is plenty. Power of 2 so the slot is a mask, as in rollback.c.
#define SEND_TIME_RING 64

typedef struct {
    Tick tick;
    double time;
} SendTime;

static SendTime send_times[SEND_TIME_RING];
static Tick newest_timed_tick = NO_TICK;  // Newest tick with a send time.
static Tick newest_rtt_ack = NO_TICK;     // Newest ack already measured.
static double rtt_ms = -1;                // Smoothed; -1 until first sample.

static SendTime *send_time_slot(Tick tick) {
    return &send_times[(uint32_t)tick & (SEND_TIME_RING - 1)];
}

// Called for every tick in an outgoing packet. Only the first send counts.
static void note_sent(Tick tick, double now) {
    if (tick > newest_timed_tick) {
        SendTime *slot = send_time_slot(tick);
        slot->tick = tick;
        slot->time = now;
        newest_timed_tick = tick;
    }
}

static void note_ack(Tick ack, double now) {
    if (ack <= newest_rtt_ack) {
        return;  // old or duplicate ack: already measured
    }
    newest_rtt_ack = ack;

    SendTime *slot = send_time_slot(ack);
    if (slot->tick != ack) {
        return;  // a prefilled tick we never sent, or an overwritten slot
    }
    double sample = (now - slot->time) * 1000.0;
    // Exponential moving average: each sample moves the estimate 10% of the
    // way. Smooths jitter so the number on screen is readable.
    rtt_ms = rtt_ms < 0 ? sample : rtt_ms * 0.9 + sample * 0.1;
}

// Drain every waiting packet into the rollback module. Returns true if at
// least one valid packet arrived.
static bool receive_packets(double now) {
    uint8_t buf[PACKET_MAX_SIZE];
    bool got_any = false;
    int len;

    while ((len = netsim_receive(buf, sizeof buf)) > 0) {
        InputPacket packet;
        if (!packet_decode(buf, (size_t)len, &packet)) {
            TraceLog(LOG_WARNING, "net: dropped malformed packet (%d bytes)", len);
            continue;
        }
        got_any = true;

        receive_ack(packet.ack);
        note_ack(packet.ack, now);
        sync_check_add_remote(packet.hash_tick, packet.hash);
        // packet.sender_tick is unused for now: it's for frame advantage
        // sync (mvp_plan.md step 8).
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
static void send_packet(double now, Tick next_tick) {
    InputPacket packet;
    packet.ack = get_last_confirmed_remote_tick();
    packet.sender_tick = next_tick;
    // Only the newest hash. One per packet is enough: packets go out about
    // once per tick, and a hash that's lost just means that tick isn't
    // compared.
    sync_check_newest_local(&packet.hash_tick, &packet.hash);
    find_inputs_to_resend(&packet.first_tick, &packet.count);

    // Keep the oldest inputs if there are too many: the peer needs them in
    // order, and the rest go out once these are acked.
    if (packet.count > PACKET_MAX_INPUTS) {
        packet.count = PACKET_MAX_INPUTS;
    }
    for (int i = 0; i < packet.count; i++) {
        packet.inputs[i] = get_local_input(packet.first_tick + i);
        note_sent(packet.first_tick + i, now);
    }

    uint8_t buf[PACKET_MAX_SIZE];
    size_t len = packet_encode(&packet, buf, sizeof buf);
    assert(len > 0);  // count is clamped and buf is max size, so can't fail
    netsim_send(buf, len);
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

// --------------------------------------------------------
// Confirmed state hashes (cross-machine sync check)
// --------------------------------------------------------
// Snapshot t (the state before tick t) is final once:
// - every input it depends on (ticks 0 .. t-1) is confirmed, so it can never
//   be rolled back again: t - 1 <= last confirmed remote tick, and
// - it has been saved, which happens when tick t is simulated: t < next_tick.
// Hash each snapshot once it's final and hand it to sync_check.
//
// Same timing rule as desync_check: call after every tick that advances, so
// any rollback for inputs received this frame has already been applied, and
// the snapshot is still in the state ring.
static Tick next_hash_tick = 0;  // Oldest snapshot not hashed yet.

static void hash_confirmed_states(Tick next_tick) {
    Tick last_confirmed = get_last_confirmed_remote_tick();
    while (next_hash_tick <= last_confirmed + 1 && next_hash_tick < next_tick) {
        GameState snapshot;
        load_state(next_hash_tick, &snapshot);
        sync_check_add_local(next_hash_tick, sync_hash_state(&snapshot));
        next_hash_tick++;
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
    const char *player_names[MAX_PLAYERS] = { "Blue", "Red" };

    for (int i = 0; i < MAX_BULLETS; i++) {
        const Bullet *b = &state->bullets[i];
        if (!b->active) continue;
        Rectangle r = {
            to_px(b->position.x), to_px(b->position.y),
            to_px(BULLET_SIZE), to_px(BULLET_SIZE),
        };
        DrawRectangleRec(r, player_colors[b->owner]);
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &state->players[i];
        Rectangle r = {
            to_px(p->position.x), to_px(p->position.y),
            to_px(PLAYER_WIDTH), to_px(PLAYER_HEIGHT),
        };
        // Flicker while in hitstun so hits are easy to see.
        bool flicker = p->hitstun_left > 0 && (state->tick / 3) % 2 == 0;
        DrawRectangleRec(r, flicker ? Fade(player_colors[i], 0.3f) : player_colors[i]);

        // Small mark on the facing side so direction is visible.
        float mark_x = p->facing_right ? r.x + r.width : r.x - 3;
        DrawRectangleRec((Rectangle){ mark_x, r.y + 2, 3, 3 }, BLACK);

        float cx = r.x + r.width / 2;
        float cy = r.y + r.height / 2;

        // During shot startup, a line shows where the bullet will go.
        if (p->shoot_startup_left > 0) {
            Vector2 dir = {
                to_px(p->queued_bullet_velocity.x), to_px(p->queued_bullet_velocity.y),
            };
            DrawLineEx((Vector2){ cx, cy },
                       (Vector2){ cx + dir.x * 3, cy + dir.y * 3 }, 2, player_colors[i]);
        }

        // Health: one pip per point, above the player.
        for (int h = 0; h < MAX_HEALTH; h++) {
            Color c = h < p->health ? player_colors[i] : LIGHTGRAY;
            DrawRectangleRec((Rectangle){ cx - 8 + h * 6, r.y - 8, 4, 4 }, c);
        }
        // Ammo: dots under the player, or a bar while reloading.
        if (p->reload_frames_left > 0) {
            float done = 1.0f - (float)p->reload_frames_left / RELOAD_TICKS;
            DrawRectangleRec((Rectangle){ cx - 8, r.y + r.height + 4, 16, 2 }, LIGHTGRAY);
            DrawRectangleRec((Rectangle){ cx - 8, r.y + r.height + 4, 16 * done, 2 }, DARKGRAY);
        } else {
            for (int a = 0; a < p->ammo; a++) {
                DrawRectangleRec((Rectangle){ cx - 7 + a * 5, r.y + r.height + 4, 3, 3 }, DARKGRAY);
            }
        }
    }

    // Score, top right (the debug overlay is top left).
    const char *score = TextFormat("%d - %d", state->kills[PLAYER_1], state->kills[PLAYER_2]);
    int score_width = MeasureText(score, 30);
    int score_x = MAP_PIXEL_WIDTH - score_width - 20;
    DrawText(score, score_x, 10, 30, DARKGRAY);
    DrawRectangle(score_x - 16, 20, 10, 10, player_colors[PLAYER_1]);
    DrawRectangle(score_x + score_width + 6, 20, 10, 10, player_colors[PLAYER_2]);

    // Between rounds: who scored, or who won the match.
    if (state->round_over_time_left > 0) {
        int match_winner = sim_match_winner(state);
        const char *text;
        if (match_winner >= 0) {
            text = TextFormat("%s wins the match!", player_names[match_winner]);
        } else if (state->round_winner >= 0) {
            text = TextFormat("%s scores", player_names[state->round_winner]);
        } else {
            text = "Tie - no point";
        }
        int width = MeasureText(text, 40);
        DrawText(text, (MAP_PIXEL_WIDTH - width) / 2, 150, 40, DARKGRAY);
    }
}

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

// --------------------------------------------------------
// Netsim hotkeys
// --------------------------------------------------------
// Number keys, not F-keys: on a Mac laptop F-keys need fn held down.
#define LAG_STEP_MS 20
#define JITTER_STEP_MS 10
#define LOSS_STEP_PCT 5

// True on the frame a key goes down. Built from IsKeyDown because input is
// polled twice per frame, which breaks raylib's IsKeyPressed (see the
// comment at PollInputEvents in main).
static bool key_went_down(int key) {
    static bool was_down[512];
    assert(key >= 0 && key < 512);
    bool down = IsKeyDown(key);
    bool went_down = down && !was_down[key];
    was_down[key] = down;
    return went_down;
}

static void handle_netsim_hotkeys(void) {
    NetsimSettings s = netsim_get();
    if (key_went_down(KEY_ONE))   s.lag_ms -= LAG_STEP_MS;
    if (key_went_down(KEY_TWO))   s.lag_ms += LAG_STEP_MS;
    if (key_went_down(KEY_THREE)) s.jitter_ms -= JITTER_STEP_MS;
    if (key_went_down(KEY_FOUR))  s.jitter_ms += JITTER_STEP_MS;
    if (key_went_down(KEY_FIVE))  s.loss_pct -= LOSS_STEP_PCT;
    if (key_went_down(KEY_SIX))   s.loss_pct += LOSS_STEP_PCT;
    if (key_went_down(KEY_ZERO))  memset(&s, 0, sizeof s);
    netsim_set(s);  // clamps, so going below 0 or above the max is harmless
}

// --------------------------------------------------------
// Overlay
// --------------------------------------------------------

// No packet for this long means the peer is probably gone (closed the game,
// lost Wi-Fi). Long enough that a short Wi-Fi hiccup doesn't trigger it.
#define DISCONNECT_TIMEOUT 3.0

static void render_overlay(const char *local_addr, const char *peer_addr,
                           bool peer_seen, bool disconnected) {
    int y = 10;
    if (rtt_ms >= 0) {
        DrawText(TextFormat("ping %d ms", (int)(rtt_ms + 0.5)), 10, y, 20, DARKGRAY);
    } else {
        DrawText("ping --", 10, y, 20, DARKGRAY);
    }
    y += 22;
    DrawText(TextFormat("rollbacks/s %d   max depth %d   stalls/s %d",
                        stats_shown.rollbacks, stats_shown.max_depth,
                        stats_shown.stalled_frames),
             10, y, 20, DARKGRAY);
    y += 22;

    NetsimSettings s = netsim_get();
    DrawText(TextFormat("sim lag %d ms   jitter %d ms   loss %d%%   (dropped %d)",
                        s.lag_ms, s.jitter_ms, s.loss_pct, netsim_dropped_count()),
             10, y, 20, s.lag_ms || s.jitter_ms || s.loss_pct ? MAROON : DARKGRAY);
    y += 24;
    DrawText("keys: 1/2 lag   3/4 jitter   5/6 loss   0 reset", 10, y, 10, GRAY);
    y += 14;
    DrawText(TextFormat("me %s   peer %s", local_addr, peer_addr), 10, y, 10, GRAY);
    y += 14;

    // Cross-machine hash check. Gray until the first comparison, green while
    // every compared tick has matched, red for good once one hasn't.
    Tick desync = sync_check_first_desync();
    Tick match = sync_check_last_match();
    if (desync != NO_TICK) {
        DrawText(TextFormat("DESYNC at tick %d", desync), 10, y, 20, RED);
    } else if (match != NO_TICK) {
        DrawText(TextFormat("sync OK (tick %d)", match), 10, y, 20, DARKGREEN);
    } else {
        DrawText("sync --", 10, y, 20, GRAY);
    }

    // Centered banners for states where the game isn't running normally.
    const char *banner = NULL;
    if (!peer_seen) {
        banner = "Waiting for peer...";
    } else if (disconnected) {
        banner = "Peer disconnected";
    }
    if (banner != NULL) {
        int width = MeasureText(banner, 30);
        DrawText(banner, (MAP_PIXEL_WIDTH - width) / 2, MAP_PIXEL_HEIGHT / 2 - 15, 30,
                 disconnected ? RED : GRAY);
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
                "usage: %s <local_port> <peer_ip> <peer_port> <player 0|1>"
                " [--lag ms] [--jitter ms] [--loss pct]\n"
                "  e.g. %s 5000 127.0.0.1 5001 0   (and on the peer: 5001 127.0.0.1 5000 1)\n"
                "  --lag/--jitter/--loss simulate a bad connection (both directions)\n",
                argv[0], argv[0]);
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

    // Stand-in for the start handshake (see follow_ups.md, "Shared start
    // time"): don't start ticking until the first packet from the peer
    // arrives. Both sides send every frame, so each starts within about one
    // one-way trip of the other. Tick times are measured from start_time.
    bool peer_seen = false;
    double start_time = 0;
    double last_packet_time = 0;

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
        if (IsKeyDown(KEY_LEFT))  local_input |= INPUT_AIM_LEFT;
        if (IsKeyDown(KEY_RIGHT)) local_input |= INPUT_AIM_RIGHT;
        if (IsKeyDown(KEY_UP))    local_input |= INPUT_AIM_UP;
        if (IsKeyDown(KEY_DOWN))  local_input |= INPUT_AIM_DOWN;
        // J, or right shift which sits next to the arrow keys on a laptop.
        if (IsKeyDown(KEY_J) || IsKeyDown(KEY_RIGHT_SHIFT)) local_input |= INPUT_SHOOT;

        handle_netsim_hotkeys();

        // -------------------------------------------
        // Receive
        // -------------------------------------------
        // Before rollback, so the newest remote inputs are used to fix the
        // past before we build new ticks on top of it.
        // netsim first: it reads the socket and releases packets whose
        // simulated delay is over.
        netsim_update(frame_start);
        if (receive_packets(frame_start)) {
            last_packet_time = frame_start;
            if (!peer_seen) {
                peer_seen = true;
                start_time = frame_start;
                TraceLog(LOG_INFO, "net: peer connected, starting match");
            }
        }
        // Display only: if packets start arriving again, the game just
        // carries on (stalled at the rollback limit until then).
        bool disconnected = peer_seen
            && frame_start - last_packet_time > DISCONNECT_TIMEOUT;
        stats_update(frame_start);

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
                hash_confirmed_states(next_tick);
            }
        }

        // -------------------------------------------
        // Send
        // -------------------------------------------
        // Every frame, including while waiting for the peer (an empty packet
        // is what lets the peer see us) and while stalled.
        send_packet(GetTime(), next_tick);
        // Pushes out the packet just queued if it has no simulated delay,
        // instead of it waiting until next frame.
        netsim_update(GetTime());

        // -------------------------------------------
        // Render
        // -------------------------------------------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        render(&state);
        render_overlay(local_addr, peer_addr, peer_seen, disconnected);

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
