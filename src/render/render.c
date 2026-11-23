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

static const Color player_colors[MAX_PLAYERS] = { BLUE, RED };
// Darker shades of player_colors, so the eye shows against the body.
static const Color eye_colors[MAX_PLAYERS] = {
    { 0, 40, 100, 255 },   // darker than raylib's DARKBLUE (0, 82, 172)
    { 110, 15, 30, 255 },  // darker than raylib's MAROON (190, 33, 55)
};

// Aim arrow: one chevron per round in the magazine, in a row pointing where
// the next shot goes. Loaded rounds are filled, spent ones are outlines.
#define AIM_RADIUS 12.0f         // px from the player's center to the first chevron
#define AIM_PLANT_OFFSET 4.0f    // pushed out this much while planted
#define AIM_ARROW_LENGTH 4.0f
#define AIM_ARROW_HALF_WIDTH 3.0f
#define AIM_ARROW_SPACING 5.0f   // px between chevrons along the aim
#define AIM_SHOT_TRAVEL 20.0f    // px the firing chevron moves during startup

// Chevron with its base `radius` px from `center`, pointing along unit `d`.
static void draw_chevron(Vector2 center, Vector2 d, float radius, bool filled, Color c) {
    Vector2 n = { -d.y, d.x };  // perpendicular to d
    Vector2 base = { center.x + d.x * radius, center.y + d.y * radius };
    Vector2 tip = { base.x + d.x * AIM_ARROW_LENGTH, base.y + d.y * AIM_ARROW_LENGTH };
    Vector2 left = { base.x - n.x * AIM_ARROW_HALF_WIDTH, base.y - n.y * AIM_ARROW_HALF_WIDTH };
    Vector2 right = { base.x + n.x * AIM_ARROW_HALF_WIDTH, base.y + n.y * AIM_ARROW_HALF_WIDTH };
    // raylib only fills triangles given counter-clockwise (on screen).
    if (filled) {
        DrawTriangle(tip, left, right, c);
    } else {
        DrawTriangleLines(tip, left, right, c);
    }
}

// Unit vector (in px) along a bullet velocity. One of 8 directions.
static Vector2 unit_direction(IntVector2 v) {
    float x = (float)((v.x > 0) - (v.x < 0));
    float y = (float)((v.y > 0) - (v.y < 0));
    float scale = (x != 0 && y != 0) ? 0.7071f : 1.0f;
    Vector2 d = { x * scale, y * scale };
    return d;
}

// Aim and ammo for player i, as a row of MAGAZINE_SIZE chevrons:
// - Points where a shot fired now would go (from the player's latest input),
//   or during startup, the locked-in aim.
// - Loaded rounds are filled, innermost first; spent rounds are outlines.
//   While reloading they fill up one by one as the reload progresses.
// - Faint when the player can't fire (hitstun, cooldown, reloading).
// - During startup the outermost loaded chevron flies out along the aim and
//   the bullet appears when it's gone the full distance.
// - Pushed out a little while planted.
static void draw_aim(const GameState *state, int i, Vector2 center) {
    const Player *p = &state->players[i];
    Input in = state->previous_input[i];
    bool in_startup = p->shoot_startup_left > 0;

    IntVector2 aim = in_startup ? p->queued_bullet_velocity : sim_aim_velocity(p, in);
    Vector2 d = unit_direction(aim);

    bool planted = (in & INPUT_PLANT) && p->grounded;
    float first = AIM_RADIUS + (planted ? AIM_PLANT_OFFSET : 0.0f);

    Color c = player_colors[i];
    if (!in_startup && !sim_can_shoot(p)) c = Fade(c, 0.35f);

    int loaded = p->ammo;
    if (p->reload_frames_left > 0) {
        float done = 1.0f - (float)p->reload_frames_left / RELOAD_TICKS;
        loaded = (int)(done * MAGAZINE_SIZE);
    }

    // The round being fired: ammo only goes down when the bullet appears.
    int firing = in_startup ? p->ammo - 1 : -1;

    for (int k = 0; k < MAGAZINE_SIZE; k++) {
        float radius = first + k * AIM_ARROW_SPACING;
        draw_chevron(center, d, radius, k < loaded && k != firing, c);
    }

    if (in_startup) {
        float done = 1.0f - (float)p->shoot_startup_left / (SHOOT_STARTUP_TICKS + 1);
        float radius = first + firing * AIM_ARROW_SPACING + AIM_SHOT_TRAVEL * done;
        draw_chevron(center, d, radius, true, c);
    }
}

