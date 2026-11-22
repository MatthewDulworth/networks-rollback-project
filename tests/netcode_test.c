// End-to-end netcode test: real peers over localhost UDP, no window.
//
// Each scenario forks two processes, one per player. Each runs the same
// per-frame loop as the game (match.c) with scripted pseudo-random input, so
// players move, jump and shoot and the remote side mispredicts. Player 1's
// network simulator degrades the link in both directions.
//
// A scenario passes when both peers:
// - reach TARGET_TICKS,
// - never see different state hashes from the two machines (sync_check),
// - kept comparing hashes until near the end (so the check really ran),
// - rolled back at least once on a lossy link (so rollback was exercised),
// - in delay mode: never rolled back (nothing is predicted) and stalled at
//   least once (so it really waited for late input),
// - ran at about 60 frames a second, not faster (the old bug where a
//   stalled loop stopped sleeping and spun),
// - and didn't trip any assert, including the local desync check
//   (an assert kills the process, which the parent reports).
//
// Takes about 10 s: scenarios run in parallel, in real time. Needs to bind
// UDP ports 47100-47107 on localhost.
//
// Run with `ctest --test-dir build` (or ./build/netcode_test).

#include "match.h"
#include "net/net.h"
#include "net/netsim.h"
#include "rollback/rollback.h"
#include "rollback/sync_check.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TARGET_TICKS 480        // 8 s of game time
#define LINGER_SECONDS 1.0      // keep exchanging packets after the target
#define TIMEOUT_SECONDS 30.0    // give up if a peer gets stuck
#define BASE_PORT 47100

typedef struct {
    const char *name;
    NetsimSettings netsim;  // applied on player 1's machine only
    bool expect_rollbacks;
    bool delay_mode;        // both peers use delay-based netcode
} Scenario;

// The delay-mode link is milder than the rollback ones: delay mode runs at
// most 2 * INPUT_DELAY ticks per round trip, so a 200 ms round trip would
// play at half speed and take ~16 s to reach TARGET_TICKS.
static const Scenario scenarios[] = {
    { "clean",                          { 0, 0, 0 },     false, false },
    { "100 ms, 30 ms jitter, 10% loss", { 100, 30, 10 }, true,  false },
    { "250 ms, 40 ms jitter, 20% loss", { 250, 40, 20 }, true,  false },
    { "delay mode, 60 ms, 20 ms jitter, 10% loss", { 60, 20, 10 }, false, true },
};
#define NUM_SCENARIOS ((int)(sizeof scenarios / sizeof scenarios[0]))

// --------------------------------------------------------
// Clock and sleep (what raylib's GetTime/WaitTime do in the game)
// --------------------------------------------------------

