#include "input.h"
#include "raylib.h"
#include "net/netsim.h"
#include <assert.h>
#include <string.h>

// --------------------------------------------------------
// Player input
// --------------------------------------------------------

Input read_local_input(void) {
    Input input = 0;
    if (IsKeyDown(KEY_A)) input |= INPUT_MOVE_LEFT;
    if (IsKeyDown(KEY_D)) input |= INPUT_MOVE_RIGHT;
    if (IsKeyDown(KEY_W)) input |= INPUT_MOVE_UP;
    if (IsKeyDown(KEY_S)) input |= INPUT_MOVE_DOWN;
    if (IsKeyDown(KEY_SPACE)) input |= INPUT_JUMP;
    if (IsKeyDown(KEY_LEFT))  input |= INPUT_AIM_LEFT;
    if (IsKeyDown(KEY_RIGHT)) input |= INPUT_AIM_RIGHT;
    if (IsKeyDown(KEY_UP))    input |= INPUT_AIM_UP;
    if (IsKeyDown(KEY_DOWN))  input |= INPUT_AIM_DOWN;
    // J, or right shift which sits next to the arrow keys on a laptop.
    if (IsKeyDown(KEY_J) || IsKeyDown(KEY_RIGHT_SHIFT)) input |= INPUT_SHOOT;
    return input;
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
// comment at PollInputEvents in main.c).
static bool key_went_down(int key) {
    static bool was_down[512];
    assert(key >= 0 && key < 512);
    bool down = IsKeyDown(key);
    bool went_down = down && !was_down[key];
    was_down[key] = down;
    return went_down;
}

void handle_netsim_hotkeys(void) {
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
// Window hotkeys
// --------------------------------------------------------

// The debug stats start visible: during development they're what you want.
static bool stats_visible = true;

bool input_stats_visible(void) {
    return stats_visible;
}

void handle_window_hotkeys(void) {
    if (key_went_down(KEY_TAB)) stats_visible = !stats_visible;

    // Borderless windowed rather than ToggleFullscreen: true fullscreen
    // changes the monitor's resolution, which is slow and flickery (especially
    // on macOS). Borderless just grows the window to cover the screen.
    if (key_went_down(KEY_ENTER)) ToggleBorderlessWindowed();
}
