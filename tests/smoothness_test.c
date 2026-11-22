// Rollback vs delay-based netcode under the same network conditions.
//
// For each condition, runs one match in rollback mode and one in delay mode
// (two real peers each, over localhost UDP, player 1's netsim degrading the
// link), measures what a player would notice, prints a comparison table and
// checks that:
// - on a clean link the two modes are equally smooth (rollback costs
//   nothing when there's nothing to hide),
// - once the round trip is longer than the input delay covers, rollback is
//   clearly smoother: the game keeps closer to full speed, freezes less,
//   and input shows up on screen sooner.
//
// What's measured, over a fixed window of real time after a warm-up:
// - speed: game ticks run per real second, as a fraction of 60. Below 100%
//   the game is running in slow motion.
// - frozen: frames where no tick ran, so the picture didn't move.
// - longest freeze: the longest real time between two frames that ran a
//   tick. 17 ms is a perfectly smooth 60 fps.
// - input latency: from sampling the keyboard to the first frame that shows
//   the tick using that sample. Rollback is a steady INPUT_DELAY ticks
//   (50 ms). This understates delay mode a little: a key pressed during a
//   freeze isn't sampled until the freeze ends, and that wait isn't counted.
// - rollbacks/s, for context (the cost rollback pays for its smoothness:
//   corrections that can make things jump).
// Each table cell is the worse of the two players.
//
// Takes about 10 s: every match runs in parallel, in real time. Needs to
// bind UDP ports 47200-47221 on localhost.
//
// Run with `ctest --test-dir build` (or ./build/smoothness_test).

#include "match.h"
#include "net/net.h"
#include "net/netsim.h"
#include "rollback/rollback.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WARMUP_SECONDS 1.0    // skip the start (connection, first sync)
#define WINDOW_SECONDS 6.0    // measure over this much real time
#define LINGER_SECONDS 1.5    // keep the peer fed while it finishes its window
#define TIMEOUT_SECONDS 30.0
#define BASE_PORT 47200

#define TICKS_PER_SECOND (1.0 / STEP)

typedef enum {
    // Both modes should be equally smooth.
    EXPECT_SAME,
    // Rollback should be clearly smoother than delay mode.
    EXPECT_ROLLBACK_SMOOTHER,
} Expectation;

typedef struct {
    const char *name;
    NetsimSettings netsim;  // applied on player 1's machine only
    Expectation expect;
} Condition;

// lag is the round trip (half each way). INPUT_DELAY is 3 ticks, 50 ms.
static const Condition conditions[] = {
    { "clean",                           { 0, 0, 0 },     EXPECT_SAME },
    { "50 ms RTT",                       { 50, 0, 0 },    EXPECT_SAME },
    { "100 ms RTT",                      { 100, 0, 0 },   EXPECT_ROLLBACK_SMOOTHER },
    { "200 ms RTT",                      { 200, 0, 0 },   EXPECT_ROLLBACK_SMOOTHER },
    { "200 ms RTT, 30 ms jitter, 10% loss", { 200, 30, 10 }, EXPECT_ROLLBACK_SMOOTHER },
    { "500 ms RTT",                      { 500, 0, 0 },   EXPECT_ROLLBACK_SMOOTHER },
};
#define NUM_CONDITIONS ((int)(sizeof conditions / sizeof conditions[0]))
#define NUM_MODES 2  // 0 = rollback, 1 = delay

typedef struct {
    bool ok;                   // false: timed out before finishing the window
    double speed;              // fraction of real time, 1.0 = full speed
    double frozen;             // fraction of frames that ran no tick
    double longest_freeze_ms;
    double avg_latency_ms;
    double max_latency_ms;
    double rollbacks_per_second;
} PeerResult;

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

// xorshift32: scripted input only.
static uint32_t rng_state;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// --------------------------------------------------------
// One peer (runs in a child process)
// --------------------------------------------------------

