# Sim, physics and rendering

Notes on the first playable version of the simulation: player movement,
floor collision, rendering, and calling the sim from the main loop.

## Where things live

| File | Contains |
|---|---|
| `src/sim/sim.h` | The public contract: units and sizes, `IntVector2`, `Collision`, `Input` bits, `extern MapCollision`, `GameState` and its structs, `sim_init()`, `sim()`. |
| `src/sim/sim.c` | Everything private: tuning constants, the `MapCollision` definition, floor constants, `static` helpers, and the two public functions. |
| `src/main.c` | Window, input sampling, fixed-timestep loop, `render()`. |

The rule: **the header holds only what another file needs** to create, step,
draw or send a `GameState`. If changing something should never force an edit in
`main.c` or the netcode, it goes in `sim.c`.

- `MapCollision` is *declared* `extern` in the header and *defined* once in
  `sim.c`. A definition in the header would give every including `.c` file its
  own copy, which causes duplicate-symbol link errors.
- Helpers are `static` (file-private). Otherwise they're global symbols that
  can clash with same-named functions in other files.
- Types must be declared before use: `IntVector2` comes before `Collision`.
- `PLAYER_WIDTH/HEIGHT` are public only because the renderer needs them. If
  player size ever varies (crouch), replace them with a function like
  `Collision sim_player_box(const Player *)`.

## Units: fixed-point subpixels

- All sim positions, sizes and velocities are integers in **subpixels**:
  1 px = `SUB_PIXEL_FACTOR` (256).
- Velocity is subpixels/tick, gravity is subpixels/tick².
- Why: floats can produce different results on different compilers and CPUs,
  which means desyncs. Integers are exact. Whole pixels are too coarse (you
  can't express 0.25 px/tick²), so we scale.
- Division only happens in rendering (`to_px()`), which uses float. That's safe
  because rendering never feeds back into the sim.
- C integer `/` rounds toward zero (`-1/256 == 0`). It's deterministic, but
  asymmetric left vs right, so avoid it in the sim.

### Current tuning (`sim.c`)

| Constant | Value | In pixels |
|---|---|---|
| `GRAVITY` | 64 | 0.25 px/tick² |
| `MOVE_SPEED` | 768 | 3 px/tick (180 px/s) |
| `JUMP_VELOCITY` | -1792 | 7 px/tick up |
| `MAX_SPEED` | 3072 | 12 px/tick per axis |
| Player size | 2560 × 2560 | 10 × 10 px |
| Floor | full width, 20 px tall | top at y = 430 px |

Jump height = v² / 2g = 49 / 0.5 ≈ **98 px**, airtime ≈ **56 ticks**.

## One tick (`sim()`)

1. `memcpy(next, prev)`, then `next->tick += 1`. Read "before" values from
   `prev`, write to `next`. `restrict` means they must not overlap.
2. For each player **in fixed order (P1 then P2)**: `update_player()`.
3. Store this tick's input in `previous_input`.

`update_player()`:

1. **Input → intent**, skipped while `hitstun_left > 0` so knockback survives.
   - Horizontal velocity is *set* to `dir * MOVE_SPEED`. There's no
     acceleration or friction yet.
   - `facing_right` follows the last move direction.
   - Jump fires on the **press edge** (`in & JUMP && !(prev_in & JUMP)`) and
     only when `grounded`. Holding jump doesn't bunny-hop.
2. **Gravity**: `velocity.y += GRAVITY`.
3. **Clamp** each axis to ±`MAX_SPEED`. This is the design doc's max speed. It
   also keeps fall speed (12 px) below floor thickness (20 px), so no
   tunneling.
4. **`move_x()` then `move_y()`**: separate-axis collision.

## Collision: separate-axis resolution

Move one axis, check overlap, push out. Then do the other axis.

- Because only one axis moved, any overlap came from that axis, and the
  velocity's sign says which side was hit. Moving right means snap to
  `wall.x - PLAYER_WIDTH`. This removes corner ambiguity.
- After snapping, the boxes share an edge exactly. `colliding()` uses strict
  `<`/`>`, so shared edges *don't* count as overlap and the next tick starts
  clean.
- The blocked axis's velocity is zeroed so it doesn't build up.
- `move_y()` clears `grounded` and sets it only when pushed *up* out of
  terrain. A standing player still gets gravity each tick, sinks 0.25 px, and
  gets pushed back. That keeps `grounded` true without a separate ground probe.
- `grounded` is stored in `Player` (in `GameState`), not computed in a local.
  Anything that affects the next tick **must** be in the state, or rollback
  can't restore it.

Known limits:
- With several overlapping boxes, snapping out of one can push the player
  into another. That's fine for simple axis-aligned maps, but watch for it as
  the map grows.
- Fast small things (bullets) can tunnel. The plan is to substep them (move in
  N small steps per tick, check each one).

## Setup and the main loop (`main.c`)

- `sim_init(&state)` builds tick 0: `memset` to zero (padding included, so the
  state can be hashed later), players on the floor at ¼ and ¾ map width,
  facing each other. It lives in the sim because both peers must produce
  byte-identical starting states.
- Each tick in the catch-up loop:
  `sim(input, &state, &next_state); memcpy(&state, &next_state, ...)`.
  - It uses `memcpy` instead of `state = next_state`, because C doesn't
    guarantee that struct assignment copies padding bytes.
  - Rollback will replace this with a ring buffer of saved states.
- Catch-up ticks in one frame all get the same input sample. Press detection
  still works: only the first of those ticks sees a press edge, since
  `previous_input` updates every tick.
- The old diagnostics (delta/tick text, ticks-per-frame `printf`) were removed.

### Controls

| | Left | Right | Up | Down | Jump |
|---|---|---|---|---|---|
| P1 | A | D | W | S | Space |
| P2 | L | ' | P | ; | , |

Up/down are sampled but not used by the sim yet.

## Rendering (`render()` in `main.c`)

- It takes `const GameState *`, so the compiler enforces that drawing can't
  change the game.
- The map is drawn straight from `MapCollision` (dark gray), so what you see is
  exactly what you collide with. Keep this as a debug view even after adding
  art.
- Players: P1 blue, P2 red, with a small black mark on the facing side.
- `DrawRectangleRec` with float `Rectangle`s, so subpixel positions aren't
  truncated to ints.
- Interpolation or smoothing, if added later, belongs here, never in the sim.

## Determinism checklist for future sim code

- No floats, no `rand()`, no clock, no keyboard reads in the sim. Only the
  `Input` array.
- Fixed iteration order (players by index, bullets by index).
- All state that affects the next tick lives in `GameState`. No `static`
  locals.
- Copy states with `memcpy` to keep padding zeroed.

## Open items

- `health` is 0 after `sim_init`. Set it to 3 once something reads it.
- `hitstun_left` gates input but nothing counts it down yet.
- On tick 1, `grounded` is still false (it becomes true after the first
  gravity + push-out), so a jump pressed on the very first tick is ignored.
  A pre-round countdown would hide this.
- No acceleration/friction. `velocity.x += accel` with friction would let
  knockback blend with input.
- Not done yet: bullets, portals, death bounds, rounds.
