#pragma once

// Public interface of the simulation: everything another file (main loop,
// renderer, netcode) needs to create, step, draw or send a GameState.
// Tuning constants and helpers that only the sim uses live in sim.c.

#include <stdint.h>
#include <stdbool.h>

// --------------------------------------------------------
// Units and sizes
// --------------------------------------------------------

// Seconds per tick (60 Hz). Wall-clock only: the main loop and netcode use
// it to decide *when* ticks run. The sim never reads it, because floats in
// the sim would break determinism.
#define STEP (1.0 / 60.0)

// All positions, sizes and velocities in the sim are in subpixels.
#define SUB_PIXEL_FACTOR 256

#define MAP_PIXEL_WIDTH 640
#define MAP_PIXEL_HEIGHT 450

#define MAP_WIDTH (MAP_PIXEL_WIDTH * SUB_PIXEL_FACTOR)
#define MAP_HEIGHT (MAP_PIXEL_HEIGHT * SUB_PIXEL_FACTOR)

#define PLAYER_WIDTH  (10 * SUB_PIXEL_FACTOR)
#define PLAYER_HEIGHT (20 * SUB_PIXEL_FACTOR)

#define PLAYER_1 0
#define PLAYER_2 1

#define MAX_PLAYERS 2
#define MAX_BULLETS 16

#define BULLET_SIZE (8 * SUB_PIXEL_FACTOR)

// Rules the renderer also needs (for the HUD).
#define MAX_HEALTH 2
#define MAGAZINE_SIZE 3
#define RELOAD_TICKS 90
#define SHOOT_STARTUP_TICKS 6  // from pressing fire to the bullet appearing
#define KILLS_TO_WIN 3
#define LOBBY_COUNTDOWN_TICKS 180  // 3 s from both ready to the match starting

// --------------------------------------------------------
// Basic types
// --------------------------------------------------------

typedef int32_t Tick;

typedef struct {
    int32_t x;
    int32_t y;
} IntVector2;

// Axis-aligned box: top-left corner and size.
typedef struct {
    IntVector2 origin;
    IntVector2 bounds;
} Collision;

// --------------------------------------------------------
// Input
// --------------------------------------------------------

typedef uint16_t Input;

enum InputFields {
    INPUT_MOVE_LEFT = 1 << 0,
    INPUT_MOVE_RIGHT= 1 << 1,
    INPUT_MOVE_UP = 1 << 2,
    INPUT_MOVE_DOWN = 1 << 3,

    INPUT_JUMP = 1 << 4,
    INPUT_SHOOT = 1 << 5,
    // Held on the ground: stand still, left/right only aim and turn.
    INPUT_PLANT = 1 << 6,
};

// --------------------------------------------------------
// Map (static, not part of GameState)
// --------------------------------------------------------

#define NUM_MAP_OBJ 26

// Defined in sim.c. Exposed so the renderer can draw it.
extern const Collision MapCollision[NUM_MAP_OBJ];

// Which way a portal's opening faces: things enter moving against this
// direction and leave moving along it. Clockwise order, so the difference
// between two facings is a number of quarter turns.
typedef enum {
    FACING_UP,
    FACING_RIGHT,
    FACING_DOWN,
    FACING_LEFT,
} Facing;

typedef struct {
    Collision box;
    Facing facing;
} Portal;

// Portals are linked in pairs that sit next to each other in the array:
// 0 with 1, 2 with 3, ... so portal i's partner is i ^ 1.
#define NUM_PORTALS 14

// Defined in sim.c, like MapCollision.
extern const Portal MapPortals[NUM_PORTALS];

// --------------------------------------------------------
// Game state
// --------------------------------------------------------

typedef struct {
    int8_t health;

    IntVector2 position;
    IntVector2 velocity;

    bool facing_right;

    // Standing on terrain at the end of the last tick. Set by collision.
    bool grounded;

    // Jump helpers. All counters are in ticks and count down to 0.
    // Coyote time: can still jump this long after walking off a ledge.
    int32_t coyote_left;
    // Jump buffer: a jump pressed this recently fires as soon as it's allowed
    // (e.g. pressed just before landing).
    int32_t jump_buffer_left;
    // Rising from our own jump. Letting go of jump while this is set cuts the
    // rise short (variable jump height). Cleared at the peak, on cut or hit.
    bool in_jump;

    int32_t hitstun_left;

    // Gun
    int16_t ammo;
    IntVector2 queued_bullet_velocity;

    int32_t reload_frames_left;
    bool tried_active_reload;

    int32_t shoot_startup_left;
    int32_t shoot_cooldown_left;
} Player;

typedef struct {
    bool active;
    uint8_t owner;
    // Terrain hits it can still bounce off. The next hit at 0 removes it.
    uint8_t bounces_left;

    IntVector2 position;
    IntVector2 velocity;

    int32_t lifetime_left;
} Bullet;

typedef struct {
    // Frames since the begining of the sim.
    Tick tick;

    uint8_t round_number; // Which round it is.
    uint8_t kills[MAX_PLAYERS]; // How many kills each player has

    // Ticks until the next round starts. 0 while a round is being played.
    // While it's above 0 the game is frozen and shows who scored.
    int32_t round_over_time_left;

    // Who won the round that just ended: PLAYER_1, PLAYER_2, or -1 for a tie.
    // Only meaningful while round_over_time_left > 0.
    int8_t round_winner;

    // Lobby: before the first match and after each one ends. Players are
    // frozen at their spawns and press fire to toggle ready. Once both are
    // ready a countdown runs, and the match starts when it reaches 0.
    // kills is kept until then, so the lobby can show the last result.
    bool in_lobby;
    bool ready[MAX_PLAYERS];
    // Ticks left in the countdown. Held at LOBBY_COUNTDOWN_TICKS while
    // either player isn't ready, so un-readying restarts it.
    int32_t lobby_countdown_left;

    // The two player structs
    Player players[MAX_PLAYERS];

    // Last frame's inputs.
    Input previous_input[MAX_PLAYERS];

    // Every bullet in the game
    Bullet bullets[MAX_BULLETS];
} GameState;

// --------------------------------------------------------
// Functions
// --------------------------------------------------------

// Fill in the starting state (tick 0): in the lobby, nobody ready.
void sim_init(GameState *state);

// PLAYER_1 or PLAYER_2 if that player has reached KILLS_TO_WIN, else -1.
int sim_match_winner(const GameState *state);

// Bullet velocity for a shot fired now with input `in`: the held direction
// keys (8 ways), or the way the player is facing if none are held. Straight
// down on the ground would only hit the floor, so it becomes down-forward.
// Public so the renderer can show the aim with the same rules.
IntVector2 sim_aim_velocity(const Player *p, Input in);

// Whether a fire press right now would start a shot.
bool sim_can_shoot(const Player *p);

// Advance the game by one tick: next = step(prev, input).
// prev and next must be different GameStates (they must not overlap).
void sim(const Input input[MAX_PLAYERS],
         const GameState *restrict prev,
         GameState *restrict next);
