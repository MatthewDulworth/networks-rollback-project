#include "raylib.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "sim/sim.h"

#define STEP (1.0 / 60.0)
#define CATCHUP_MARGIN (0.004)
#define TICK_THRESHOLD (STEP / 30.0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))

int main(void) {
    InitWindow(800, 450, "Rollback Fighter");

    Tick next_tick = 0;

    // memset rather than `= {0}`: it also zeroes struct padding bytes, so the
    // state can later be hashed/compared byte-for-byte for desync detection.
    GameState state;
    memset(&state, 0, sizeof(state));
    double total_time = 0;
    double last_frame_time = 0;

    while (!WindowShouldClose()) {
        last_frame_time = total_time;
        total_time = GetTime();
        double delta = total_time - last_frame_time;

        // -------------------------------------------
        // Input
        // -------------------------------------------
        Input input = 0;
        if (IsKeyDown(KEY_A)) input |= INPUT_MOVE_LEFT;
        if (IsKeyDown(KEY_D)) input |= INPUT_MOVE_RIGHT;
        if (IsKeyDown(KEY_W)) input |= INPUT_MOVE_UP;
        if (IsKeyDown(KEY_S)) input |= INPUT_MOVE_DOWN;

        // -------------------------------------------
        // Simulate
        // -------------------------------------------
        double elapsed = 0;
        int ticks_this_frame = 0;

        while (
            total_time >= next_tick * STEP - TICK_THRESHOLD 
            && elapsed < (STEP - CATCHUP_MARGIN)) {
            // input = get_input(next_tick)
            // state = sim(input, state)
            elapsed = GetTime() - total_time;
            next_tick += 1;
            ticks_this_frame +=1;
        }

        // -------------------------------------------
        // Render
        // -------------------------------------------
        char msg[50];

        BeginDrawing();
        ClearBackground(RAYWHITE);
        
        snprintf(msg, sizeof(msg), "Delta: %.4f. Total: %.2f", delta, total_time);
        DrawText(msg, 190, 200, 20, BLACK);

        snprintf(msg, sizeof(msg), "Tick: %" PRIu32 ". Diff: %.4f", next_tick, total_time - (next_tick * STEP));
        DrawText(msg, 190, 230, 20, BLACK);

        if (ticks_this_frame != 1) {
            printf("Current Tick: %" PRIu32 ", Time: %.4f, Ticks: %d\n", next_tick, total_time, ticks_this_frame);
        }

        // render(state)

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
