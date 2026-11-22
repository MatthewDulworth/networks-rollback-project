#include "sim.h"
#include <string.h>

// Everything in this file must be deterministic: the same prev state and
// inputs must give the exact same next state, byte for byte, on every machine.
// So: integers only (no float/double), no rand(), no reading the clock, and
// loops always go in index order.

// --------------------------------------------------------
// Tuning (sim-only)
// --------------------------------------------------------

// All units are subpixels (1 pixel = SUB_PIXEL_FACTOR). Velocities are
// subpixels per tick, GRAVITY is subpixels per tick per tick. Times are ticks
// (60 per second).
#define GRAVITY        (SUB_PIXEL_FACTOR / 4)      // 0.25 px/tick^2
#define MOVE_SPEED     (3 * SUB_PIXEL_FACTOR)      // 3 px/tick
#define JUMP_VELOCITY  (-7 * SUB_PIXEL_FACTOR)     // negative = up
#define MAX_SPEED      (12 * SUB_PIXEL_FACTOR)     // per axis

#define SHOOT_STARTUP_TICKS  6    // from pressing fire to the bullet appearing
#define SHOOT_COOLDOWN_TICKS 20   // after a bullet appears, before the next press
#define BULLET_SPEED         (8 * SUB_PIXEL_FACTOR)
#define BULLET_LIFETIME      90

// 181/256 is about 1/sqrt(2). Diagonal bullets use this per axis so they
// travel at the same overall speed as straight ones. Integer math, so both
// machines get exactly the same result.
#define DIAGONAL_SPEED (BULLET_SPEED * 181 / 256)

#define HITSTUN_TICKS        20
#define KNOCKBACK_UP         (2 * SUB_PIXEL_FACTOR)   // extra upward pop on hit

#define ROUND_OVER_TICKS     90   // pause after a round
#define MATCH_OVER_TICKS     240  // longer pause when someone wins the match

// --------------------------------------------------------
// Map data (declared extern in sim.h)
// --------------------------------------------------------

#define PX(n) ((n) * SUB_PIXEL_FACTOR)

#define FLOOR_TOP PX(430)

// A floor with gaps at both sides (fall in and you die), a platform over each
// gap, and one in the middle. All solid from every side.
const Collision MapCollision[NUM_MAP_OBJ] = {
    { {PX(100), FLOOR_TOP}, {PX(600), PX(20)} },  // floor
    { {PX(30),  PX(350)},   {PX(170), PX(10)} },  // left platform
    { {PX(600), PX(350)},   {PX(170), PX(10)} },  // right platform
    { {PX(320), PX(270)},   {PX(160), PX(10)} },  // middle platform
};

// The whole map. A player or bullet not touching it at all is "outside the
// map": players die, bullets disappear.
static const Collision MapBounds = { {0, 0}, {MAP_WIDTH, MAP_HEIGHT} };

// --------------------------------------------------------
// Helpers (file-private)
// --------------------------------------------------------

static bool colliding(Collision A, Collision B) {
    return (
        (A.origin.x < B.origin.x + B.bounds.x)
        && (A.origin.x + A.bounds.x > B.origin.x)
        && (A.origin.y < B.origin.y + B.bounds.y)
        && (A.origin.y + A.bounds.y > B.origin.y)
    );
}

