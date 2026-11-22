# How the rollback netcode works

A conceptual walkthrough of the netcode this game uses. It goes into more
detail than the proposal but stays above the code level. Implementation details
and open questions are in `rollback.md`.

## The core idea

Both machines run the **same deterministic simulation**. The only thing
that crosses the network is each player's controller input, one small bitmask
per tick. If both machines apply the same inputs to the same starting state,
they compute the same game, so there's never a need to send positions, health
or bullets.

The problem is that the opponent's input for a tick arrives late (half a
round trip at best). There are two ways to deal with that:

- **Delay-based (lockstep):** wait until you have the input before you simulate
  the tick. The game is always correct, but input lag grows with ping, and a lost
  packet freezes the game.
- **Rollback:** don't wait. **Guess** the opponent's input, simulate now, and
  if the guess turns out wrong, **rewind and redo** the ticks that used it.

We use a **hybrid**: a small fixed input delay plus rollback for whatever
latency the delay doesn't cover.

## Ticks, not frames

The simulation advances in fixed **ticks** at 60 per second, independent of
how often the screen redraws. A tick is identified by its number, counting
from 0 at the start of the match, and both peers use the same numbering. "Tick
N" means the same moment in the game on both machines, even though the two
machines reach it at slightly different wall-clock times.

Throughout this doc, **state N** is the game state at the start of tick N.
Applying both players' inputs for tick N to state N gives state N+1.

## The pieces

| Piece | What it is |
|---|---|
| **Input history** | For each recent tick, both players' inputs, plus whether the remote one is **confirmed** (actually received) or **predicted** (guessed). |
| **State history** | Copies of recent game states, so we can rewind to any tick still inside the rollback window. |
| **Input delay** | Local input sampled during tick N is recorded for tick N + D (D = `INPUT_DELAY`, 3). |
| **Rollback window** | The furthest back we're willing to rewind (`MAX_ROLLBACK`, 8 ticks). It bounds memory, CPU cost and how far apart the peers can drift. |
| **Network messages** | Unacknowledged local inputs, an ack of the peer's inputs, timing info for sync, and occasionally a state hash. |

## One tick, step by step

Each time the loop decides a tick is due:

1. **Receive.** Read every packet that has arrived. For each remote input:
   - If we predicted something different for that tick, note it as a
     **misprediction**. We only need the earliest one.
   - Store it and mark it confirmed.
   - Update what the peer has acked of *our* inputs.
2. **Roll back if needed.** If there was a misprediction at tick M:
   - Load state M.
   - Re-simulate M, M+1, … up to the present, using the now-corrected inputs.
     Remote inputs that still aren't confirmed are predicted again.
   - Overwrite the saved states along the way, since they were computed from
     wrong inputs.
3. **Check the limit.** If advancing would put us more than `MAX_ROLLBACK`
   ticks past the last confirmed remote input, **stall**: don't advance this
   frame (see "Stalling" below).
4. **Record local input.** Sample the keyboard and store the input for tick
   `now + D`.
5. **Send.** Send the peer every local input it hasn't acked yet, plus our
   ack of its inputs.
6. **Simulate one tick.** Get both inputs for the current tick (predicting the
   remote one if needed), run the sim, and save the new state.
7. **Render** the latest state.

During re-simulation the game doesn't render or play sounds. Only the final
result appears on screen, so a rollback looks like a small jump, not a
replay.

## Prediction

When the remote input for a tick isn't confirmed, we **repeat their last
confirmed input**. This works well for fighting games, because inputs are held
for many ticks and only change occasionally. Most predictions are correct, and
a correct prediction means nothing needs to be redone.

Two details:

- The prediction is **written into the input history**, so that when the real
  input arrives we compare it against what the sim actually used.
- During re-simulation, predictions for later unconfirmed ticks are made
  again, because "the last confirmed input" may have just changed.

Wrong predictions mostly happen at the **moment an input changes** (pressing
fire, changing direction), and those are exactly the moments players notice.
That's why rollback is combined with a little input delay.

## Input delay and why we keep some

If your input for tick N is recorded for tick N + 3, you see your own action
3 ticks (50 ms) after pressing. The opponent's machine also gets 3 extra ticks
before it needs that input, so with a one-way latency under 50 ms it usually
arrives in time and **no prediction is needed at all**.

| | Small delay, more rollback | Large delay, less rollback |
|---|---|---|
| Local feel | Responsive | Sluggish |
| Opponent on screen | Teleports / corrects more | Smooth |
| CPU | More re-simulation | Less |

A few ticks of delay hides most of a typical connection's latency. Rollback
covers the rest and handles jitter.

For the first D ticks of the match nobody could have sent input, so both
peers treat those ticks as "no buttons, already confirmed".

## Saved states and the rollback window

To rewind to tick M we need state M, so we save a copy of the state every
tick. We only keep the last `MAX_ROLLBACK + 1`. Anything older can't be rolled
back to, and the stall rule guarantees we never need to.

This only works because the whole game state is **one plain struct** with no
pointers, heap allocations or hidden globals, so saving and loading is a
single `memcpy`. Anything that affects the game and isn't in that struct
(a static variable in the sim, a timer read from the OS, `rand()`) breaks
rollback.

## Stalling

We can only roll back `MAX_ROLLBACK` ticks, so we can't run more than that
far past the last remote input we've actually received. When we hit that
limit we **stop advancing ticks**, but we keep:

