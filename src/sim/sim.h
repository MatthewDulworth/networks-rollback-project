#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PLAYER_1 0
#define PLAYER_2 1

#define MAX_PLAYERS 2
#define MAX_BULLETS 16

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

typedef struct {
    int32_t x;
    int32_t y;
} IntVector2;

typedef struct {
    int8_t health;

    IntVector2 position;
    IntVector2 velocity;

    bool facing_right;
    
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
    int32_t frame_number;

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