static bool touching_terrain(Collision box) {
    for (int i = 0; i < NUM_MAP_OBJ; i++) {
        if (colliding(box, MapCollision[i])) return true;
    }
    return false;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static Collision player_box(const Player *p) {
    Collision box = { p->position, {PLAYER_WIDTH, PLAYER_HEIGHT} };
    return box;
}

static Collision bullet_box(const Bullet *b) {
    Collision box = { b->position, {BULLET_SIZE, BULLET_SIZE} };
    return box;
}

// Move the player along X by velocity.x, then push it out of any terrain it
// ended up inside. Only X changes here, so any overlap must have come from
// X movement and the direction of travel says which side to snap to.
static void move_x(Player *p) {
    p->position.x += p->velocity.x;

    for (int i = 0; i < NUM_MAP_OBJ; i++) {
        const Collision *wall = &MapCollision[i];
        if (!colliding(player_box(p), *wall)) continue;

        if (p->velocity.x > 0) {
            p->position.x = wall->origin.x - PLAYER_WIDTH;
        } else if (p->velocity.x < 0) {
            p->position.x = wall->origin.x + wall->bounds.x;
        }
        p->velocity.x = 0;
    }
}

// Same as move_x but for Y. Being pushed up out of terrain while falling
// means we landed.
static void move_y(Player *p) {
    p->position.y += p->velocity.y;
    p->grounded = false;

    for (int i = 0; i < NUM_MAP_OBJ; i++) {
        const Collision *wall = &MapCollision[i];
        if (!colliding(player_box(p), *wall)) continue;

        if (p->velocity.y > 0) {
            p->position.y = wall->origin.y - PLAYER_HEIGHT;
            p->grounded = true;
        } else if (p->velocity.y < 0) {
            p->position.y = wall->origin.y + wall->bounds.y;
        }
        p->velocity.y = 0;
    }
}

// -1, 0 or +1 from a pair of opposite input bits. Both held cancel out.
static int axis(Input in, Input negative, Input positive) {
    return ((in & positive) ? 1 : 0) - ((in & negative) ? 1 : 0);
}

// Bullet velocity for the direction the player is aiming. Uses the aim keys;
// if none are held, the movement keys; if none of those, the way the player
// is facing.
static IntVector2 aim_velocity(const Player *p, Input in) {
    int dx = axis(in, INPUT_AIM_LEFT, INPUT_AIM_RIGHT);
    int dy = axis(in, INPUT_AIM_UP, INPUT_AIM_DOWN);
    if (dx == 0 && dy == 0) {
        dx = axis(in, INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT);
        dy = axis(in, INPUT_MOVE_UP, INPUT_MOVE_DOWN);
    }
    if (dx == 0 && dy == 0) {
        dx = p->facing_right ? 1 : -1;
    }
    int32_t speed = (dx != 0 && dy != 0) ? DIAGONAL_SPEED : BULLET_SPEED;
    IntVector2 v = { dx * speed, dy * speed };
    return v;
}

// Put a bullet in the first free slot. If all MAX_BULLETS are in use the shot
// is lost (can't happen with 2 players and these fire rates, but it must
// still behave the same on both machines if it ever does).
static void spawn_bullet(GameState *s, int owner, const Player *p) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &s->bullets[i];
        if (b->active) continue;

        // Field by field, not `*b = (Bullet){...}`: struct assignment may
        // copy junk into the padding bytes, and the desync check compares
        // the whole state byte for byte.
        b->active = true;
        b->owner = (uint8_t)owner;
        b->position.x = p->position.x + PLAYER_WIDTH / 2 - BULLET_SIZE / 2;
        b->position.y = p->position.y + PLAYER_HEIGHT / 2 - BULLET_SIZE / 2;
        b->velocity = p->queued_bullet_velocity;
        b->lifetime_left = BULLET_LIFETIME;
        return;
    }
}

// Shooting: press fire to start the startup, the bullet appears when it ends.
static void update_gun(GameState *s, int index, Input in, Input prev_in) {
    Player *p = &s->players[index];

    if (p->shoot_cooldown_left > 0) p->shoot_cooldown_left--;

    // Reloading continues even in hitstun.
    if (p->reload_frames_left > 0) {
        p->reload_frames_left--;
        if (p->reload_frames_left == 0) {
            p->ammo = MAGAZINE_SIZE;
        }
    }

    // A shot in startup always comes out, even if the player got hit
    // during it.
    if (p->shoot_startup_left > 0) {
        p->shoot_startup_left--;
        if (p->shoot_startup_left == 0) {
            spawn_bullet(s, index, p);
            p->shoot_cooldown_left = SHOOT_COOLDOWN_TICKS;
            p->ammo--;
            if (p->ammo == 0) {
                p->reload_frames_left = RELOAD_TICKS;  // automatic reload
            }
        }
        return;
    }

    bool shoot_pressed = (in & INPUT_SHOOT) && !(prev_in & INPUT_SHOOT);
    bool can_shoot = p->hitstun_left == 0
        && p->shoot_cooldown_left == 0
        && p->reload_frames_left == 0
        && p->ammo > 0;
    if (shoot_pressed && can_shoot) {
        // Aim is locked in now, on the press, not when the bullet appears.
        p->queued_bullet_velocity = aim_velocity(p, in);
        p->shoot_startup_left = SHOOT_STARTUP_TICKS;
    }
}

static void update_player(GameState *s, int index, Input in, Input prev_in) {
    Player *p = &s->players[index];

    if (p->hitstun_left == 0) {
        int dir = axis(in, INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT);
        p->velocity.x = dir * MOVE_SPEED;
        if (dir != 0) p->facing_right = (dir > 0);

        // Jump on the tick the button goes down, not while held.
        bool jump_pressed = (in & INPUT_JUMP) && !(prev_in & INPUT_JUMP);
        if (jump_pressed && p->grounded) {
            p->velocity.y = JUMP_VELOCITY;
        }
    } else {
        // In hitstun input is ignored and the knockback carries the player,
        // slowing a little each tick (1/16 of the speed). C integer division
        // rounds toward zero, the same on every machine.
        p->velocity.x -= p->velocity.x / 16;
        p->hitstun_left--;
    }

    p->velocity.y += GRAVITY;

    p->velocity.x = clamp_i32(p->velocity.x, -MAX_SPEED, MAX_SPEED);
    p->velocity.y = clamp_i32(p->velocity.y, -MAX_SPEED, MAX_SPEED);

    move_x(p);
    move_y(p);

    update_gun(s, index, in, prev_in);
}

