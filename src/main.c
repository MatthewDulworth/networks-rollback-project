#include "raylib.h"
#include <string.h>
#include <assert.h>
#include "sim/sim.h"
#include "rollback/rollback.h"

#define STEP (1.0 / 60.0)
#define CATCHUP_MARGIN (0.004)
#define TICK_THRESHOLD (STEP / 30.0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

// --------------------------------------------------------
// Fake remote (testing only, replaced by UDP later)
// --------------------------------------------------------
// Player 2's keyboard input is treated as if it came from the network: each
// input is held back for FAKE_LATENCY seconds before add_remote_input sees it.
// Try 0 (no rollbacks), ~0.08 (visible rollbacks) and ~0.25 (stalls).
#define FAKE_LATENCY (0.080)

// Inputs "in flight". Only filled while ticks advance, so it holds about
// FAKE_LATENCY / STEP entries. 64 covers up to ~1 s of latency.
#define FAKE_QUEUE_SIZE 64

typedef struct {
    Tick tick;
    Input input;
    double arrive_at;
} FakePacket;

// FIFO: latency is constant, so packets arrive in the order they were sent.
static FakePacket fake_queue[FAKE_QUEUE_SIZE];
static int fake_head = 0;  // index of the oldest packet
static int fake_count = 0;

static void fake_remote_send(Tick tick, Input input, double now) {
    assert(fake_count < FAKE_QUEUE_SIZE && "fake queue full: latency too high");
    int tail = (fake_head + fake_count) % FAKE_QUEUE_SIZE;
    fake_queue[tail] = (FakePacket){ tick, input, now + FAKE_LATENCY };
    fake_count++;
}

// Hand every packet whose arrival time has passed to the rollback module.
static void fake_remote_receive(double now) {
    while (fake_count > 0 && fake_queue[fake_head].arrive_at <= now) {
        add_remote_input(fake_queue[fake_head].tick, fake_queue[fake_head].input);
        fake_head = (fake_head + 1) % FAKE_QUEUE_SIZE;
        fake_count--;
    }
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

// Call after rollback and the advance loop, so every saved snapshot reflects
// all remote inputs received so far.
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

int main(void) {
    InitWindow(MAP_PIXEL_WIDTH, MAP_PIXEL_HEIGHT, "Rollback Fighter");

    Tick next_tick = 0;

    GameState state;
    GameState next_state;
    sim_init(&state);
    rollback_init(PLAYER_1); // player Needs to be set at runtime once we have two clients
    desync_check_init(&state);

    double total_time = 0;

    Input input[MAX_PLAYERS];

    while (!WindowShouldClose()) {
        total_time = GetTime();

        // -------------------------------------------
        // Capture Input
        // -------------------------------------------
        input[PLAYER_1] = 0;
        if (IsKeyDown(KEY_A)) input[PLAYER_1] |= INPUT_MOVE_LEFT;
        if (IsKeyDown(KEY_D)) input[PLAYER_1] |= INPUT_MOVE_RIGHT;
        if (IsKeyDown(KEY_W)) input[PLAYER_1] |= INPUT_MOVE_UP;
        if (IsKeyDown(KEY_S)) input[PLAYER_1] |= INPUT_MOVE_DOWN;
        if (IsKeyDown(KEY_SPACE)) input[PLAYER_1] |= INPUT_JUMP;


        input[PLAYER_2] = 0;
        if (IsKeyDown(KEY_L)) input[PLAYER_2] |= INPUT_MOVE_LEFT;
        if (IsKeyDown(KEY_APOSTROPHE)) input[PLAYER_2] |= INPUT_MOVE_RIGHT;
        if (IsKeyDown(KEY_P)) input[PLAYER_2] |= INPUT_MOVE_UP;
        if (IsKeyDown(KEY_SEMICOLON)) input[PLAYER_2] |= INPUT_MOVE_DOWN;
        if (IsKeyDown(KEY_COMMA)) input[PLAYER_2] |= INPUT_JUMP;

        // -------------------------------------------
        // Receive
        // -------------------------------------------
        // Before rollback, so the newest remote inputs are used to fix the
        // past before we build new ticks on top of it.
        fake_remote_receive(GetTime());

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
            write_local_input(next_tick + INPUT_DELAY, input[PLAYER_1]);
            fake_remote_send(next_tick + INPUT_DELAY, input[PLAYER_2], GetTime());

            process_tick(next_tick, &state, &next_state);

            elapsed = GetTime() - total_time;
            next_tick += 1;
        }

        desync_check(next_tick);

        // -------------------------------------------
        // Send Inputs
        // -------------------------------------------
        // later

        // -------------------------------------------
        // Render
        // -------------------------------------------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        render(&state);

        EndDrawing();

        double time_til_next_tick = MAX(0, next_tick * STEP - GetTime());
        WaitTime(time_til_next_tick);
        // EndDrawing() already polled input, but that was before the wait.
        // Poll again so input read at the top of the loop is fresh.
        // Polling twice per frame breaks IsKeyPressed/IsKeyReleased (the
        // second poll overwrites the "previous" key state), so only use
        // IsKeyDown and derive press/release in the sim from previous_input.
        PollInputEvents();
    }

    CloseWindow();
    return 0;
}
