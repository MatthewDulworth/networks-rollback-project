#pragma once

// Command line: <local_port> <peer_ip> <peer_port> <player 0|1>, then any
// number of optional "--flag value" pairs and the --delay switch.

#include <stdbool.h>
#include <stdint.h>
#include "net/netsim.h"  // NetsimSettings

typedef struct {
    uint16_t local_port;
    const char *peer_ip;  // points into argv
    uint16_t peer_port;
    int player;  // PLAYER_1 or PLAYER_2; must be opposite on the two peers
    NetsimSettings netsim;  // Optional --lag/--jitter/--loss, default all 0
    bool delay_mode;        // --delay: delay-based instead of rollback
} Args;

// Fill args from argv. False if anything is missing or malformed.
bool parse_args(int argc, char **argv, Args *args);

// Print how to run the game to stderr. program is argv[0].
void print_usage(const char *program);
