#pragma once

// Public interface of the simulation: everything another file (main loop,
// renderer, netcode) needs to create, step, draw or send a GameState.
// Tuning constants and helpers that only the sim uses live in sim.c.

#include <stdint.h>
#include <stdbool.h>

// --------------------------------------------------------
// Units and sizes
// --------------------------------------------------------

// All positions, sizes and velocities in the sim are in subpixels.
#define SUB_PIXEL_FACTOR 256

#define MAP_PIXEL_WIDTH 800
#define MAP_PIXEL_HEIGHT 450

#define MAP_WIDTH (MAP_PIXEL_WIDTH * SUB_PIXEL_FACTOR)
#define MAP_HEIGHT (MAP_PIXEL_HEIGHT * SUB_PIXEL_FACTOR)

#define PLAYER_WIDTH  (10 * SUB_PIXEL_FACTOR)
#define PLAYER_HEIGHT (10 * SUB_PIXEL_FACTOR)

#define PLAYER_1 0
#define PLAYER_2 1

#define MAX_PLAYERS 2
#define MAX_BULLETS 16

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

    INPUT_AIM_LEFT = 1 << 4,
    INPUT_AIM_RIGHT = 1 << 5,
    INPUT_AIM_UP = 1 << 6,
    INPUT_AIM_DOWN = 1 << 7,

    INPUT_JUMP = 1 << 8,
    INPUT_SHOOT = 1 << 9,
};

// --------------------------------------------------------
// Map (static, not part of GameState)
// --------------------------------------------------------

#define NUM_MAP_OBJ 1

// Defined in sim.c. Exposed so the renderer can draw it.
extern const Collision MapCollision[NUM_MAP_OBJ];

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

    IntVector2 position;
    IntVector2 velocity;

    int32_t lifetime_left;
} Bullet;

typedef struct {
    // Frames since the begining of the sim.
    Tick tick;

    uint8_t round_number; // Which round it is.
    uint8_t kills[MAX_PLAYERS]; // How many kills each player has

    // How much time until the next round starts
    int32_t round_over_time_left;

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

// Fill in the starting state for a match (tick 0).
void sim_init(GameState *state);

// Advance the game by one tick: next = step(prev, input).
// prev and next must be different GameStates (they must not overlap).
void sim(const Input input[MAX_PLAYERS],
         const GameState *restrict prev,
         GameState *restrict next);
