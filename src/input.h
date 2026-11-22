#pragma once

// Keyboard handling. Only IsKeyDown is used: input is polled twice per frame
// (see PollInputEvents in main.c), which breaks raylib's IsKeyPressed.

#include "sim/sim.h"

// Sample the keyboard into this machine's Input bitmask. Each peer reads one
// keyboard, whichever player it controls.
Input read_local_input(void);

// Number keys change the network simulator live: 1/2 lag, 3/4 jitter,
// 5/6 loss, 0 resets everything.
void handle_netsim_hotkeys(void);
