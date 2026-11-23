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
| `GROUND_ACCEL` / `GROUND_FRICTION` / `GROUND_DRAG` | 384 / 384 / 128 | 1.5 / 1.5 / 0.5 px/tick² |
| `AIR_ACCEL` / `AIR_FRICTION` / `AIR_DRAG` | 256 / 128 / 16 | 1 / 0.5 / 0.0625 px/tick² |
| Player size | 2560 × 5120 | 10 × 20 px (w × h) |

Combat tuning is in `combat_and_rounds.md`.

Jump height = v² / 2g = 49 / 0.5 ≈ **98 px**, airtime ≈ **56 ticks**. A tap
(released immediately) halves v: 12.25 / 0.5 ≈ **24 px**.

## The map

Window and map are 640 × 450 px, mirrored left/right, with a ceiling on
top and a tower in the middle:

```
  y 125 >                                            <   high wrap-around (gold)
  y 190 ====[v]                                [v]====   high ledges
  y 270          ==[^]==     [tower]     ==[^]==         T2 platforms
  y 350 >=====                                  =====<   low ledges (red)
  y 430 >=========================[<]##[>]============<  floor (sky blue)
```

`MapCollision` holds 26 solid boxes (left side; the right is the mirror
image, `x' = 640 - x - width`):

| Box | Position (px) | Size (px) |
|---|---|---|
| Floor | x 0, y 430 | 640 × 20 |
| Wall pieces (gaps for the wall portals) | x 0, y 0 / 125 / 350 | 12 × 85 / 185 / 40 |
| Ceiling (gap for the sky portals) | x 12, y 0 | 268 × 12 |
| Tower core (one box, centred) | x 302, y 270 | 36 × 160 |
| Tower strips above the doors | x 290 / 338, y 270 | 12 × 120 |
| Low ledge | x 0, y 350 | 100 × 10 |
| T2 platform halves | x 150 / 225, y 270 | 35 × 20 |
| T2 backing under the notch | x 185, y 282 | 40 × 8 |
| High ledge, solid part | x 0, y 190 | 78 × 10 |
| High ledge, backing under the portal | x 78, y 202 | 40 × 8 |
| High ledge, lip at the inner end | x 118, y 190 | 12 × 20 |

- The climb goes floor → low ledge → T2 → high ledge. Each step is 80 px,
  within the 98 px jump. From T2 you can also jump across to the tower
  top. (The old 800 px map's climb was brute-forced in a headless test;
  this layout has only been playtested.)
- There are no death pits, so rounds end on health. The ceiling (12 px,
  `MAX_SPEED`, so nothing tunnels through) and the sky portals in its gap
  mean nothing can leave through the top.
- All boxes are solid from every side, so you bump your head jumping up into
  one from below. One-way platforms would need extra logic in `move_y`.
- `MapBounds` (the whole 640 × 450 area) is separate. Anything not touching
  it is "outside the map".

## Portals

`MapPortals` holds static portals, linked in pairs that sit next to each other
in the array, so portal `i`'s partner is `i ^ 1`. Each has a box (12 px thick)
and a `Facing`: the direction its opening faces. The renderer colours pair
`i / 2` with `pair_colors[i / 2]`, so the array order has to match that list.

| Pair | Portal | Box (px) | Facing | Turn |
|---|---|---|---|---|
| Orange | Tower left door | x 290, y 390, 12 × 40 | left | 90° |
| Orange | Right high ledge floor | x 522, y 190, 40 × 12 | up | |
| Purple | Tower right door | x 338, y 390, 12 × 40 | right | 90° |
| Purple | Left high ledge floor | x 78, y 190, 40 × 12 | up | |
| Green | Left T2 notch | x 185, y 270, 40 × 12 | up | 180° |
| Green | Right T2 notch | x 415, y 270, 40 × 12 | up | |
| Sky blue | Left wall, floor level | x 0, y 390, 12 × 40 | right | 0° |
| Sky blue | Right wall, floor level | x 628, y 390, 12 × 40 | left | |
| Red | Left wall above the low ledge | x 0, y 310, 12 × 40 | right | 90° |
| Red | Left sky portal | x 280, y 0, 40 × 12 | down | |
| Red | Right wall above the low ledge | x 628, y 310, 12 × 40 | left | 90° |
| Red | Right sky portal | x 320, y 0, 40 × 12 | down | |
| Gold | Left wall, high | x 0, y 85, 12 × 40 | right | 0° |
| Gold | Right wall, high | x 628, y 85, 12 × 40 | left | |

- **Doors and high ledges (orange, purple), crossed.** Walk into a tower
  door and you pop up out of the floor of the *other* side's high ledge: a
  fast way up and across. The other way round, drop onto the ledge portal
  and you dash out of the far door along the floor, your falling speed
  turned sideways. A solid lip at the ledge's inner end stops anything
  getting in from the side.
- **Trampolines (green).** Both face up, so the turn is 180°. Fall into one
  and you're thrown up out of the other just as fast, with your sideways
  direction mirrored. Jump off a high ledge into one to reach the other
  high ledge.
- **Wrap-arounds (sky blue, gold)** at floor level and above the high
  ledges: walk or jump into one wall and come out of the other.
