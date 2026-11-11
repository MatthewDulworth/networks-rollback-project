#include "raylib.h"
#include <string.h>
#include "sim/sim.h"

#define STEP (1.0 / 60.0)
#define CATCHUP_MARGIN (0.004)
#define TICK_THRESHOLD (STEP / 30.0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

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

int main(void) {
    InitWindow(MAP_PIXEL_WIDTH, MAP_PIXEL_HEIGHT, "Rollback Fighter");

    Tick next_tick = 0;

    GameState state;
    GameState next_state;
    sim_init(&state);

    double total_time = 0;

    Input input[MAX_PLAYERS];

    while (!WindowShouldClose()) {
        total_time = GetTime();

        // -------------------------------------------
        // Input
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
        // Simulate
        // -------------------------------------------
        double elapsed = 0;

        while (
            total_time >= next_tick * STEP - TICK_THRESHOLD
            && elapsed < (STEP - CATCHUP_MARGIN)) {
            sim(input, &state, &next_state);
            // memcpy, not `state = next_state`: struct assignment may skip
            // padding bytes, and we want them to stay zero for hashing.
            memcpy(&state, &next_state, sizeof state);

            elapsed = GetTime() - total_time;
            next_tick += 1;
        }

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