- rendering (the screen doesn't freeze),
- receiving packets,
- **resending unacked inputs every frame**. If we only sent when a tick
  advanced, a lost packet could leave both sides waiting on each other forever.

The one check "how far am I past the peer's last confirmed input?" covers
every cause: the peer is slow, the peer is paused, or packets are delayed or
lost. You can't see the peer's actual tick, only the inputs you've received.

Because each peer can be up to `MAX_ROLLBACK` ticks ahead of what it has
confirmed, the **remote inputs you receive can be for ticks ahead of your own
current tick**. The input history has to have room for those as well as for
the past ticks you might roll back to.

## Sending inputs over UDP

UDP can drop, duplicate and reorder packets. Instead of adding reliability
on top the way TCP does (timeouts, retransmit queues), we use redundancy:

- **Every packet carries all local inputs the peer hasn't acked yet**, as a
  contiguous range starting from the first unacked tick. Inputs are tiny, so
  this costs almost nothing.
- **Acks are cumulative:** "I have all of your inputs up to tick N."
  Since inputs are sent as contiguous ranges, one number is enough.
- **The receiver only accepts the next tick it's missing.** Anything it already
  has is a duplicate and is ignored. Anything past a gap is also ignored,
  because it will come again in the next packet that fills the gap.
- Old or reordered packets are harmless: their inputs are duplicates and their
  acks are older than the one we already have.

A lost packet costs nothing extra. The next one (about 16 ms later) carries
the same inputs again.

## Time sync (frame advantage)

Even if both peers start at the same moment, their clocks drift, and one
inevitably ends up running a little ahead. The peer that's ahead does most of
the predicting and therefore most of the rolling back. It also keeps hitting the
stall limit, which shows up as stutter.

Each peer estimates its **frame advantage**:

```
peer's probable current tick ≈ last received remote tick + (RTT / 2) / tick length
advantage = my current tick − peer's probable current tick
```

The two peers exchange their advantages. The one that's ahead **slows
down slightly**, for example by skipping a tick every so often, until they're
balanced. This also absorbs any error in the start time, so the match start
doesn't need to be perfectly simultaneous.

The stall rule stays as a hard fallback. Time sync is what keeps it from
triggering in normal play.

## Starting the match

**Current version (a stand-in):** each peer starts ticking when the first
packet from the other one arrives. Both send every frame, so they start within
about one one-way trip of each other. The plan below is a nice-to-have.

1. The peers exchange packets to confirm the connection works in both
   directions and to measure round-trip time.
2. The two sides agree on which player each one is (player 0 and player 1),
   using opposite indices. Both machines put the same person's inputs in the
   same slot, because the sim treats the slots differently.
3. The host says "start". The guest starts when it receives that message, and
   the host waits about RTT/2 before starting, so both reach tick 0 at about the
   same time. Frame-advantage sync corrects whatever error is left.

## Determinism and desync detection

Everything above assumes that the same state plus the same inputs gives the
same next state, bit for bit, on both machines. This is why the sim:

- uses **integer / fixed-point math** (subpixels) instead of floats,
- reads input **only from the input history**, never from the keyboard,
- keeps all state in `GameState` and processes things in a fixed order.

If the machines ever diverge (a **desync**), rollback can't repair it, because
it only re-simulates. It never sends real state. So we detect it:

- A state is **final** once both players' inputs are confirmed for every tick
  before it. It can never be rolled back again.
- Every N ticks, each peer hashes a final state and sends `(tick, hash)`.
- If the hashes for the same tick differ, log it loudly. That's a bug in the
  sim, not a network problem.

A **local check** catches most determinism bugs without a second machine.
This project keeps a second copy of the game that never guesses: it only runs
a tick once both inputs are confirmed. Rollback promises that once the inputs
are known, its saved state for a tick is exactly what you'd get by simulating
the real inputs from the start, so the two are compared byte for byte (see
`main_loop.md`). Another common approach (GGPO's "sync test") is to roll back
a few ticks every tick and check the re-simulated state matches.

## A short example

D = 3, one-way latency ≈ 6 ticks. Player A presses fire during tick 10, so A's
input is recorded for tick 13.

| Wall-clock tick | On A | On B |
|---|---|---|
| 10 | Records fire for tick 13, sends it. | |
| 13 | Simulates 13 with its own fire. A's character starts shooting on A's screen. | Simulates 13 predicting A's old input (no fire). |
| 16 | | Receives A's tick 13 input: fire ≠ predicted. Loads state 13, re-simulates 13, 14, 15 with fire, then continues to 16. A's shot appears on B's screen already 3 ticks into its startup. |

B never froze, and B lost 3 ticks of A's startup animation. That loss is
latency minus input delay (6 − 3). The shot's short startup is what makes this
acceptable: a correction during startup is much less noticeable than a bullet
suddenly appearing mid-flight.

## Where each piece lives

| Concern | Where |
|---|---|
| Deterministic game step | `src/sim/` |
| Input/state history, prediction, misprediction tracking, stall check (`rollback.c`); local desync check (`desync_check.c`); cross-machine hash check (`sync_check.c`) | `src/rollback/` |
| Socket (`net.c`), wire format (`packet.c`), network simulator (`netsim.c`), packets ↔ rollback glue, ping and disconnect (`session.c`) | `src/net/` |
| Drawing the game and the overlay | `src/render/` |
| Command line (`args.c`), keyboard and netsim hotkeys (`input.c`) | `src/` |
| Tick timing, the order of the steps above, rollback/stall stats | `src/main.c` |

Details: `rollback.md`, `networking.md`, `main_loop.md`,
`sim_physics_rendering.md`, `combat_and_rounds.md`.

## References

- Infil, *Netcode explained*: https://words.infil.net/w01-netcode.html
- GGPO source: https://github.com/pond3r/ggpo
- GGRS time-sync notes: https://docs.rs/crate/ggrs/0.12.0/source/docs/time-synchronization.md
- Glenn Fiedler, *Fix Your Timestep*: https://gafferongames.com/post/fix_your_timestep/