- **Low walls and sky (red), two pairs.** Walk off a low ledge into the
  wall and you drop out of the top of the map above the tower. Pairs are
  strictly one-to-one, so the two sky portals sit side by side, one per
  wall.

**Backing.** A portal in the middle of the map needs terrain behind it, or you
could go round and fall through its back (entering needs moving *into* the
portal, so from behind nothing happens). The doors and T2 portals sit in 12 px
notches with terrain right behind them. The high-ledge portals have a backing
strip under them, like the T2 notches.
The 12 px thickness is what makes backing work: on the tick something first
overlaps a portal it has moved at most `MAX_SPEED` = 12 px into it, so it
can't also touch the backing. If it did, `move_x`/`move_y` would push it out
and zero its velocity before `try_portal` saw it.

`try_portal()` runs for each player after `move_x`/`move_y`, and for each
bullet after it moves (before the terrain and map-edge checks):

1. **Enter** if the box overlaps a portal *and* is moving into it: the dot
   product of velocity and the portal's outward normal is negative.
2. **Rotate** velocity by `k = (out - in + 6) % 4` quarter turns clockwise.
   That takes "into the entry" (entry facing + 2) to "out of the exit".
   A quarter turn is `(x, y) → (-y, x)`, so this is exact in integers.
3. **Minimum exit speed.** If the speed along the exit's facing is below
   `MIN_EXIT_SPEED` (4 px/tick), raise it to that. Without this, walking
   onto a floor portal (entering at gravity speed, 0.25 px/tick) comes out
   of the partner just as slowly, falls straight back in next tick and
   flickers between the two. Out of a floor portal, 4 px/tick is a 32 px
   hop. Only the part along the facing changes; sideways speed is kept.
4. **Place** the box just in front of the exit (touching, not overlapping),
   keeping its offset from the portal centre (rotated the same way), but
   clamped so the box stays within the exit's span.
5. At most one portal per tick.

Then, for players, `update_player` clears `grounded` and `coyote_left`.
`move_y` set `grounded` before the teleport, so it described where you were.
Left alone, walking into a door (grounded) gave a mid-air jump out of the
ceiling, straight back into it.

- **No instant re-entry.** After step 2 the box moves *out* of the exit, so
  step 1 can't fire on the exit. Moving back into it later (e.g. falling back
  into a floor portal) works straight away, as the design asks.
- **Momentum is kept** apart from direction (and the minimum exit speed).
  The drag rule in movement means holding forward doesn't cap it.
- **Same-facing pairs** (the trampolines) rotate 180°, which also flips
  the sideways direction. Opposite-facing pairs (the wrap) rotate 0°.
- **No tunneling.** Thickness + size beats speed: 12 + 8 > 8 px/tick for
  bullets, 12 + 10 > 12 for players.
- **Placing new portals:** keep open air directly in front of each exit, at
  least a player deep (step 4 keeps things within the exit's span), and
  give mid-map portals backing.

## One tick (`sim()`)

1. `memcpy(next, prev)`, then `next->tick += 1`. Read "before" values from
   `prev`, write to `next`. `restrict` means they must not overlap.
2. For each player **in fixed order (P1 then P2)**: `update_player()`.
3. Bullets, deaths and round logic (see `combat_and_rounds.md`).
4. Store this tick's input in `previous_input`.

`update_player()`:

1. **Input → intent**, skipped while `hitstun_left > 0` so knockback survives
   (hitstun counts down and the knockback slows instead).
   - Horizontal velocity moves *toward* `dir * MOVE_SPEED` by at most a
     step per tick (`approach()`). The step is DRAG when faster than walking
     speed and not holding the opposite way, ACCEL while pushing, otherwise
     FRICTION. ACCEL and FRICTION are high so walking feels snappy (2 ticks
     to full speed or to a stop on the ground); DRAG is low so knockback and
     portal momentum decay gradually instead of being cut off. Ground and
     air use different steps, picked by last tick's `grounded`.
   - `facing_right` follows the last move direction.
   - Jump uses the **press edge** (`in & JUMP && !(prev_in & JUMP)`), so
     holding jump doesn't bunny-hop. The press starts a **jump buffer**
     (`JUMP_BUFFER_TICKS` = 6); the jump fires on any tick the buffer is
     live and the player is `grounded` or in **coyote time**
     (`COYOTE_TICKS` = 6 after leaving the ground). Jumping clears both.
   - **Variable height**: while rising from a jump (`in_jump`), not holding
     jump halves `velocity.y` once. Full jump ≈ 98 px, instant release
     ≈ 24 px. A hit clears `in_jump` so knockback is never cut.
   - The coyote and buffer timers count down every tick, including hitstun.
2. **Gravity**: `velocity.y += GRAVITY`.
3. **Clamp** each axis to ±`MAX_SPEED`. This is the design doc's max speed. It
   also keeps fall speed (12 px) below floor thickness (20 px), so no
   tunneling.
4. **`move_x()` then `move_y()`**: separate-axis collision.
   Then `try_portal()`; after a teleport, `grounded` and `coyote_left` are
   cleared (see Portals).
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
| Move | A / D |
| Aim (8 directions) | WASD (else facing) |
| Plant (hold, on ground: aim without moving) | K |
| Jump | Space |
| Shoot | J |
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
