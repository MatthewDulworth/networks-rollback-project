# Rollback buffers

Notes on the input and state ring buffers in `src/rollback/`: what they hold,
the design decisions behind them, and what each function does. For the
concepts behind rollback, read `rollback_overview.md` first. For how the main
loop uses this module, see `main_loop.md`.

## Where things live

| File | Contains |
|---|---|
| `src/rollback/rollback.h` | `MAX_ROLLBACK` (8), `INPUT_DELAY` (3), `NO_TICK` (-1), and the public functions. |
| `src/rollback/rollback.c` | Ring sizes, `InputSlot`/`StateSlot`, the buffers and trackers (all `static`), `ring_index()`, and the function bodies. |

Everything except the API is `static`, so other files can only change the
buffers through the functions. That keeps the invariants (ticks written in
order, acks only moving forward) in one place.

`CMakeLists.txt` adds `src/` as an include directory, so includes are written
from there: `"sim/sim.h"`, `"rollback/rollback.h"`.

## Two rings, not one generic ring

The input ring and state ring have different sizes, slot contents and
operations. The only shared logic is "tick → slot", which is the `static`
helper `ring_index()`. A generic `void *` ring would lose type checking for no
real gain. (GGPO also keeps them separate.)

| | Input ring | State ring |
|---|---|---|
| Slot | `tick`, `input[MAX_PLAYERS]`, `confirmed[MAX_PLAYERS]`, `remote_predicted` | `tick`, `GameState` |
| Size | 32, must be ≥ `2 * (MAX_ROLLBACK + INPUT_DELAY) + 1` | 16, must be ≥ `MAX_ROLLBACK + 1` |
| Holds future ticks? | Yes, because of input delay and a peer running ahead | No |

The input ring has to hold two windows at once:

- **Rollback:** from the oldest tick we might roll back to
  (`next_tick - MAX_ROLLBACK`) up to the newest delayed local input
  (`next_tick + INPUT_DELAY`).
- **The peer running ahead:** its inputs can arrive for ticks up to about
  `MAX_ROLLBACK + INPUT_DELAY` past our own newest tick, and our unacked local
  inputs (still needed for resending) stretch back about as far.

Both come to roughly `2 * (MAX_ROLLBACK + INPUT_DELAY)` live ticks, so 32
slots. The resend window stays inside that because of the stall rule: acks
come with the peer's inputs, so if we're getting their inputs we're getting
acks, and if we aren't we stall.

`remote_predicted` is set when the sim runs a tick on a guessed remote input.
Only those ticks can be mispredicted. An unsimulated slot just holds a
placeholder 0, which means nothing.

## Ring indexing

```c
((uint32_t) tick) & (size - 1)
```

- `Tick` is signed, and C's `%` keeps the left side's sign: `-1 % 16 == -1`,
  an out-of-bounds index. Casting to unsigned avoids that.
- After the cast, -1 is 2³² − 1. That only lands in the last slot if the size
  divides 2³², i.e. is a power of two. With size 14, tick -1 would land in slot
  3 and collide with tick 3. This applies to `%` too, not just the mask.
- `_Static_assert`s in the header check both size and power-of-two at compile
  time, so changing `MAX_ROLLBACK` can't silently break the ring.

`ring_index` says where a tick **would** live. Ticks N, N+16, N−16 share a
slot. Every slot stores its `tick`, and readers compare it with the tick
they asked for to know what's actually there.

## Player slots

`local_player` is a **runtime** value passed to `rollback_init()`, and the two
peers must use opposite values. The remote index is `1 - local_player`.

Each peer can't just put itself in slot 0: the sim treats the indices
differently (spawn side, update order, `bullet.owner`, `kills[i]`), so both
machines must put the same person's inputs in the same slot. If each player
should see themselves on the left, mirror the view in rendering.

## Confirmed flags and the two directions

- `confirmed[p]` is **per player**. Local input is always confirmed; only the
  remote input can be a prediction.
- **Inputs to resend** = local inputs the peer hasn't **acked**
  (`last_acked_local_tick`). Not the same as remote inputs we haven't
  **received** (`last_confirmed_remote_tick`). They usually move together but
  differ under asymmetric loss or when the peers are at different ticks.
- The ack is cumulative ("I have all your inputs up to N"), like TCP. Inputs
  are sent in order as contiguous ranges, so one number is enough.

## `rollback_init(player)`