// When each tick's local input was sampled, to measure input latency.
// Indexed by tick; big enough for any number of ticks one frame can run
// plus INPUT_DELAY. The tick is stored to catch stale slots.
#define SAMPLE_RING_SIZE 256
typedef struct {
    Tick tick;
    double time;
} Sample;
static Sample samples[SAMPLE_RING_SIZE];

static PeerResult run_peer(int condition, int mode, int player) {
    const Condition *c = &conditions[condition];
    int run = condition * NUM_MODES + mode;
    uint16_t local_port = (uint16_t)(BASE_PORT + 2 * run + player);
    uint16_t peer_port = (uint16_t)(BASE_PORT + 2 * run + (1 - player));
    PeerResult result = { 0 };

    if (!net_init(local_port, "127.0.0.1", peer_port)) {
        fprintf(stderr, "couldn't open UDP port %u\n", local_port);
        return result;
    }
    netsim_init();
    NetsimSettings off = { 0, 0, 0 };
    netsim_set(player == PLAYER_1 ? c->netsim : off);
    match_init(player, mode == 1, clock_now);

    for (int i = 0; i < SAMPLE_RING_SIZE; i++) {
        samples[i].tick = NO_TICK;
    }
    // Same input script for both modes, so they play the same game.
    rng_state = 0x9E3779B9u ^ (uint32_t)(condition * 7919 + player * 104729 + 1);
    Input input = 0;
    int frame_count = 0;

    double begin = clock_now();
    double started_at = -1;

    // Window measurements.
    bool window_open = false;
    Tick window_first_tick = 0;
    Tick window_last_tick = 0;
    double window_start = 0;
    double window_end = 0;
    int window_frames = 0;
    int frozen_frames = 0;
    double last_advance = 0;
    double longest_freeze = 0;
    double latency_sum = 0;
    int latency_count = 0;
    double latency_max = 0;
    MatchStats stats_at_start = { 0 };
    MatchStats stats_at_end = { 0 };

    for (;;) {
        double t = clock_now();
        if (t - begin > TIMEOUT_SECONDS) {
            net_shutdown();
            return result;  // ok stays false
        }
        if (started_at < 0 && match_started()) {
            started_at = t;
        }
        double since_start = started_at < 0 ? -1 : t - started_at;
        if (since_start > WARMUP_SECONDS + WINDOW_SECONDS + LINGER_SECONDS) {
            break;
        }
        bool in_window = since_start >= WARMUP_SECONDS
                         && since_start < WARMUP_SECONDS + WINDOW_SECONDS;

        if (frame_count % 8 == 0) {
            input = (Input)(rng_next() & 0x3FF);
        }
        frame_count++;

        // Every tick run this frame records this frame's sample for tick
        // + INPUT_DELAY (see match.c), so note when that sample was taken.
        Tick before = match_next_tick();
        match_frame(input);
        Tick after = match_next_tick();
        double shown = clock_now();  // the game draws right after this
        for (Tick k = before; k < after; k++) {
            Tick input_tick = k + INPUT_DELAY;
            samples[input_tick % SAMPLE_RING_SIZE] = (Sample){ input_tick, t };
        }

        if (in_window) {
            if (!window_open) {
                window_open = true;
                window_first_tick = before;
                window_start = t;
                last_advance = t;
                stats_at_start = match_stats_total();
            }
            window_frames++;
            if (after == before) {
                frozen_frames++;
            } else {
                double gap = shown - last_advance;
                if (gap > longest_freeze) {
                    longest_freeze = gap;
                }
                last_advance = shown;

                // Ticks before..after-1 just ran, so their local inputs
                // are on screen now for the first time.
                for (Tick k = before; k < after; k++) {
                    const Sample *s = &samples[k % SAMPLE_RING_SIZE];
                    if (s->tick == k) {
                        double latency = shown - s->time;
                        latency_sum += latency;
                        latency_count++;
                        if (latency > latency_max) {
                            latency_max = latency;
                        }
                    }
                }
            }
            window_last_tick = after;
            window_end = shown;
            stats_at_end = match_stats_total();
        }

        sleep_seconds(match_time_until_next_frame());
    }

    net_shutdown();

    double seconds = window_end - window_start;
    if (!window_open || seconds <= 0 || latency_count == 0) {
        return result;
    }
    result.ok = true;
    result.speed = (window_last_tick - window_first_tick) / (seconds * TICKS_PER_SECOND);
    result.frozen = (double)frozen_frames / window_frames;
    result.longest_freeze_ms = longest_freeze * 1000;
    result.avg_latency_ms = latency_sum / latency_count * 1000;
    result.max_latency_ms = latency_max * 1000;
    result.rollbacks_per_second =
        (stats_at_end.rollbacks - stats_at_start.rollbacks) / seconds;
    return result;
}

