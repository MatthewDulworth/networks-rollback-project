#pragma once

// Drawing with raylib. Only reads: nothing here can change the game state,
// so floats and anything else non-deterministic are fine.

#include <stdbool.h>
#include "sim/sim.h"
#include "net/netsim.h"  // NetsimSettings

// Map, bullets, players and the HUD (health, ammo, score, round banners).
void render_game(const GameState *state);

// The ready-up panel, drawn on top of render_game while state->in_lobby.
// Only call it once the peer is connected: before that no ticks run, so
// pressing ready would do nothing (the overlay's waiting banner shows then).
void render_lobby(const GameState *state);

// Everything the debug overlay shows. Filled in by main each frame, so the
// renderer doesn't need to know which module each number comes from.
typedef struct {
    double rtt_ms;  // -1 before the first sample
    int rollbacks;
    int max_depth;
    int stalled_frames;
    NetsimSettings netsim;
    int netsim_dropped;
    const char *local_addr;
    const char *peer_addr;
    Tick sync_last_match;    // NO_TICK if nothing compared yet
    Tick sync_first_desync;  // NO_TICK if no desync
    bool peer_seen;
    bool disconnected;
    double frame_advantage;  // ticks ahead of the peer; 0 before an estimate
    bool delay_mode;         // delay-based netcode instead of rollback
    bool show_stats;         // false hides the stats; banners always show
} OverlayInfo;

// Debug overlay (top left) and the waiting / disconnected banners. Drawn
// straight to the screen at its real resolution, not into the scaled game
// canvas, so the text stays sharp and small in fullscreen.
void render_overlay(const OverlayInfo *info);