static double clock_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_seconds(double s) {
    if (s <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

// --------------------------------------------------------
// One peer (runs in a child process)
// --------------------------------------------------------

// xorshift32: scripted input only. Separate from the netsim's generator.
static uint32_t rng_state;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// Returns the process exit code: 0 = pass, 1 = fail.
static int run_peer(int index, int player) {
    const Scenario *sc = &scenarios[index];
    uint16_t local_port = (uint16_t)(BASE_PORT + 2 * index + player);
    uint16_t peer_port = (uint16_t)(BASE_PORT + 2 * index + (1 - player));
    char tag[96];
    snprintf(tag, sizeof tag, "[%s, player %d]", sc->name, player + 1);

    if (!net_init(local_port, "127.0.0.1", peer_port)) {
        fprintf(stderr, "%s FAIL: couldn't open UDP port %u\n", tag, local_port);
        return 1;
    }
    netsim_init();
    NetsimSettings off = { 0, 0, 0 };
    netsim_set(player == PLAYER_1 ? sc->netsim : off);
    match_init(player, sc->delay_mode, clock_now);

    rng_state = 0x9E3779B9u ^ (uint32_t)(index * 7919 + player * 104729 + 1);
    Input input = 0;
    int frames = 0;
    double begin = clock_now();
    double reached_target_at = -1;

    for (;;) {
        double t = clock_now();
        if (t - begin > TIMEOUT_SECONDS) {
            break;
        }
        if (reached_target_at < 0 && match_next_tick() >= TARGET_TICKS) {
            reached_target_at = t;
        }
        // Keep going a little after the target, so the last inputs, acks
        // and hashes reach the peer (it may be a few ticks behind).
        if (reached_target_at >= 0 && t - reached_target_at > LINGER_SECONDS) {
            break;
        }

        // New buttons every 8 frames: any of the 10 input bits.
        if (frames % 8 == 0) {
            input = (Input)(rng_next() & 0x3FF);
        }
        frames++;

        match_frame(input);
        sleep_seconds(match_time_until_next_frame());
    }

    double seconds = clock_now() - begin;
    MatchStats stats = match_stats_total();
    Tick ticks = match_next_tick();
    Tick last_match = sync_check_last_match();
    Tick desync = sync_check_first_desync();
    net_shutdown();

    double frames_per_second = frames / seconds;
    printf("%s ticks %d, rollbacks %d (max depth %d), stalled frames %d, "
           "%.0f frames/s, last hash match at tick %d\n",
           tag, ticks, stats.rollbacks, stats.max_depth, stats.stalled_frames,
           frames_per_second, last_match);

    int failures = 0;
    if (desync != NO_TICK) {
        fprintf(stderr, "%s FAIL: DESYNC, the machines' hashes differ at tick %d\n",
                tag, desync);
        failures++;
    }
    if (ticks < TARGET_TICKS) {
        fprintf(stderr, "%s FAIL: only reached tick %d of %d (timed out)\n",
                tag, ticks, TARGET_TICKS);
        failures++;
    }
    // A second of slack: the last hashes in flight when a peer stops are
    // never compared, which is fine.
    if (last_match < TARGET_TICKS - 60) {
        fprintf(stderr, "%s FAIL: hashes were last compared at tick %d, "
                "should be near %d\n", tag, last_match, TARGET_TICKS);
        failures++;
    }
    if (sc->delay_mode && stats.rollbacks > 0) {
        fprintf(stderr, "%s FAIL: %d rollbacks in delay mode, which should "
                "never predict\n", tag, stats.rollbacks);
        failures++;
    }
    if (sc->delay_mode && stats.stalled_frames == 0) {
        fprintf(stderr, "%s FAIL: delay mode never stalled on a laggy link, "
                "so it didn't wait for input\n", tag);
        failures++;
    }
    if (sc->expect_rollbacks && stats.rollbacks == 0) {
        fprintf(stderr, "%s FAIL: no rollbacks on a lossy link, so rollback "
                "wasn't exercised\n", tag);
        failures++;
    }
    // Every frame sleeps until the next tick is due (or one STEP while
    // stalled), so frames run at about 60/s. Counting frames, not stalls:
    // a spinning loop inflates both together. 90 leaves room for catch-up.
    if (frames_per_second > 90) {
        fprintf(stderr, "%s FAIL: %.0f frames/s, the loop isn't sleeping "
                "(spinning while stalled?)\n", tag, frames_per_second);
        failures++;
    }
    return failures == 0 ? 0 : 1;
}

// --------------------------------------------------------
// Parent: start every peer, wait for all of them
// --------------------------------------------------------

int main(void) {
    pid_t pids[NUM_SCENARIOS][MAX_PLAYERS];

    for (int i = 0; i < NUM_SCENARIOS; i++) {
        for (int p = 0; p < MAX_PLAYERS; p++) {
            // Flush first, or each child inherits (and later prints) a copy
            // of anything still sitting in the parent's stdout buffer.
            fflush(stdout);
            fflush(stderr);
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return 1;
            }
            if (pid == 0) {
                // _exit, not exit/return: skip the parent's atexit handlers
                // (and sanitizer reports about the parent's memory). _exit
                // also skips flushing stdio, so flush the result line first.
                int code = run_peer(i, p);
                fflush(stdout);
                fflush(stderr);
                _exit(code);
            }
            pids[i][p] = pid;
        }
    }

    int failed = 0;
    for (int i = 0; i < NUM_SCENARIOS; i++) {
        for (int p = 0; p < MAX_PLAYERS; p++) {
            int status;
            if (waitpid(pids[i][p], &status, 0) < 0) {
                perror("waitpid");
                failed++;
                continue;
            }
            if (WIFSIGNALED(status)) {
                // abort() from a failed assert lands here (SIGABRT = 6).
                fprintf(stderr, "[%s, player %d] FAIL: killed by signal %d "
                        "(a failed assert?)\n",
                        scenarios[i].name, p + 1, WTERMSIG(status));
                failed++;
            } else if (WEXITSTATUS(status) != 0) {
                failed++;
            }
        }
    }

    if (failed > 0) {
        fprintf(stderr, "%d peer(s) failed\n", failed);
        return 1;
    }
    printf("all netcode scenarios passed\n");
    return 0;
}