// Move bullets, remove the ones that hit terrain, leave the map or run out of
// lifetime, and apply hits to the other player.
static void update_bullets(GameState *s) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &s->bullets[i];
        if (!b->active) continue;

        b->position.x += b->velocity.x;
        b->position.y += b->velocity.y;
        b->lifetime_left--;

        // Bullets move at most 8 px per tick and are 4 px big, so they can't
        // skip through a 10 px player or platform in one step.
        Collision box = bullet_box(b);
        if (b->lifetime_left <= 0 || !colliding(box, MapBounds)
            || touching_terrain(box)) {
            b->active = false;
            continue;
        }

        int target = 1 - b->owner;  // no self-damage
        Player *p = &s->players[target];
        if (colliding(box, player_box(p))) {
            b->active = false;
            p->health--;
            p->hitstun_left = HITSTUN_TICKS;
            // Knocked in the bullet's direction at 3/4 of its speed, plus a
            // small pop upward so grounded players actually move.
            p->velocity.x = b->velocity.x * 3 / 4;
            p->velocity.y = b->velocity.y * 3 / 4 - KNOCKBACK_UP;
        }
    }
}

static bool is_dead(const Player *p) {
    return p->health <= 0 || !colliding(player_box(p), MapBounds);
}

// Put both players back at their spawns with full health and ammo, and clear
// every bullet. Keeps tick, kills, round number and previous_input.
static void reset_round(GameState *s) {
    // memset first: zeroes every field we don't set below (timers,
    // velocity...) and the padding bytes, for byte-for-byte comparison.
    memset(s->players, 0, sizeof s->players);
    memset(s->bullets, 0, sizeof s->bullets);

    // On the floor, a quarter of the map in from each side, facing each other.
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &s->players[i];
        p->health = MAX_HEALTH;
        p->ammo = MAGAZINE_SIZE;
        p->position.y = FLOOR_TOP - PLAYER_HEIGHT;
        p->grounded = true;
    }
    s->players[PLAYER_1].position.x = MAP_WIDTH / 4 - PLAYER_WIDTH / 2;
    s->players[PLAYER_1].facing_right = true;
    s->players[PLAYER_2].position.x = 3 * MAP_WIDTH / 4 - PLAYER_WIDTH / 2;
    s->players[PLAYER_2].facing_right = false;
}

// After everything has moved: if anyone died, end the round. Both dying on
// the same tick is a tie and nobody scores.
static void check_round_end(GameState *s) {
    bool dead[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        dead[i] = is_dead(&s->players[i]);
    }
    if (!dead[PLAYER_1] && !dead[PLAYER_2]) {
        return;
    }

    if (dead[PLAYER_1] && dead[PLAYER_2]) {
        s->round_winner = -1;
    } else {
        int winner = dead[PLAYER_1] ? PLAYER_2 : PLAYER_1;
        s->round_winner = (int8_t)winner;
        s->kills[winner]++;
    }
    s->round_over_time_left =
        sim_match_winner(s) >= 0 ? MATCH_OVER_TICKS : ROUND_OVER_TICKS;
}

// --------------------------------------------------------
// Public
// --------------------------------------------------------

void sim_init(GameState *state) {
    // memset rather than `= {0}`: it also zeroes struct padding bytes, so the
    // state can later be hashed/compared byte-for-byte for desync detection.
    memset(state, 0, sizeof *state);
    state->round_winner = -1;
    reset_round(state);
}

int sim_match_winner(const GameState *state) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->kills[i] >= KILLS_TO_WIN) return i;
    }
    return -1;
}

void sim(const Input input[MAX_PLAYERS], const GameState *restrict prev, GameState *restrict next) {
    // Start from an exact copy of the previous state, then change only what
    // this tick changes. Read "before" values from prev, write to next.
    memcpy(next, prev, sizeof *next);
    next->tick += 1;

    if (next->round_over_time_left > 0) {
        // Between rounds: everything is frozen until the timer runs out.
        next->round_over_time_left--;
        if (next->round_over_time_left == 0) {
            if (sim_match_winner(next) >= 0) {
                // Match over: start a new one.
                memset(next->kills, 0, sizeof next->kills);
                next->round_number = 0;
            } else {
                next->round_number++;
            }
            next->round_winner = -1;
            reset_round(next);
        }
    } else {
        // Players first (may fire new bullets), then bullets (may hit
        // players), then deaths. Fixed order, the same on both machines.
        for (int i = 0; i < MAX_PLAYERS; i++) {
            update_player(next, i, input[i], prev->previous_input[i]);
        }
        update_bullets(next);
        check_round_end(next);
    }

    // Recorded even while frozen, so a button held through the pause doesn't
    // count as a fresh press when the round starts.
    for (int i = 0; i < MAX_PLAYERS; i++) {
        next->previous_input[i] = input[i];
    }
}
