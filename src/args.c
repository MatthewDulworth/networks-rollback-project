#include "args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim/sim.h"  // PLAYER_1, PLAYER_2

static bool parse_port(const char *text, uint16_t *out) {
    char *end;
    long value = strtol(text, &end, 10);
    // end == text: no digits at all. *end != '\0': junk after the number.
    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

// A non-negative whole number with nothing after it.
static bool parse_non_negative(const char *text, int *out) {
    char *end;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 100000) {
        return false;
    }
    *out = (int)value;
    return true;
}

bool parse_args(int argc, char **argv, Args *args) {
    // Either "lan" or four positional arguments, then any number of
    // "--flag value" pairs and value-less switches (--delay).
    args->lan = argc >= 2 && strcmp(argv[1], "lan") == 0;
    int first_flag = args->lan ? 2 : 5;
    if (argc < first_flag) {
        return false;
    }
    memset(&args->netsim, 0, sizeof args->netsim);
    args->delay_mode = false;
    args->skip_lobby = false;
    // One argument per step; a flag with a value consumes its value too.
    for (int i = first_flag; i < argc; i++) {
        if (strcmp(argv[i], "--delay") == 0) {
            args->delay_mode = true;
            continue;
        }
        if (strcmp(argv[i], "--no-lobby") == 0) {
            args->skip_lobby = true;
            continue;
        }
        if (i + 1 >= argc) {
            return false;  // flag with no value after it
        }
        int *field;
        if (strcmp(argv[i], "--lag") == 0) {
            field = &args->netsim.lag_ms;
        } else if (strcmp(argv[i], "--jitter") == 0) {
            field = &args->netsim.jitter_ms;
        } else if (strcmp(argv[i], "--loss") == 0) {
            field = &args->netsim.loss_pct;
        } else {
            return false;
        }
        if (!parse_non_negative(argv[i + 1], field)) {
            return false;
        }
        i++;  // skip the value
    }

    if (args->lan) {
        // Peer address and player come from discovery, in main.
        args->local_port = LAN_PORT;
        args->peer_ip = NULL;
        args->peer_port = LAN_PORT;
        args->player = PLAYER_1;
        return true;
    }

    args->peer_ip = argv[2];
    if (!parse_port(argv[1], &args->local_port)
        || !parse_port(argv[3], &args->peer_port)) {
        return false;
    }
    if (strcmp(argv[4], "0") == 0) {
        args->player = PLAYER_1;
    } else if (strcmp(argv[4], "1") == 0) {
        args->player = PLAYER_2;
    } else {
        return false;
    }
    return true;
}

void print_usage(const char *program) {
    fprintf(stderr,
            "usage: %s <local_port> <peer_ip> <peer_port> <player 0|1>"
            " [--lag ms] [--jitter ms] [--loss pct] [--delay] [--no-lobby]\n"
            "   or: %s lan [--lag ms] [--jitter ms] [--loss pct] [--delay] [--no-lobby]\n"
            "  e.g. %s 5000 127.0.0.1 5001 0   (and on the peer: 5001 127.0.0.1 5000 1)\n"
            "  lan finds the other machine by multicast on port %d and picks\n"
            "      the players itself (two machines only, both run \"lan\")\n"
            "  --lag/--jitter/--loss simulate a bad connection (both directions)\n"
            "  --delay uses delay-based netcode instead of rollback (no prediction)\n"
            "  --no-lobby skips the ready-up lobby (pass it to both peers)\n",
            program, program, program, LAN_PORT);
}
