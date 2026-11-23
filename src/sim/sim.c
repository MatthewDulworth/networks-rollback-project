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

// Anything leaving a portal moves out of it at least this fast. Without it,
// walking onto a floor portal (entering at gravity speed, 0.25 px/tick) comes
// out of the partner just as slowly, falls straight back in next tick and
// flickers between the two. 4 px/tick out of a floor portal is a 32 px hop.
#define MIN_EXIT_SPEED (4 * SUB_PIXEL_FACTOR)

#define COYOTE_TICKS      6  // jump still allowed this long after leaving ground
#define JUMP_BUFFER_TICKS 6  // an early jump press is remembered this long

// Horizontal speed changes by at most this much per tick, in subpixels per
// tick per tick. Three cases:
// - ACCEL: pushing toward walking speed (or turning around).
// - FRICTION: not pushing, at or below walking speed (stopping).
// - DRAG: faster than walking speed and not holding the opposite way (from
//   knockback, later portals). Much gentler, so extra momentum bleeds off
//   gradually instead of vanishing.
// Walking is snappy (ACCEL and FRICTION are high); only the extra speed
// behaves physically.
#define GROUND_ACCEL    (SUB_PIXEL_FACTOR * 3 / 2) // 0 -> full speed in 2 ticks
#define GROUND_FRICTION (SUB_PIXEL_FACTOR * 3 / 2) // full speed -> 0 in 2 ticks
#define GROUND_DRAG     (SUB_PIXEL_FACTOR / 2)     // 12 px/tick -> walking in 18 ticks
#define AIR_ACCEL       (SUB_PIXEL_FACTOR)         // 3 ticks
#define AIR_FRICTION    (SUB_PIXEL_FACTOR / 2)     // 6 ticks
#define AIR_DRAG        (SUB_PIXEL_FACTOR / 16)    // 144 ticks

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

// Mirrored left/right, on four levels plus the floor, with a tower in the
// middle. Numbers on the left are the y of each level's top surface.
//
//   100 ==[v]=                                  =[v]==   perches
//   190 ========                              ========   high ledges
//   270          ==[^]==     [tower]     ==[^]==         T2 platforms
//   350 ======                                  ======   low ledges
//   430 >=========================[<]##[>]============<  floor
//
// [<] [>]: doors in the tower's base. [v]: ceiling portals under the
// perches. [^]: trampoline portals in notches in the T2 platforms.
// > <: floor-level wrap-around at the walls.
//
// Each level is at most 90 px above one you can jump from (jump: 98 px).
// All boxes are solid from every side.
const Collision MapCollision[NUM_MAP_OBJ] = {
    { {PX(0),   FLOOR_TOP}, {PX(800), PX(20)} },  // floor
    // Walls, down to where the wrap-around portals start.
    { {PX(0),   PX(0)},     {PX(12),  PX(390)} },
    { {PX(788), PX(0)},     {PX(12),  PX(390)} },
    // Tower: a core, plus the two strips above the doors. The doors are
    // notches in the tower's sides (see "Backing" below).
    { {PX(382), PX(270)},   {PX(36),  PX(160)} },
    { {PX(370), PX(270)},   {PX(12),  PX(120)} },
    { {PX(418), PX(270)},   {PX(12),  PX(120)} },
    // Low ledges.
    { {PX(0),   PX(350)},   {PX(120), PX(10)} },
    { {PX(680), PX(350)},   {PX(120), PX(10)} },
    // T2 platforms, 20 px thick: two halves either side of a 40 px notch,
    // and a backing strip under the notch.
    { {PX(180), PX(270)},   {PX(35),  PX(20)} },
    { {PX(255), PX(270)},   {PX(35),  PX(20)} },
    { {PX(215), PX(282)},   {PX(40),  PX(8)} },
    { {PX(510), PX(270)},   {PX(35),  PX(20)} },
    { {PX(585), PX(270)},   {PX(35),  PX(20)} },
    { {PX(545), PX(282)},   {PX(40),  PX(8)} },
    // High ledges.
    { {PX(0),   PX(190)},   {PX(160), PX(10)} },
    { {PX(640), PX(190)},   {PX(160), PX(10)} },
    // Perches. Climb up from the outer end of the high ledge, past the
    // ceiling portal hanging underneath.
    { {PX(0),   PX(100)},   {PX(100), PX(10)} },
    { {PX(700), PX(100)},   {PX(100), PX(10)} },
};

