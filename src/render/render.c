#include "render/render.h"
#include "raylib.h"
#include "rollback/rollback.h"  // NO_TICK

// --------------------------------------------------------
// Game
// --------------------------------------------------------

// Sim units are subpixels; raylib draws in pixels. Floats are fine here:
// rendering only reads the state, so it can't cause a desync.
static float to_px(int32_t subpixels) {
    return (float)subpixels / SUB_PIXEL_FACTOR;
}

void render_game(const GameState *state) {
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
// Overlay
// --------------------------------------------------------

void render_overlay(const OverlayInfo *info) {
    int y = 10;
    // Ping, then frame advantage on the same line: positive means this
    // machine is ahead of the peer (and slowing down to let it catch up).
    if (info->rtt_ms >= 0) {
        DrawText(TextFormat("ping %d ms   advantage %+.1f",
                            (int)(info->rtt_ms + 0.5), info->frame_advantage),
                 10, y, 20, DARKGRAY);
    } else {
        DrawText("ping --", 10, y, 20, DARKGRAY);
    }
    y += 22;
    // Mode first on the line, so it's obvious which netcode a side-by-side
    // comparison is looking at. Delay mode never rolls back, so its line is
    // all about stalls.
    if (info->delay_mode) {
        DrawText(TextFormat("DELAY MODE   stalls/s %d", info->stalled_frames),
                 10, y, 20, MAROON);
    } else {
        DrawText(TextFormat("rollbacks/s %d   max depth %d   stalls/s %d",
                            info->rollbacks, info->max_depth, info->stalled_frames),
                 10, y, 20, DARKGRAY);
    }
    y += 22;

    const NetsimSettings *s = &info->netsim;
    DrawText(TextFormat("sim lag %d ms   jitter %d ms   loss %d%%   (dropped %d)",
                        s->lag_ms, s->jitter_ms, s->loss_pct, info->netsim_dropped),
             10, y, 20, s->lag_ms || s->jitter_ms || s->loss_pct ? MAROON : DARKGRAY);
    y += 24;
    DrawText("keys: 1/2 lag   3/4 jitter   5/6 loss   0 reset", 10, y, 10, GRAY);
    y += 14;
    DrawText(TextFormat("me %s   peer %s", info->local_addr, info->peer_addr), 10, y, 10, GRAY);
    y += 14;

    // Cross-machine hash check. Gray until the first comparison, green while
    // every compared tick has matched, red for good once one hasn't.
    if (info->sync_first_desync != NO_TICK) {
        DrawText(TextFormat("DESYNC at tick %d", info->sync_first_desync), 10, y, 20, RED);
    } else if (info->sync_last_match != NO_TICK) {
        DrawText(TextFormat("sync OK (tick %d)", info->sync_last_match), 10, y, 20, DARKGREEN);
    } else {
        DrawText("sync --", 10, y, 20, GRAY);
    }

    // Centered banners for states where the game isn't running normally.
    const char *banner = NULL;
    if (!info->peer_seen) {
        banner = "Waiting for peer...";
    } else if (info->disconnected) {
        banner = "Peer disconnected";
    }
    if (banner != NULL) {
        int width = MeasureText(banner, 30);
        DrawText(banner, (MAP_PIXEL_WIDTH - width) / 2, MAP_PIXEL_HEIGHT / 2 - 15, 30,
                 info->disconnected ? RED : GRAY);
    }
}

