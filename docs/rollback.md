# Rollback buffers

Notes on the input and state ring buffers in `src/rollback/`: what they hold,
the design decisions behind them, what's implemented, and what's still open.

## Where things live

| File | Contains |
|---|---|
| `src/rollback/rollback.h` | `MAX_ROLLBACK`, `INPUT_DELAY`, `NO_TICK`, ring sizes, `InputSlot`/`StateSlot`, `extern` buffers and trackers, the public API. |
| `src/rollback/rollback.c` | Definitions of the buffers and trackers, `static local_player`, `static ring_index()`, and the function bodies. |

`CMakeLists.txt` adds `src/` as an include directory, so includes are written
from there: `"sim/sim.h"`, `"rollback/rollback.h"`.

## Two rings, not one generic ring

The input ring and state ring have different sizes, slot contents and
operations. The only shared logic is "tick → slot", which is the `static`
helper `ring_index()`. A generic `void *` ring would lose type checking for no
real gain. (GGPO also keeps them separate.)

| | Input ring | State ring |
|---|---|---|
| Slot | `tick`, `input[MAX_PLAYERS]`, `confirmed[MAX_PLAYERS]` | `tick`, `GameState` |
| Size | 16, must be ≥ `MAX_ROLLBACK + INPUT_DELAY + 1` | 16, must be ≥ `MAX_ROLLBACK + 1` |
| Holds future ticks? | Yes, because of input delay | No |

The input ring spans from the oldest tick we might roll back to
(`next_tick - MAX_ROLLBACK`) to the newest delayed local input
(`next_tick + INPUT_DELAY`). The resend window stays inside that because of the
stall rule: acks come with the peer's inputs, so if we're getting their inputs
we're getting acks, and if we aren't we stall.

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

## Still to implement

| Function | Notes |
|---|---|
| `get_inputs(tick, out)` | Predicts the remote input if it isn't confirmed, and **writes the prediction back** into the slot so a later comparison uses what the sim actually used. Predict again during re-simulation, because a corrected input changes later predictions. |
| `predict_input` | `static` helper: repeat the last confirmed remote input. |
| `write_local_input(tick, input)` | Writes into `input[local_player]`, confirmed. Called with `next_tick + INPUT_DELAY`. |
| `add_remote_input(tick, input)` | Only accepts `last_confirmed_remote_tick + 1`. Ignore duplicates and anything past a gap (it gets resent). Stores it, confirms it, advances the tracker, and sets `earliest_misprediction` if it differs from the stored prediction. |
| `receive_ack(acked_tick)` | Ignores acks older than the current one (out-of-order packets). |
| `can_advance(next_tick)` | False when `next_tick - last_confirmed_remote_tick > MAX_ROLLBACK`. |
| `find_rollback_point` / `clear_rollback_point` | Returns `earliest_misprediction` or `NO_TICK`; cleared after re-simulating. |
| `find_inputs_to_resend(&first, &count)` | From `last_acked_local_tick + 1` to the newest local input written. |

## Open decisions

- **Saved-state tick convention.** Is the state saved for tick N the state
  before tick N is simulated, or after? This decides whether `save_state` can
  assert `state->tick == tick`, where the tick 0 state gets saved, and the
  bounds of the rollback loop.
- **Where the rollback loop lives.** Load the state at the rollback point, then
  for each tick up to the present: get inputs, `sim()`, **save the new state**.
  Either `rollback.c` calls `sim()` itself, or `main.c` runs the loop and the
  module only stores data.
- **How `local_player` is chosen.** Command-line flag, or "host is player 0".

## Desync checking (later)

Not part of the rings. It splits across three places:

1. `sim_hash(const GameState *)` in the sim module (e.g. FNV-1a over the raw
   bytes). This is safe because padding is kept zeroed by `memset`/`memcpy`.
   Also used by the local sync test.
2. The rollback side knows when a state is **final** (all inputs up to that
   tick confirmed). Only final states can be compared, and they must be hashed
   before their slot is reused.
3. The network layer sends `(tick, hash)` every N ticks, keeps whichever side
   arrives first until the other shows up, and logs mismatches.