// 12 px thick, the same as MAX_SPEED. Two reasons:
// - No tunneling: nothing moves more than 12 px per tick, so nothing can
//   jump over a portal in one tick.
// - Backing: a portal in the middle of the map needs solid terrain behind
//   it, or you could go round and fall through its back. That terrain sits
//   right behind the 12 px box. On the tick something first overlaps the
//   portal it has moved at most 12 px into it, so it never also touches the
//   backing. That matters, because move_x/move_y would push it out of the
//   backing and zero its velocity before try_portal ran.
#define PORTAL_THICKNESS PX(12)

// try_portal keeps whatever comes out inside the exit's span, so the only
// placement rule is: open air directly in front of each exit, at least one
// player deep.
const Portal MapPortals[NUM_PORTALS] = {
    // Doors and ceilings, crossed (a quarter turn). Walk into the tower's
    // left door and you drop out of the ceiling under the RIGHT perch,
    // onto the right high ledge: a fast way up, onto the other side.
    // Backwards: jump from the high ledge up into that ceiling and you
    // dash out of the left door, your jump speed turned sideways. Bullets
    // work the same way: shoot the door and it rains on the high ledge.
    { { {PX(370), PX(390)}, {PORTAL_THICKNESS, PX(40)} }, FACING_LEFT },
    { { {PX(730), PX(110)}, {PX(40), PORTAL_THICKNESS} }, FACING_DOWN },
    // The mirror image: right door <-> ceiling under the left perch.
    { { {PX(418), PX(390)}, {PORTAL_THICKNESS, PX(40)} }, FACING_RIGHT },
    { { {PX(30),  PX(110)}, {PX(40), PORTAL_THICKNESS} }, FACING_DOWN },
    // Trampolines, both facing up (a half turn). Fall into one and you're
    // thrown up out of the other just as fast, with your sideways
    // direction mirrored. Drop in from a high ledge to reach the other one.
    { { {PX(215), PX(270)}, {PX(40), PORTAL_THICKNESS} }, FACING_UP },
    { { {PX(545), PX(270)}, {PX(40), PORTAL_THICKNESS} }, FACING_UP },
    // Floor-level wrap-around at the walls (no turn), so the floor is a
    // loop round the tower.
    { { {PX(0),   PX(390)}, {PORTAL_THICKNESS, PX(40)} }, FACING_RIGHT },
    { { {PX(788), PX(390)}, {PORTAL_THICKNESS, PX(40)} }, FACING_LEFT },
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

// Move v toward target by at most step, without overshooting it.
static int32_t approach(int32_t v, int32_t target, int32_t step) {
    if (v < target) return (v + step > target) ? target : v + step;
    return (v - step < target) ? target : v - step;
}

static Collision player_box(const Player *p) {
    Collision box = { p->position, {PLAYER_WIDTH, PLAYER_HEIGHT} };
    return box;
}

static Collision bullet_box(const Bullet *b) {
    Collision box = { b->position, {BULLET_SIZE, BULLET_SIZE} };
    return box;
}

// Outward direction of each facing as a unit vector (y grows downward).
static const IntVector2 FacingNormal[4] = {
    [FACING_UP]    = {  0, -1 },
    [FACING_RIGHT] = {  1,  0 },
    [FACING_DOWN]  = {  0,  1 },
    [FACING_LEFT]  = { -1,  0 },
};

// Rotate by k quarter turns clockwise (on screen, where y points down).
// Exact in integers: each quarter turn just swaps and negates.
static IntVector2 rotate_quarters(IntVector2 v, int k) {
    for (int i = 0; i < k; i++) {
        IntVector2 r = { -v.y, v.x };
        v = r;
    }
    return v;
}

static IntVector2 box_center(Collision b) {
    IntVector2 c = { b.origin.x + b.bounds.x / 2, b.origin.y + b.bounds.y / 2 };
    return c;
}

// If a box (top-left *pos, given size) overlaps a portal while moving into
// it, move it out of the linked portal and rotate its velocity to match.
// Used for players and bullets. Returns true if it went through.
static bool try_portal(IntVector2 *pos, IntVector2 *vel, IntVector2 size) {
    Collision box = { *pos, size };
    for (int i = 0; i < NUM_PORTALS; i++) {
        const Portal *in = &MapPortals[i];

        // Moving into the portal means against its facing: negative dot
        // product. Also what stops instant re-entry: we leave the exit moving
        // along its facing, so the exit can't take us straight back.
        IntVector2 n = FacingNormal[in->facing];
        if (vel->x * n.x + vel->y * n.y >= 0) continue;
        if (!colliding(box, in->box)) continue;

        const Portal *out = &MapPortals[i ^ 1];

        // Quarter turns that take "into in" (the opposite of in's facing,
        // which is in->facing + 2) to "out of out" (out->facing). +6 rather
        // than -2 keeps it positive before the %, since C's % of a negative
        // number is negative.
        int k = (int)(out->facing - in->facing + 6) % 4;

        // Where we were relative to the entry portal's centre, rotated the
        // same way, so walking through the bottom of one portal comes out of
        // the bottom of the other.
        IntVector2 c_in = box_center(in->box);
        IntVector2 c_out = box_center(out->box);
        IntVector2 offset = {
            pos->x + size.x / 2 - c_in.x,
            pos->y + size.y / 2 - c_in.y,
        };
        offset = rotate_quarters(offset, k);
        *vel = rotate_quarters(*vel, k);

        // Enforce the minimum speed out of the exit (see MIN_EXIT_SPEED).
        // Only the part of the velocity along the exit's facing changes.
        IntVector2 n_out = FacingNormal[out->facing];
        int32_t out_speed = vel->x * n_out.x + vel->y * n_out.y;
        if (out_speed < MIN_EXIT_SPEED) {
            vel->x += (MIN_EXIT_SPEED - out_speed) * n_out.x;
            vel->y += (MIN_EXIT_SPEED - out_speed) * n_out.y;
        }

        // Keep the offset along the exit portal, but put the box just in
        // front of it (touching, not overlapping) so it starts in open air.
        // The offset is clamped so the box stays within the exit's span:
        // an offset from a longer entry would otherwise poke out past the
        // end of a shorter exit, maybe into the terrain next to it.
        const Collision *ob = &out->box;
        int32_t min_x = ob->origin.x, max_x = ob->origin.x + ob->bounds.x - size.x;
        int32_t min_y = ob->origin.y, max_y = ob->origin.y + ob->bounds.y - size.y;
        switch (out->facing) {
        case FACING_UP:
            pos->x = clamp_i32(c_out.x + offset.x - size.x / 2, min_x, max_x);
            pos->y = ob->origin.y - size.y;
            break;
        case FACING_DOWN:
            pos->x = clamp_i32(c_out.x + offset.x - size.x / 2, min_x, max_x);
            pos->y = ob->origin.y + ob->bounds.y;
            break;
        case FACING_LEFT:
            pos->x = ob->origin.x - size.x;
            pos->y = clamp_i32(c_out.y + offset.y - size.y / 2, min_y, max_y);
            break;
        case FACING_RIGHT:
            pos->x = ob->origin.x + ob->bounds.x;
            pos->y = clamp_i32(c_out.y + offset.y - size.y / 2, min_y, max_y);
            break;
        }
        return true;  // at most one portal per tick
    }
    return false;
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

// Bullet velocity for the direction the player is aiming: the held direction
// keys (8 ways), or the way the player is facing if none are held. Straight
// down on the ground would only hit the floor, so it becomes down-forward.
static IntVector2 aim_velocity(const Player *p, Input in) {
    int dx = axis(in, INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT);
    int dy = axis(in, INPUT_MOVE_UP, INPUT_MOVE_DOWN);
    int facing = p->facing_right ? 1 : -1;
    if (dx == 0 && dy == 0) {
        dx = facing;
    }
    if (dx == 0 && dy > 0 && p->grounded) {
        dx = facing;
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

    // Jump timers tick every tick, in hitstun too, so a press made during
    // hitstun has expired long before it ends.
    if (p->grounded) {
        p->coyote_left = COYOTE_TICKS;
    } else if (p->coyote_left > 0) {
        p->coyote_left--;
    }
    if (p->jump_buffer_left > 0) p->jump_buffer_left--;

    if (p->hitstun_left == 0) {
        int dir = axis(in, INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT);
        if (dir != 0) p->facing_right = (dir > 0);

        // Planted on the ground: left/right still turn the player (above)
        // but don't move them, as if nothing were held. In the air plant
        // does nothing. Uses last tick's grounded, like the steering below.
        if ((in & INPUT_PLANT) && p->grounded) dir = 0;

        // Steer toward walking speed in the held direction (or toward 0).
        // Faster than walking counts as overspeed unless the player is
        // holding the opposite way (braking): letting go or holding forward
        // must not throw away momentum.
        int32_t vx = p->velocity.x;
        int moving = (vx > 0) - (vx < 0);              // -1, 0 or +1
        int32_t speed = vx < 0 ? -vx : vx;
        bool overspeed = speed > MOVE_SPEED && dir != -moving;
        bool pushing = dir != 0 && !overspeed;
        // grounded is from the end of last tick, before this tick's move.
        int32_t step;
        if (overspeed) {
            step = p->grounded ? GROUND_DRAG : AIR_DRAG;
        } else if (pushing) {
            step = p->grounded ? GROUND_ACCEL : AIR_ACCEL;
        } else {
            step = p->grounded ? GROUND_FRICTION : AIR_FRICTION;
        }
        p->velocity.x = approach(vx, dir * MOVE_SPEED, step);

        // A press (not a hold) starts the buffer; the jump fires on any tick
        // the buffer is live and we're on the ground or in coyote time.
        bool jump_pressed = (in & INPUT_JUMP) && !(prev_in & INPUT_JUMP);
        if (jump_pressed) p->jump_buffer_left = JUMP_BUFFER_TICKS;

        bool can_jump = p->grounded || p->coyote_left > 0;
        if (p->jump_buffer_left > 0 && can_jump) {
            p->velocity.y = JUMP_VELOCITY;
            p->jump_buffer_left = 0;
            p->coyote_left = 0;  // or you could jump again mid-air
            p->in_jump = true;
        }

        // Variable height: if jump isn't held while still rising from a jump,
        // halve the upward speed once. Checks "held", not "released", so a
        // buffered jump whose button was already let go becomes a short hop.
        if (p->in_jump) {
            if (p->velocity.y >= 0) {
                p->in_jump = false;  // past the peak, nothing left to cut
            } else if (!(in & INPUT_JUMP)) {
                p->velocity.y /= 2;
                p->in_jump = false;
            }
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
    IntVector2 player_size = { PLAYER_WIDTH, PLAYER_HEIGHT };
    if (try_portal(&p->position, &p->velocity, player_size)) {
        // move_y set grounded before the teleport, so it describes where
        // we were, not where we are now: we always come out in the air.
        // Coyote time is cleared too, or walking into a door (grounded)
        // would allow a mid-air jump out of the ceiling, straight back in.
        p->grounded = false;
        p->coyote_left = 0;
    }

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

        // Before the terrain and map-edge checks, so a bullet that reached a
        // portal is judged where it comes out.
        IntVector2 bullet_size = { BULLET_SIZE, BULLET_SIZE };
        try_portal(&b->position, &b->velocity, bullet_size);

        // Bullets move at most 8 px per tick and are 8 px big, so they can't
        // skip through a 10 px wide player or platform in one step.
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
            // The knockback replaces the jump, so releasing jump after
            // hitstun mustn't cut the knockback's upward speed.
            p->in_jump = false;
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
