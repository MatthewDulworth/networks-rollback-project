# MVP Plan

Goal for the presentation: two machines play a real round over UDP, the network
can be made worse live, and on-screen numbers show rollback handling it.

Each step is one commit and leaves the game working, so we can stop at any point
and still have something to demo.

## Two machines

The demo runs on two separate laptops on the same network, not two windows on
localhost.

- Each peer is started with the other's LAN IP: `scripts/player1.sh <peer_ip>`
  on one, `scripts/player2.sh <peer_ip>` on the other.
- Show the local IP in the window title or overlay so it's easy to read off
  during setup.
- macOS asks to allow incoming connections the first time. Accept it on both
  machines before the presentation.
- Test on the actual network you'll present on. Campus Wi-Fi may block traffic
  between clients. A phone hotspot is a good backup.
- The network simulator applies to both directions on the machine where it's
  turned on, so one laptop controls the whole link during the demo.

## Phase 1: Show the netcode working (done)

Explained in `networking.md`.

**1. Network condition simulator** (`src/net/netsim.c`)
- Sits between the game and `net_send`/`net_receive`. Packets wait in a fixed
  queue until now + latency ± jitter, and each one is dropped with probability
  `loss%`.
- Uses its own random number generator. The sim never calls `rand()`.
- Command line flags `--lag <ms> --jitter <ms> --loss <pct>`, plus hotkeys to
  change lag and loss live during the demo.
- Also tests the resend, gap and stall code paths, which a clean connection
  never reaches.

**2. Debug overlay**
- Ping, rollbacks per second, max rollback depth, frames stalled, and the
  current network simulator settings.
- RTT is measured locally: remember when each tick was first sent, and when its
  ack comes back, the difference is the round trip. No packet changes needed.

**3. Disconnect timeout**
- No packet for ~3 seconds shows "Peer disconnected" instead of freezing at the
  rollback limit.

## Phase 2: Give it a game (done)

Explained in `combat_and_rounds.md`. Controls: arrows aim, J or Right Shift
shoots.

All sim changes use integer math only, so the existing desync check catches any
determinism mistake immediately.

**4. Shooting and combat**
- Aim with the arrow keys (8 directions, falling back to movement direction,
  then facing direction), shoot with J.
- Short startup, cooldown, small magazine with automatic reload.
- Bullets move, expire after a lifetime, disappear on terrain or off the map,
  and hit the opponent for 1 damage with knockback and hitstun.
- Diagonals use a fixed integer factor (181/256 ≈ 1/√2).

**5. Rounds**
- 3 health. Dying (including falling off the map) gives the opponent a kill.
  Dying on the same tick gives no kill.
- Short pause, reset positions, first to 3 kills wins.
- HUD for health, ammo and kills. A couple of platforms on the map.

## Phase 3: Networking extras, in order of value (all done)

**6. Packet format v2** (magic `RBF2`): add the sender's current tick and a
`(hash_tick, hash)` pair, so the format only changes once.

**7. State hash exchange**: hash (FNV-1a) a confirmed tick's `GameState` and
send it. The receiver compares it against a small ring of its own hashes and
shows **DESYNC** on the overlay if they differ. This checks that the two
machines agree, which the local desync check can't.

**8. Frame advantage sync**: estimate the remote tick as
`their_tick + (RTT / 2) / STEP`. If we're more than one tick ahead, push
`start_time` forward slightly. Removes the stutter at the stall limit.

**9. Delay-based mode** (`--delay`): only advance ticks whose inputs are
confirmed. Allows a side-by-side comparison with rollback under the same
network conditions. See "Delay mode" in `networking.md`.

## Nice to have

Not needed for the presentation, but worth doing afterwards or mentioning as
next steps:

- Portals
- Active reload
- Real start handshake (see `follow_ups.md`, "Shared start time")
- Catch-up cap for the main loop (see `follow_ups.md`, "Main loop")
- Matchmaking server
- Download server

## Demo script

1. Two laptops, clean connection: play for a bit, point out zero or few rollbacks.
2. Add 100 ms of lag: still responsive, rollback counter climbs.
3. Add 10% loss: still playable, resends absorb it.
4. Restart both with `--delay` and the same network settings: show it stalls
   and feels sluggish.
5. Point to the hash check staying green the whole time.
