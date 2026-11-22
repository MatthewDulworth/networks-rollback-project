# Main loop

What `src/main.c` does each frame, in order, and why the order matters. This
is where the sim, the rollback module and the network meet.

## Startup

1. `parse_args`: `<local_port> <peer_ip> <peer_port> <player 0|1>`, then
   optional `--lag`, `--jitter`, `--loss` (see `networking.md`).
2. `net_init` **before** the window opens, so a bad address or busy port
   exits immediately with an error.
3. `netsim_init` and apply the flags. Look up the local IP for the overlay.
4. Open the window. Player 1's window goes on the left edge of the screen,
   player 2's on the right, so two local copies don't start on top of each
   other.
5. `sim_init(&state)`, `rollback_init(player)`, `desync_check_init(&state)`.

## Starting the match

The stand-in for a real start handshake: don't tick until the first packet
from the peer arrives. `start_time` is set to that frame's time, and every tick
time is measured from it. Both sides send every frame, so each starts within
about one one-way trip of the other. A peer that starts a little late just
runs a little behind, and rollback covers the difference.

## One frame

| Step | What | Why here |
|---|---|---|
| 1. Input | Read the keyboard into `local_input` (`IsKeyDown` only). Handle the netsim hotkeys. | Sampled once per frame. |
| 2. Receive | `netsim_update`, then `receive_packets`. Update `last_packet_time`, start the match on the first packet, update the disconnect flag and stats window. | **Before rollback**, so the newest remote inputs fix the past before new ticks are built on top of it. |
| 3. Rollback | If `find_rollback_point()` gives tick M: `load_state(M)`, then `process_tick` for M up to `next_tick - 1`, then `clear_rollback_point()`. Counted for the overlay. | Asserts depth ≤ `MAX_ROLLBACK`. `can_advance` guarantees that, so a deeper rollback means a bug. |
| 4. Simulate | Run every tick that's due (see below). | |
| 5. Send | `send_packet`, then `netsim_update` again. | **Every frame**, even while stalled or waiting for the peer (see `networking.md`). |
| 6. Render | Game, then overlay. | Reads the state only. |
| 7. Wait | Sleep until the next tick is due, then poll input again. | |

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

After drawing, `WaitTime` sleeps until the next tick is due. `EndDrawing`
already polled input, but that was before the sleep, so `PollInputEvents` is
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
start. `desync_check(next_tick)` checks that promise: for every tick T that is
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
  same inputs (different compilers, floats). That needs the hash exchange
  between peers (MVP plan step 7).

## Overlay and stats

Stats are counted in `main.c` because that's where rollbacks and stalls
happen: `stats_note_rollback(depth)` in the rollback block,
`stats_current.stalled_frames++` at the stall `break`. See `networking.md` for
what each overlay line means.
