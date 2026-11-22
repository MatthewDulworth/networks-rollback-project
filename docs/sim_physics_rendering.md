# Sim, physics and rendering

Notes on the simulation's foundations: units, player movement, collision,
the map and rendering. Shooting, bullets, health and rounds are in
`combat_and_rounds.md`. The main loop is in `main_loop.md`.

## Where things live

| File | Contains |
|---|---|
| `src/sim/sim.h` | The public contract: units and sizes, rules the HUD needs (`MAX_HEALTH`, `MAGAZINE_SIZE`, ...), `IntVector2`, `Collision`, `Input` bits, `extern MapCollision`, `GameState` and its structs, `sim_init()`, `sim()`, `sim_match_winner()`. |
| `src/sim/sim.c` | Everything private: tuning constants, the map, `static` helpers, and the public functions. |
| `src/main.c` | Window, keyboard, drawing, waiting between frames. |
| `src/match.c` | The fixed-timestep loop: which ticks run each frame, rollback, netcode. |
| `src/input.c` | Keyboard → `Input` bitmask (`read_local_input()`). |
| `src/render/render.c` | `render_game()` and the debug overlay. |

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
- C integer `/` rounds toward zero (`-1/256 == 0`). It's deterministic. On
  **velocities** it's symmetric (+17/16 and −17/16 both become ±1), so the
  combat code uses it for knockback and decay. On **positions** it's
  asymmetric around 0 (x = −100 and x = +100 subpixels both become pixel 0),
  so don't divide positions in the sim.

### Current tuning (`sim.c`)

| Constant | Value | In pixels |
|---|---|---|
| `GRAVITY` | 64 | 0.25 px/tick² |
| `MOVE_SPEED` | 768 | 3 px/tick (180 px/s) |
| `JUMP_VELOCITY` | -1792 | 7 px/tick up |
| `MAX_SPEED` | 3072 | 12 px/tick per axis |
| Player size | 2560 × 2560 | 10 × 10 px |

Combat tuning is in `combat_and_rounds.md`.

Jump height = v² / 2g = 49 / 0.5 ≈ **98 px**, airtime ≈ **56 ticks**.

## The map

Window and map are 800 × 450 px. `MapCollision` holds four solid boxes:

| Box | Position (px) | Size (px) |
|---|---|---|
| Floor | x 100, y 430 | 600 × 20 |
| Left platform | x 30, y 350 | 170 × 10 |
| Right platform | x 600, y 350 | 170 × 10 |
| Middle platform | x 320, y 270 | 160 × 10 |

- The floor stops 100 px short of each side. Falling into a gap takes you off
  the map, which kills you.
- The side platforms hang over the gaps, so you can jump across them.
- Platforms are 80 px apart vertically, within the 98 px jump.
- All boxes are solid from every side, so you bump your head jumping up into
  one from below. One-way platforms would need extra logic in `move_y`.
- `MapBounds` (the whole 800 × 450 area) is separate. Anything not touching
  it is "outside the map".

## One tick (`sim()`)

1. `memcpy(next, prev)`, then `next->tick += 1`. Read "before" values from
   `prev`, write to `next`. `restrict` means they must not overlap.
2. For each player **in fixed order (P1 then P2)**: `update_player()`.
3. Bullets, deaths and round logic (see `combat_and_rounds.md`).
4. Store this tick's input in `previous_input`.

`update_player()`:

1. **Input → intent**, skipped while `hitstun_left > 0` so knockback survives
   (hitstun counts down and the knockback slows instead).
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
5. **`update_gun()`** (see `combat_and_rounds.md`).

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
- Fast small things can tunnel (jump past a thin wall in one step). Bullets
  are slow and big enough that they can't yet (see `combat_and_rounds.md`). A
  faster bullet would need substepping: move in N small steps per tick and
  check each one.

## Setup and the main loop (`match.c`)

- `sim_init(&state)` builds tick 0: `memset` to zero (padding included, so the
  state can be compared byte for byte), then `reset_round` puts the players
  on the floor at ¼ and ¾ map width, facing each other, with full health and
  ammo. It lives in the sim because both peers must produce byte-identical
  starting states.
- The main loop steps it through `process_tick` (see `main_loop.md`).
  It uses `memcpy` instead of `state = next_state`, because C doesn't
  guarantee that struct assignment copies padding bytes.

### Controls

Each machine controls one player, with the same keys on both:

| Action | Keys |
|---|---|
| Move | A / D (W / S also count for aiming) |
| Jump | Space |
| Aim (8 directions) | Arrow keys |
| Shoot | J or Right Shift |
| Network simulator | 1/2 lag, 3/4 jitter, 5/6 loss, 0 reset (see `networking.md`) |

## Rendering (`render_game()` in `src/render/render.c`)

- It takes `const GameState *`, so the compiler enforces that drawing can't
  change the game.
- The map is drawn straight from `MapCollision` (dark gray), so what you see is
  exactly what you collide with. Keep this as a debug view even after adding
  art.
- Players: P1 blue, P2 red, with a small black mark on the facing side.
- Bullets, health, ammo, score and round messages: see `combat_and_rounds.md`.
- The debug overlay: see `networking.md`.
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

- No acceleration/friction. Horizontal velocity is set directly from input,
  and knockback only blends in because input is ignored during hitstun.
- Not done yet: portals.