// --------------------------------------------------------
// Parent: run every match, compare the modes
// --------------------------------------------------------

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

// One number per column for a match: the worse of its two players.
static PeerResult worse_of(PeerResult a, PeerResult b) {
    PeerResult r;
    r.ok = a.ok && b.ok;
    r.speed = MIN(a.speed, b.speed);
    r.frozen = MAX(a.frozen, b.frozen);
    r.longest_freeze_ms = MAX(a.longest_freeze_ms, b.longest_freeze_ms);
    r.avg_latency_ms = MAX(a.avg_latency_ms, b.avg_latency_ms);
    r.max_latency_ms = MAX(a.max_latency_ms, b.max_latency_ms);
    r.rollbacks_per_second = MAX(a.rollbacks_per_second, b.rollbacks_per_second);
    return r;
}

static void print_row(const char *condition, const char *mode, PeerResult r) {
    if (!r.ok) {
        printf("%-36s %-9s  (failed to finish)\n", condition, mode);
        return;
    }
    printf("%-36s %-9s %5.0f%% %6.0f%% %8.0f ms %7.0f / %4.0f ms %7.1f\n",
           condition, mode, r.speed * 100, r.frozen * 100, r.longest_freeze_ms,
           r.avg_latency_ms, r.max_latency_ms, r.rollbacks_per_second);
}

// Returns the number of failed checks for one condition.
static int check(const Condition *c, PeerResult rb, PeerResult dl) {
    int failures = 0;
    if (!rb.ok || !dl.ok) {
        fprintf(stderr, "[%s] FAIL: a match didn't finish (timed out, or "
                "couldn't open a port)\n", c->name);
        return 1;
    }

    switch (c->expect) {
    case EXPECT_SAME:
        // Nothing to hide: both run at full speed without freezing, and
        // input takes the same INPUT_DELAY to show up.
        if (rb.speed < 0.97 || dl.speed < 0.97) {
            fprintf(stderr, "[%s] FAIL: expected both modes at full speed, "
                    "got rollback %.0f%%, delay %.0f%%\n",
                    c->name, rb.speed * 100, dl.speed * 100);
            failures++;
        }
        if (dl.avg_latency_ms > rb.avg_latency_ms + 10) {
            fprintf(stderr, "[%s] FAIL: expected the same input latency, got "
                    "rollback %.0f ms, delay %.0f ms\n",
                    c->name, rb.avg_latency_ms, dl.avg_latency_ms);
            failures++;
        }
        break;

    case EXPECT_ROLLBACK_SMOOTHER:
        // Margins are wide on purpose: this runs in real time on a shared
        // machine, so the numbers wobble from run to run.
        if (rb.speed < dl.speed + 0.10) {
            fprintf(stderr, "[%s] FAIL: expected rollback to run clearly "
                    "faster, got rollback %.0f%%, delay %.0f%%\n",
                    c->name, rb.speed * 100, dl.speed * 100);
            failures++;
        }
        // Not "half as many": at 500 ms rollback is past its own limit
        // too and freezes ~40% of frames (to delay mode's ~80%).
        if (rb.frozen > dl.frozen - 0.10) {
            fprintf(stderr, "[%s] FAIL: expected rollback to freeze on "
                    "clearly fewer frames, got rollback %.0f%%, "
                    "delay %.0f%%\n",
                    c->name, rb.frozen * 100, dl.frozen * 100);
            failures++;
        }
        if (rb.avg_latency_ms >= dl.avg_latency_ms) {
            fprintf(stderr, "[%s] FAIL: expected input to show up sooner "
                    "with rollback, got rollback %.0f ms, delay %.0f ms\n",
                    c->name, rb.avg_latency_ms, dl.avg_latency_ms);
            failures++;
        }
        break;
    }
    return failures;
}

