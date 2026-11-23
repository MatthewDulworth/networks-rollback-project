# Combat and rounds

Shooting, bullets, hits, health, rounds and matches, all in `src/sim/sim.c`.
Movement, collision and units are in `sim_physics_rendering.md`. The full
design (including things not built yet) is in `game_description.md`.

Everything here follows the determinism rules: integers only, no `rand()`,
fixed order, all state in `GameState`. The desync check (`src/rollback/desync_check.c`) would
assert on any mistake.

## Order within one tick (`sim()`)

1. Copy `prev` into `next`, `tick += 1`.
2. **If in the lobby** (`in_lobby`): only the ready flags and countdown
   change (`update_lobby`). Skip steps 3 to 5.
   **Else if a round just ended** (`round_over_time_left > 0`): everything is
   frozen except dead players, who keep flying from the knockback of the
   hit that killed them (`update_dead_player`: no input, no shooting).
   Count down, and when the timer hits 0, start the next round (or go
   back to the lobby). Skip steps 3 to 5.
3. **Players**, in index order: movement (`update_player`), then the gun
   (`update_gun`), which may fire a bullet.
4. **Bullets**, in index order: move, expire, hit (`update_bullets`).
5. **Deaths** (`check_round_end`).
6. Save this tick's input in `previous_input`. This happens even while
   frozen, so a button held through the pause doesn't count as a fresh press
   when the round starts.

Deaths are checked **after all bullets**, so if both players are killed on the
same tick it's a tie. Checking inside the bullet loop would favor whichever
bullet happened to sit first in the array.

## Aiming (`aim_velocity`)

8 directions, from the movement keys (WASD, including up and down). If none
are held, the way the player is facing. Straight down while grounded becomes
down-forward, since it would only hit the floor.

Opposite keys cancel out (`axis()` gives −1, 0 or +1).

Holding plant (K) on the ground stops the player moving: left/right still turn
them, so they can aim any of the 8 ways while standing still. In the air plant
does nothing. Plant is an input bit like the others, since it changes what the
sim does and both machines must see it.

Straight bullets move `BULLET_SPEED` (8 px/tick) along one axis. Diagonal
bullets move `BULLET_SPEED * 181 / 256` on **each** axis. 181/256 ≈ 0.707 ≈
1/√2, so diagonals travel at the same overall speed as straight shots. Floats
and `sqrt` could round differently on different machines. Integers can't.

## The gun (`update_gun`)

Each tick, in this order:

1. Count down the cooldown.
2. Count down the reload. When it hits 0, refill the magazine. Reloading
   **continues during hitstun**.
3. If a shot is in **startup**, count it down. When it hits 0: spawn the
   bullet, start the cooldown, use one ammo, and start the reload if that was
   the last bullet. Startup also continues during hitstun, so **getting hit
   doesn't cancel a shot**. While in startup, nothing else happens.
4. Otherwise, if fire was **just pressed** (`in & SHOOT && !(prev_in & SHOOT)`)
   and the player isn't in hitstun, cooldown or reload and has ammo: lock in
   the aim (`queued_bullet_velocity`) and start the startup. The aim is decided
   **on the press**, not when the bullet appears.

| Constant | Value | |
|---|---|---|
| `SHOOT_STARTUP_TICKS` | 6 | 100 ms from press to bullet |
| `SHOOT_COOLDOWN_TICKS` | 20 | after the bullet appears |
| `MAGAZINE_SIZE` | 3 | |
| `RELOAD_TICKS` | 90 | 1.5 s, starts automatically |

The short startup is good for rollback: when the remote player presses fire,
the correction usually lands during the startup (an aim line appearing a few
ticks late) instead of a bullet popping into existence mid-flight.

## Bullets (`spawn_bullet`, `update_bullets`)

- Fixed array of `MAX_BULLETS` (16) with an `active` flag, no allocation. A
  new bullet takes the first free slot. If none are free the shot is lost.
  That can't happen at these fire rates, but it would still behave the same on
  both machines.
- Spawns at the player's center, moving at `queued_bullet_velocity`.
- `spawn_bullet` sets fields one at a time instead of `*b = (Bullet){...}`.
  `Bullet` has padding bytes after `owner`, and assigning a whole struct can
  copy random bytes into them, which the byte-for-byte desync check would
  report as a desync.
- Each tick: move, lose a tick of lifetime (`BULLET_LIFETIME`, 90 ticks ≈
  720 px), and disappear if the lifetime ran out, it left the map, or it
  touches terrain.
- **No tunneling:** a bullet moves at most 8 px per tick and is 8 px wide, so
  it can't jump past a 10 px wide player or a 10 px platform in one step. A faster
  bullet would need to move in smaller steps.
- **Hits:** only the other player (no self-damage). A hit removes the bullet,
  takes 1 health, sets 20 ticks of hitstun, and sets the target's velocity to
  ¾ of the bullet's velocity plus a 2 px/tick upward pop, so grounded players
  actually move.

## Hitstun

While `hitstun_left > 0`, `update_player` ignores input and the knockback
velocity carries the player, slowing by 1/16 each tick
(`velocity.x -= velocity.x / 16`). Integer division rounds toward zero,
the same on every machine. The player can't start a new shot, but a shot
already in startup still comes out.

## Rounds and matches

- **Death:** health at 0, or the player box no longer touches the map at all
  (`MapBounds`). Falling into a side gap is the usual way.
- **Scoring:** if one player died, the other gets a kill. If both died on the
  same tick, nobody scores. `round_winner` records the result (−1 for a tie)
  so the renderer can show it.
- **Pause:** 90 ticks (1.5 s) after a round, 240 ticks (4 s) when someone
  reaches `KILLS_TO_WIN` (3). The game is frozen during the pause, apart
  from dead players finishing their knockback.
- **Reset** (`reset_round`): `memset` the players and bullets (padding
  included), then full health and ammo, spawn positions, `grounded = true`.
  Keeps tick, kills, round number and `previous_input`.
- **Match over:** after the long pause, back to the lobby.
- **Lobby** (`update_lobby`, `enter_lobby`): the game starts here, and comes
  back here after every match. Players are frozen at their spawns. A fresh
  fire press toggles that player's `ready`. While both are ready,
  `lobby_countdown_left` counts down from 180 ticks (3 s), and it resets if
  either un-readies. At 0 the kills and round number are cleared and the
  match starts. Kills are kept until then so the lobby can show who won.
  Readying is just input, so rollback keeps the lobby in sync like the rest
  of the game: no extra packets, and both peers start on the same tick.
  Before the peer connects no ticks run at all, so the overlay's "Waiting
  for peer..." banner is shown instead of the lobby panel.

## HUD (`render_game()` in `src/render/render.c`)

- Bullets in their owner's color.
- Player flickers while in hitstun. After the first hit (2 health, so one
  hit left) the body is faded with a crack drawn across it.
- The aim arrow is also the ammo display: a row of `MAGAZINE_SIZE` chevrons
  pointing where a shot fired now would go (from `sim_aim_velocity()` and the
  player's latest input, so it uses the same rules as the sim). Loaded rounds
  are filled, spent ones are outlines, and during a reload they fill up one
  by one. Faint when the player can't fire, further out while planted.
- During startup the outermost loaded chevron flies out along the locked-in
  aim; the bullet appears when it has gone the full distance.
- Score top right with each player's color.
- A centered message between rounds: who scored, a tie, or who won the match.

## Not built yet

From `game_description.md`: portals, active reload (`tried_active_reload` is in
`Player` but unused), and everything under "Things to explore".