// Crack on a damaged player's body, like a hit on glass: a few lines
// radiating from an impact point, joined by a broken ring part way out.
// Points are fractions of the body's width and height, minus a 1 px
// border so the 1 px lines (centered on their points) stay inside the body.
static void draw_crack(Rectangle body, Color c) {
    Rectangle r = { body.x + 1, body.y + 1, body.width - 2, body.height - 2 };
    const Vector2 impact = { 0.45f, 0.35f };
    const Vector2 ray_ends[] = {
        { 0.05f, 0.10f }, { 0.95f, 0.05f }, { 1.00f, 0.55f },
        { 0.70f, 1.00f }, { 0.00f, 0.70f },
    };
    // How far along each ray the ring crosses it. Uneven so it isn't a
    // perfect circle.
    const float ring_at[] = { 0.50f, 0.40f, 0.45f, 0.35f, 0.55f };
    // Ring segments join rays k and k+1 (wrapping). Only some are drawn,
    // so the ring looks broken.
    const bool ring_drawn[] = { true, false, true, true, false };
    const int n = sizeof ray_ends / sizeof ray_ends[0];

    Vector2 center = { r.x + impact.x * r.width, r.y + impact.y * r.height };
    Vector2 ring[sizeof ray_ends / sizeof ray_ends[0]];
    for (int k = 0; k < n; k++) {
        Vector2 end = { r.x + ray_ends[k].x * r.width, r.y + ray_ends[k].y * r.height };
        DrawLineEx(center, end, 1.0f, c);
        ring[k] = (Vector2){
            center.x + (end.x - center.x) * ring_at[k],
            center.y + (end.y - center.y) * ring_at[k],
        };
    }
    for (int k = 0; k < n; k++) {
        if (ring_drawn[k]) DrawLineEx(ring[k], ring[(k + 1) % n], 1.0f, c);
    }
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

    // Portals: one colour per linked pair (pairs are adjacent, so i / 2),
    // with a light stripe on the open side so you can tell which way it faces.
    const Color pair_colors[] = { ORANGE, PURPLE, GREEN, SKYBLUE, RED, RED, GOLD };
    const int num_colors = sizeof pair_colors / sizeof pair_colors[0];
    for (int i = 0; i < NUM_PORTALS; i++) {
        const Portal *portal = &MapPortals[i];
        Rectangle r = {
            to_px(portal->box.origin.x), to_px(portal->box.origin.y),
            to_px(portal->box.bounds.x), to_px(portal->box.bounds.y),
        };
        DrawRectangleRec(r, pair_colors[(i / 2) % num_colors]);

        Rectangle stripe = r;
        switch (portal->facing) {
        case FACING_UP:    stripe.height = 2; break;
        case FACING_DOWN:  stripe.y += r.height - 2; stripe.height = 2; break;
        case FACING_LEFT:  stripe.width = 2; break;
        case FACING_RIGHT: stripe.x += r.width - 2; stripe.width = 2; break;
        }
        DrawRectangleRec(stripe, Fade(WHITE, 0.7f));
    }

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
        // Damaged (one hit left): faded and cracked. Flicker while in
        // hitstun so hits are easy to see.
        bool damaged = p->health < MAX_HEALTH;
        bool flicker = p->hitstun_left > 0 && (state->tick / 3) % 2 == 0;
        float alpha = damaged ? 0.5f : 1.0f;
        if (flicker) alpha *= 0.3f;
        DrawRectangleRec(r, Fade(player_colors[i], alpha));
        if (damaged) draw_crack(r, Fade(BLACK, 0.7f));

        // Eye on the facing side so direction is visible: inside the body,
        // flush with the edge it faces.
        const float eye_width = 5, eye_height = 3;
        float eye_x = p->facing_right ? r.x + r.width - eye_width : r.x;
        DrawRectangleRec((Rectangle){ eye_x, r.y + 2, eye_width, eye_height }, eye_colors[i]);

        Vector2 center = { r.x + r.width / 2, r.y + r.height / 2 };
        draw_aim(state, i, center);
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

// The debug stats, top left. Hidden and shown with Tab.
static void render_stats(const OverlayInfo *info) {
    // Pale backing panel: in fullscreen the stats can land on the black
    // letterbox bar, where dark gray text would be unreadable. Sized to fit
    // the lines below.
    DrawRectangle(0, 0, 600, 134, Fade(RAYWHITE, 0.85f));

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
    DrawText("keys: 1/2 lag   3/4 jitter   5/6 loss   0 reset   Enter fullscreen   Tab hide stats", 10, y, 10, GRAY);
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
}

void render_overlay(const OverlayInfo *info) {
    if (info->show_stats) {
        render_stats(info);
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
        // Screen size, not map size: the overlay is drawn at the window's
        // real resolution, which in fullscreen is much bigger than the map.
        DrawText(banner, (GetScreenWidth() - width) / 2, GetScreenHeight() / 2 - 15, 30,
                 info->disconnected ? RED : GRAY);
    }
}

