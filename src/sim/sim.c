#include "sim.h"
#include <string.h>

// --------------------------------------------------------
// Tuning (sim-only)
// --------------------------------------------------------

// All units are subpixels (1 pixel = SUB_PIXEL_FACTOR). Velocities are
// subpixels per tick, GRAVITY is subpixels per tick per tick.
#define GRAVITY        (SUB_PIXEL_FACTOR / 4)      // 0.25 px/tick^2
#define MOVE_SPEED     (3 * SUB_PIXEL_FACTOR)      // 3 px/tick
#define JUMP_VELOCITY  (-7 * SUB_PIXEL_FACTOR)     // negative = up
#define MAX_SPEED      (12 * SUB_PIXEL_FACTOR)     // per axis

// --------------------------------------------------------
// Map data (declared extern in sim.h)
// --------------------------------------------------------

#define FLOOR_HEIGHT (20 * SUB_PIXEL_FACTOR)
#define FLOOR_TOP    (MAP_HEIGHT - FLOOR_HEIGHT)

// Floor: full map width, sitting at the bottom of the map.
const Collision MapCollision[NUM_MAP_OBJ] = {
    {
        {0, FLOOR_TOP},
        {MAP_WIDTH, FLOOR_HEIGHT}
    },
};

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

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static Collision player_box(const Player *p) {
    Collision box = { p->position, {PLAYER_WIDTH, PLAYER_HEIGHT} };
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

static void update_player(Player *p, Input in, Input prev_in) {
    // Input -> intent. Ignored in hitstun so knockback velocity is kept.
    if (p->hitstun_left == 0) {
        int dir = 0;
        if (in & INPUT_MOVE_LEFT)  dir -= 1;
        if (in & INPUT_MOVE_RIGHT) dir += 1;

        p->velocity.x = dir * MOVE_SPEED;
        if (dir != 0) p->facing_right = (dir > 0);

        // Jump on the tick the button goes down, not while held.
        bool jump_pressed = (in & INPUT_JUMP) && !(prev_in & INPUT_JUMP);
        if (jump_pressed && p->grounded) {
            p->velocity.y = JUMP_VELOCITY;
        }
    }

    p->velocity.y += GRAVITY;

    p->velocity.x = clamp_i32(p->velocity.x, -MAX_SPEED, MAX_SPEED);
    p->velocity.y = clamp_i32(p->velocity.y, -MAX_SPEED, MAX_SPEED);

    move_x(p);
    move_y(p);
}

// --------------------------------------------------------
// Public
// --------------------------------------------------------

void sim_init(GameState *state) {
    // memset rather than `= {0}`: it also zeroes struct padding bytes, so the
    // state can later be hashed/compared byte-for-byte for desync detection.
    memset(state, 0, sizeof *state);

    // Standing on the floor, a quarter of the map in from each side,
    // facing each other.
    Player *p1 = &state->players[PLAYER_1];
    p1->position.x = MAP_WIDTH / 4 - PLAYER_WIDTH / 2;
    p1->position.y = FLOOR_TOP - PLAYER_HEIGHT;
    p1->facing_right = true;

    Player *p2 = &state->players[PLAYER_2];
    p2->position.x = 3 * MAP_WIDTH / 4 - PLAYER_WIDTH / 2;
    p2->position.y = FLOOR_TOP - PLAYER_HEIGHT;
    p2->facing_right = false;
}

void sim(const Input input[MAX_PLAYERS], const GameState *restrict prev, GameState *restrict next) {
    // Start from an exact copy of the previous state, then change only what
    // this tick changes. Read "before" values from prev, write to next.
    memcpy(next, prev, sizeof *next);
    next->tick += 1;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        update_player(&next->players[i], input[i], prev->previous_input[i]);
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        next->previous_input[i] = input[i];
    }
}