1. Asserts `player` is 0 or 1 and stores it in the file-private `local_player`.
2. Zeroes both rings and sets every slot's `tick` to `NO_TICK` (-1). Without
   this every slot would claim to hold tick 0.
3. Prefills ticks `0 .. INPUT_DELAY-1` with no buttons, **confirmed for both
   players**. Nobody ever sends input for those ticks (local input at tick 0 is
   written to tick `INPUT_DELAY`), so both peers fill them the same way.
4. `last_acked_local_tick = last_confirmed_remote_tick = INPUT_DELAY - 1`. Both
   sides already "have" the prefilled ticks, so the first input to send or
   receive is `INPUT_DELAY`. Using `NO_TICK` here would make
   `add_remote_input` wait forever for a tick 0 that never comes.
5. `earliest_misprediction = NO_TICK`.

## `save_state` / `load_state`

- Copy into and out of `slot->state`, not the slot itself (the slot starts
  with `tick`). `memcpy` takes `void *`, so the compiler can't catch that.
- `save_state` writes the slot's `tick`. `load_state` asserts that it matches.
- Both assert `tick >= 0`, because `NO_TICK` marks empty slots and must never
  be used as a key.

### Assert vs return value

- If it can only happen because our code is wrong → **assert**. A missing
  state in `load_state` means a broken invariant (stall check, ring size,
  off-by-one), and the caller couldn't recover from `false` anyway.
- If the outside world can cause it → **handle it**. `add_remote_input` gets
  network data, so duplicates, old ticks and gaps are normal there. Ignore
  them, never assert.
- `assert` disappears in `Release` builds (`NDEBUG`). The project defaults to
  `Debug`.

## The functions

| Function | What it does |
|---|---|
| `get_inputs(tick, out)` | Returns both inputs for a tick. If the remote one isn't confirmed, it predicts it (repeats the last confirmed remote input), **writes the prediction back** into the slot and sets `remote_predicted`, so a later comparison uses what the sim actually used. During re-simulation it predicts again, because a corrected input changes later predictions. |
| `write_local_input(tick, input)` | Stores our input, confirmed. Called with `next_tick + INPUT_DELAY`. Asserts ticks come in order with no gaps and are never rewritten, because once an input has been sent it can't change. |
| `add_remote_input(tick, input)` | Only accepts `last_confirmed_remote_tick + 1`. Duplicates and anything past a gap are ignored (it gets resent). Stores it, confirms it, advances the tracker, and sets `earliest_misprediction` if the tick was simulated on a guess that differs. |
| `receive_ack(acked_tick)` | Moves `last_acked_local_tick` forward. Older acks (out-of-order packets) are ignored. |
| `can_advance(next_tick)` | False when `next_tick - last_confirmed_remote_tick > MAX_ROLLBACK`. This is the stall rule. |
| `find_rollback_point` / `clear_rollback_point` | Returns `earliest_misprediction` or `NO_TICK`. Cleared after re-simulating. |
| `find_inputs_to_resend(&first, &count)` | From `last_acked_local_tick + 1` to the newest local input written. Asserts the range fits in the ring, because older inputs would already be overwritten. |
| `get_local_input(tick)` | Our input for a tick still in the ring, used to fill outgoing packets. |
| `get_last_confirmed_remote_tick()` | The ack we send back. |

`claim_slot(tick)` is the private helper behind both writers. If the slot still
holds an older tick, it's wiped and taken over. If it already holds this tick
(local and remote inputs for the same tick can arrive in either order), it's
returned untouched.

## Decisions made

- **Saved-state tick convention:** the state saved for tick N is the state
  **before** tick N runs (so `state->tick == N`, and `save_state` asserts it).
  `process_tick` in `match.c` saves, gets inputs, then steps the sim.
- **Where the rollback loop lives:** in `match.c`. This module only stores data
  and answers questions. It never calls `sim()`. That keeps it testable on its
  own and keeps the order of operations visible in one place.
- **How `local_player` is chosen:** the fourth command-line argument (0 or 1).
  The two peers must use opposite values.

## Desync checking

**Local check** (`desync_check.c`): keeps a second copy of the game that only
advances once both inputs for a tick are confirmed, and compares it byte for
byte with the rollback side's saved snapshot. See `main_loop.md`.

**Between machines** (`sync_check.c`): hashes each snapshot with FNV-1a once
it's final (all inputs before it confirmed), before its slot in the state ring
is reused. `session.c` sends the newest hash in every packet and hands
received ones back, and the overlay shows a mismatch. See "Sync check" in
`networking.md`.