int main(void) {
    pid_t pids[NUM_CONDITIONS][NUM_MODES][MAX_PLAYERS];
    int pipes[NUM_CONDITIONS][NUM_MODES][MAX_PLAYERS];

    // One child per peer. Each sends its PeerResult back through a pipe.
    for (int c = 0; c < NUM_CONDITIONS; c++) {
        for (int m = 0; m < NUM_MODES; m++) {
            for (int p = 0; p < MAX_PLAYERS; p++) {
                int fds[2];
                if (pipe(fds) < 0) {
                    perror("pipe");
                    return 1;
                }
                // Flush first, or each child inherits (and later prints) a
                // copy of anything still in the parent's stdout buffer.
                fflush(stdout);
                fflush(stderr);
                pid_t pid = fork();
                if (pid < 0) {
                    perror("fork");
                    return 1;
                }
                if (pid == 0) {
                    close(fds[0]);
                    PeerResult r = run_peer(c, m, p);
                    // A pipe write this small (well under PIPE_BUF) is
                    // all-or-nothing, so no partial-write loop is needed.
                    ssize_t written = write(fds[1], &r, sizeof r);
                    close(fds[1]);
                    // _exit: skip the parent's atexit handlers (see
                    // netcode_test.c).
                    _exit(written == (ssize_t)sizeof r ? 0 : 1);
                }
                close(fds[1]);
                pids[c][m][p] = pid;
                pipes[c][m][p] = fds[0];
            }
        }
    }

    // Collect. A child that crashed (failed assert) writes nothing, so its
    // read comes back short and its result stays "not ok".
    PeerResult results[NUM_CONDITIONS][NUM_MODES][MAX_PLAYERS];
    for (int c = 0; c < NUM_CONDITIONS; c++) {
        for (int m = 0; m < NUM_MODES; m++) {
            for (int p = 0; p < MAX_PLAYERS; p++) {
                PeerResult r = { 0 };
                if (read(pipes[c][m][p], &r, sizeof r) != (ssize_t)sizeof r) {
                    r.ok = false;
                }
                close(pipes[c][m][p]);
                int status;
                waitpid(pids[c][m][p], &status, 0);
                if (WIFSIGNALED(status)) {
                    fprintf(stderr, "[%s, %s, player %d] killed by signal %d "
                            "(a failed assert?)\n", conditions[c].name,
                            m == 0 ? "rollback" : "delay", p + 1,
                            WTERMSIG(status));
                    r.ok = false;
                }
                results[c][m][p] = r;
            }
        }
    }

    printf("%-36s %-9s %6s %7s %11s %17s %7s\n", "condition", "mode", "speed",
           "frozen", "longest", "latency avg/max", "rb/s");
    int failures = 0;
    for (int c = 0; c < NUM_CONDITIONS; c++) {
        PeerResult rb = worse_of(results[c][0][0], results[c][0][1]);
        PeerResult dl = worse_of(results[c][1][0], results[c][1][1]);
        print_row(conditions[c].name, "rollback", rb);
        print_row("", "delay", dl);
        failures += check(&conditions[c], rb, dl);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("rollback was smoother wherever expected\n");
    return 0;
}
