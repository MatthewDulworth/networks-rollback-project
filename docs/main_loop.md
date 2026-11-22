# Main loop

What happens each frame, in order, and why the order matters. This is where
the sim, the rollback module and the network meet.

It's split in two:
- `src/match.c`: the netcode part of a frame (receive, rollback, simulate,
  frame advantage, send) and the tick timing. It doesn't use raylib, so
  `tests/netcode_test.c` runs it without a window.
- `src/main.c`: setup, the window, the keyboard, drawing and waiting.

Each frame, `main.c` calls `match_frame(input)`, draws `match_state()`, then
sleeps for `match_time_until_next_frame()`.

## Startup

1. `parse_args` (`src/args.c`): `<local_port> <peer_ip> <peer_port> <player 0|1>`, then
   optional `--lag`, `--jitter`, `--loss` (see `networking.md`).
2. `net_init` **before** the window opens, so a bad address or busy port
   exits immediately with an error.
3. `netsim_init` and apply the flags. Look up the local IP for the overlay.
4. Open the window. Player 1's window goes on the left edge of the screen,
   player 2's on the right, so two local copies don't start on top of each
   other.
5. `match_init(player, GetTime)`. It calls `sim_init`, `rollback_init`,
   `desync_check_init`, `sync_check_init` and `session_init`. The clock is
   passed in so the test can use its own.

## Starting the match

The stand-in for a real start handshake: don't tick until the first packet
from the peer arrives. `start_time` is set to that frame's time, and every tick
time is measured from it. Both sides send every frame, so each starts within
about one one-way trip of the other. A peer that starts a little late just
runs a little behind, and rollback covers the difference.

## One frame

| Step | What | Why here |
|---|---|---|
| 1. Input | `read_local_input()` (`IsKeyDown` only), then `handle_netsim_hotkeys()`. | Sampled once per frame. |
| 2. Receive | `netsim_update`, then `session_receive`. Start the match once `session_peer_seen()`, and roll the stats window. | **Before rollback**, so the newest remote inputs fix the past before new ticks are built on top of it. |
| 3. Rollback | If `find_rollback_point()` gives tick M: `load_state(M)`, then `process_tick` for M up to `next_tick - 1`, then `clear_rollback_point()`. Counted for the overlay. | Asserts depth ≤ `MAX_ROLLBACK`. `can_advance` guarantees that, so a deeper rollback means a bug. |
| 4. Simulate | Run every tick that's due (see below). Then measure frame advantage and, if we're over a tick ahead, push `start_time` forward a little. | Advantage after simulating, so `next_tick` matches the peer's `sender_tick` (see `networking.md`). |
| 5. Send | `session_send`, then `netsim_update` again. | **Every frame**, even while stalled or waiting for the peer (see `networking.md`). |
| 6. Render | `render_game`, then fill an `OverlayInfo` and `render_overlay`. | Reads the state only. |
| 7. Wait | Sleep until the next tick is due (one `STEP` while stalled or before the match), then poll input again. | |

## `process_tick(tick, state, next_state)`

Used by both normal ticks and rollback re-simulation, so they can't drift
apart:

1. `save_state(tick, state)`: snapshot the state **before** this tick. That's
   what a later rollback to this tick loads.
2. `get_inputs(tick, ...)`: both inputs, predicting the remote one if needed.
3. `sim(...)` into `next_state`, then `memcpy` it back into `state`. `memcpy`,
   not `*state = *next_state`, because struct assignment may skip padding
   bytes, and they must stay zero for byte-for-byte comparison.

## Fixed timestep

Tick N is due at `start_time + N * STEP` (`STEP` = 1/60 s). Each frame runs
every tick that's due:

```c
while (total_time >= next_tick * STEP - TICK_THRESHOLD
       && elapsed < STEP - CATCHUP_MARGIN) {
    if (!can_advance(next_tick)) { stall; break; }
    write_local_input(next_tick + INPUT_DELAY, local_input);
    process_tick(next_tick, ...);
    next_tick++;
    desync_check(next_tick);
    sync_check_after_tick(next_tick);
}
```

- **`TICK_THRESHOLD`** (1/30 of a tick) runs a tick slightly early rather than
  sleeping for a fraction of a millisecond.
- **`CATCHUP_MARGIN`**: after a stall (breakpoint, window drag), several ticks
  are due at once. The loop stops when the frame has used most of its time
  budget, so rendering keeps going while it catches up over several frames.
  (`follow_ups.md` has a better plan: cap ticks per frame instead.)
- **Stall:** `can_advance` is false when running this tick would mean guessing
  more than `MAX_ROLLBACK` remote inputs. `break`, not `continue`: the time
  condition is still true, so `continue` would spin forever instead of going
  on to receive and send.
- **Input delay:** this frame's sample is written for tick
  `next_tick + INPUT_DELAY`. Every tick run in the same frame gets the same
  sample. Press detection still works, because `previous_input` lives in the
  game state and updates every tick, so only the first of those ticks sees the
  press.

## Waiting and polling twice

After drawing, `WaitTime` sleeps until the next tick is due. That's 0 when
we're behind schedule, so the next frame starts right away and keeps catching
up.

**Except while stalled** (or before the match starts): then we're waiting for
a packet, not for the clock. `next_tick` has stopped but time hasn't, so "until
the next tick is due" is already in the past and the wait would be 0. The loop
would spin as fast as the CPU allows, sending a packet every pass: thousands a
second, enough to fill the netsim queues so they drop packets on their own,
and `stalls/s` would count passes instead of frames. A `stalled` flag, set at
the `can_advance` break, makes those frames wait one `STEP` instead.

`EndDrawing` already polled input, but that was before the sleep, so `PollInputEvents` is
called again to make the next frame's input fresh.

Polling twice per frame breaks raylib's `IsKeyPressed`/`IsKeyReleased` (the
second poll overwrites the "previous" key state). So the game only uses
`IsKeyDown`. The sim gets "just pressed" from `previous_input`, and the netsim
hotkeys use their own `key_went_down`.

## Desync check (testing)

A second copy of the game, `reference_state`, that **never guesses**: it only
runs a tick once both players' inputs for it are confirmed.

Rollback promises that once the inputs for a tick are known, its saved state
for that tick is exactly what you'd get by simulating the real inputs from the
start. `desync_check(next_tick)` (`src/rollback/desync_check.c`) checks that promise: for every tick T that is
both confirmed (`T <= last_confirmed_remote_tick`) and already simulated
(`T < next_tick`), it compares the saved snapshot for T with `reference_state`
using `memcmp`, then steps the reference forward with the confirmed inputs.
Any difference logs `DESYNC at tick T` and asserts.

- It runs **after every tick**, not once per frame. After a long stall one
  frame can run more ticks than the state ring holds, and the snapshots the
  check still needs would be overwritten.
- It catches mistakes in rollback (wrong state restored, prediction not
  redone) and non-determinism in the sim that depends on history (state kept
  outside `GameState`, uninitialized padding).
- It **can't** catch the two machines computing different results from the
  same inputs (different compilers, floats). The hash exchange between peers
  does that: `sync_check_after_tick` runs right after it, under the same
  timing rule (see "Sync check" in `networking.md`).

## Overlay and stats

Stats are counted in `match.c` because that's where rollbacks and stalls
happen: `stats_note_rollback(depth)` in the rollback block,
`stats_note_stall()` at the stall `break`. Both per-second counts (for the
overlay) and totals (for the test) are kept. See `networking.md` for
what each overlay line means. `main.c` copies the stats and everything else
the overlay shows into an `OverlayInfo` each frame, so `render.c` only draws
and never reads the network or rollback modules itself.
