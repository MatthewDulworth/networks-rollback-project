# Networking guide

A single-file overview of how this game's netcode works, from the socket up to
rollback, with the reasoning behind each decision. It condenses
`rollback_overview.md`, `rollback.md`, `networking.md` and `main_loop.md`,
and has been checked against the code. Go to those files for the full detail.

---

## 1. The big picture

Two laptops connect **peer to peer over UDP** (no server). Each runs the
**same deterministic simulation** at 60 ticks per second. The only thing that
crosses the network is each player's **controller input**: a 16-bit bitmask
per tick. Same starting state + same inputs = same game, so positions, health
and bullets are never sent.

The problem: the opponent's input for a tick shows up late (at least half a
round trip). There are two ways to deal with that:

| | Delay-based (lockstep) | Rollback (what we use) |
|---|---|---|
| Missing remote input | Wait for it | Guess it, run now |
| Wrong guess | Can't happen | Rewind to that tick, re-simulate with the real input |
| Bad network | Input lag grows, game freezes | Game stays full speed, opponent "corrects" a bit |

We use a **hybrid**: 3 ticks (50 ms) of fixed **input delay**, plus
**rollback** of up to 8 ticks for whatever latency the delay doesn't cover.
Delay-based mode is still in the game (`--delay`) to compare the two.

### Key constants

| Constant | Value | Where | Meaning |
|---|---|---|---|
| `STEP` | 1/60 s | `sim.h` | Length of one tick |
| `INPUT_DELAY` | 3 ticks | `rollback.h` | Input sampled at tick N is used at tick N+3 |
| `MAX_ROLLBACK` | 8 ticks | `rollback.h` | Furthest we'll ever guess ahead / rewind |
| `PACKET_MAX_INPUTS` | 32 | `packet.h` | Most inputs in one packet |
| `DISCONNECT_TIMEOUT` | 3 s | `session.c` | No packets this long → "Peer disconnected" |
| Input ring | 32 slots | `rollback.c` | ≥ 2 × (8 + 3) + 1 |
| State ring | 16 slots | `rollback.c` | ≥ 8 + 1 |

---

## 2. The layers

```
match.c         one frame: receive → rollback → simulate → frame advantage → send
   |
session.c       packets <-> rollback module; ping, frame advantage, disconnect
   |
packet.c        InputPacket struct <-> bytes on the wire (big-endian)
   |
netsim.c        artificial lag / jitter / loss (pass-through when all 0)
   |
net.c           one non-blocking, dual-stack UDP socket, one peer
```

Each layer only knows the one below it. `net.c` moves bytes and knows nothing
about ticks. `packet.c` knows the format but not sockets. `netsim.c` delays
bytes without looking inside them. `session.c` is the only part of `src/net/`
that knows about rollback.

`match.c` has no raylib in it: `main.c` handles the window and keyboard and
calls `match_frame(input)` once per frame. That's what lets
`tests/netcode_test.c` run two real peers headless.

---

## 3. The socket (`src/net/net.c`)

Setup (`net_init` = `net_open` + `net_connect`):

1. **`socket(AF_INET6, SOCK_DGRAM, 0)`** with `IPV6_V6ONLY` turned off. One
   **dual-stack** socket handles both IPv6 and IPv4 (IPv4 `a.b.c.d` shows up
   as the mapped address `::ffff:a.b.c.d`). One code path for both.
2. **`O_NONBLOCK`**. `recv` returns `EAGAIN` right away when nothing has
   arrived, so the game loop never freezes waiting on the network.
3. **`bind` to `[::]:port`**. Accept on every local address.
4. **`getaddrinfo(..., AI_NUMERICHOST)`** parses the peer's address text
   without DNS. Link-local IPv6 (`fe80::1%en0`) needs the interface, which
   becomes the **scope id**.
5. **`connect`**. On UDP this **sends nothing**. It sets a default
   destination (so `send()` works) and makes the kernel **drop packets from
   anyone else**.

Errors that are deliberately ignored:

- **`ECONNREFUSED`**: an earlier packet got an ICMP "port unreachable" back
  (the peer isn't running yet). Since the socket is connected, the OS reports
  it on a later call. Ignored, because next frame's packet resends everything.
- **`ENOBUFS` / `EAGAIN` on send**: send buffer full. Ignored for the same
  reason.
- **`EINTR`**: interrupted by a signal, so retry.

A UDP datagram is delivered whole or not at all, never partially.

---

## 4. The packet format (`src/net/packet.c`)

One packet type, sent **once per frame in each direction**:

| Offset | Size | Field | Purpose |
|---|---|---|---|
| 0 | 4 | magic `"RBF2"` | Reject stray packets and old versions |
| 4 | 4 | ack | Newest tick we have **all** the peer's inputs for (cumulative) |
| 8 | 4 | sender_tick | The tick the sender will simulate next (for frame advantage) |
| 12 | 4 | hash_tick | Tick the hash is for, or -1 |
| 16 | 4 | hash | FNV-1a of that confirmed state (desync detection) |
| 20 | 4 | first_tick | Tick of `inputs[0]` |
| 24 | 1 | count | 0 to 32 |
| 25 | 2 × count | inputs | One `uint16` bitmask per tick |

Things to be able to explain:

- **Big-endian (network byte order), built with shifts** (`p[0] = v >> 24`).
  It works on any CPU, so no `htonl` is needed.
- **Never `send(&struct, sizeof struct)`**. Struct layout (padding, byte
  order) depends on the compiler and CPU. That's why encode and decode are
  separate from the struct.
- **Decode validates before it reads**: too short, wrong magic, count > 32,
  length ≠ 25 + 2 × count → `false`. Network data is untrusted, so a bad packet
  is dropped and logged, never `assert`ed.
- **Size**: 25 to 89 bytes. With 28 bytes of IP and UDP headers at 60 packets
  a second, that's at most ~7 KB/s each way.
- **Versioning**: changing the magic (`RBF1` → `RBF2`) makes old and new builds
  reject each other's packets instead of misreading them.

The LAN-discovery **hello** packet (12 bytes, magic `"RBH1"`) is separate; see
§11.

---

## 5. Reliability without TCP: redundancy + cumulative acks

This is the core networking idea of the project. UDP can **drop, duplicate
and reorder** packets. Instead of TCP-style timeouts and retransmit queues:

1. **Every packet carries all local inputs the peer hasn't acked yet**, as a
   contiguous range starting at the first unacked tick
   (`find_inputs_to_resend`). Inputs are 2 bytes each, so this costs almost
   nothing.
2. **Acks are cumulative**, like TCP: "I have all your inputs up to tick N."
   Because inputs always go out as contiguous ranges, one number is enough.
3. **The receiver only accepts the next tick it's missing**
   (`add_remote_input` accepts only `last_confirmed_remote_tick + 1`).
   Duplicates are ignored. Anything past a gap is also ignored, because it
   will come again in the next packet.
4. **Old or reordered packets are harmless**: their inputs are duplicates and
   their acks are older than the newest one (`receive_ack` ignores those).

So **a lost packet costs nothing extra**. The next packet, ~16 ms later,
carries the same inputs. There are no timers and no retransmit logic.

**Two directions, two trackers** (easy to mix up):

| Tracker | Meaning | Drives |
|---|---|---|
| `last_acked_local_tick` | Peer has all **my** inputs up to here | What I resend |
| `last_confirmed_remote_tick` | I have all **the peer's** inputs up to here | The ack I send; the stall rule |

They usually move together, but they differ under asymmetric loss or when
the peers are at different ticks.

**Send every frame, even when stalled.** `session_send` runs every frame,
even when no tick advanced. After a lost packet, this resend is the only way
the peer ever gets those inputs. If we only sent when a tick advanced, both
sides could stall waiting for each other forever (**deadlock**). An empty
packet (count 0) before the match is also how each side first "sees" the
other.

---

## 6. Rollback

### State

- **Input ring** (32 slots): per tick, both inputs, a `confirmed` flag per
  player, and `remote_predicted` (the sim used a guess for this tick).
- **State ring** (16 slots): a snapshot of `GameState` **before** each tick.
  The whole game is one plain struct (no pointers, no heap), so save and load
  are a single `memcpy`.
- Ring index = `((uint32_t)tick) & (size - 1)`. The cast avoids negative `%`
  results, and power-of-two sizes keep tick -1 in the last slot.
  `_Static_assert` checks the sizes at compile time. Each slot stores its
  tick, so readers can spot stale data.

### Prediction

If the remote input isn't confirmed, **repeat the last confirmed one**.
Inputs are held for many ticks, so this is usually right. The guess is
**written back into the ring**, so a later comparison is against what the sim
actually used. Wrong guesses happen at the moment an input *changes*, which
is why we also keep a little input delay.

### Misprediction and re-simulation

When a real remote input arrives for a tick that ran on a different guess,
`add_remote_input` records the **earliest** mispredicted tick M. Next frame,
`match.c`:

1. `load_state(M)`.
2. Re-runs `process_tick` for M … `next_tick - 1` with the corrected inputs.
   Later unconfirmed ticks are predicted again, since "last confirmed" may
   have changed.
3. `clear_rollback_point()`.

Normal ticks and re-simulation both go through `process_tick` (save → get
inputs → `sim` → `memcpy`), so they can't drift apart. Nothing is rendered
during re-simulation, so a rollback looks like a small jump.

### The stall rule

`can_advance(next_tick)` is false when
`next_tick - last_confirmed_remote_tick > MAX_ROLLBACK`. That one check covers
a slow peer, a paused peer, and lost or late packets. You can't see the peer's
actual tick, only the inputs you've received. While stalled, the game keeps
rendering, receiving and **resending**, and waits one `STEP` per frame
instead of spinning.

### Input delay and the startup ticks

Local input sampled when `next_tick` is N is written to tick N + 3. That
gives the peer 3 extra ticks (50 ms) to receive it before it's needed, so on
a link under ~50 ms one way **no prediction is needed at all**. Ticks 0–2 are
prefilled as "no buttons, confirmed" on both peers, and both trackers start
at `INPUT_DELAY - 1`.

### Worked example (D = 3, one-way latency ≈ 6 ticks)

| Time | Player A | Player B |
|---|---|---|
| tick 10 | Presses fire; recorded for tick 13 and sent | |
| tick 13 | Simulates 13 with fire: A starts shooting | Simulates 13 predicting "no fire" |
| tick 16 | | Gets A's tick-13 input. Mismatch → load state 13, re-sim 13–15, continue at 16. The shot appears 3 ticks into its startup. |

B never froze. B lost (latency − input delay) = 3 ticks of A's animation.

---

## 7. One frame (`match_frame`), in order

| # | Step | Why here |
|---|---|---|
| 1 | Sample keyboard (`IsKeyDown` only) | Once per frame |
| 2 | `netsim_update`, `session_receive` | **Before rollback**, so new remote inputs fix the past before new ticks are built on it |
| 3 | Rollback if `find_rollback_point()` ≠ -1 | Asserts depth ≤ 8 (the stall rule guarantees it) |
| 4 | Run every tick that's due: stall check → `write_local_input(next_tick + 3)` → `process_tick` → desync and sync checks | Fixed timestep: tick N is due at `start_time + N × STEP` |
| 5 | Frame advantage estimate, nudge if ahead | **After** simulating, so `next_tick` means the same as the peer's `sender_tick` |
| 6 | `session_send`, `netsim_update` | **Every frame**, even while stalled |
| 7 | Render, sleep until the next tick | One `STEP` if stalled or not started, otherwise the loop would spin |

**Starting the match (stand-in handshake):** nothing ticks until the first
valid packet from the peer arrives. Then `start_time = now`. Both sides send
every frame, so they start within about one one-way trip of each other, and
frame-advantage sync absorbs the difference.

---

## 8. Measuring the link (`session.c`)

### Ping (RTT), from acks, with no extra packet fields

1. `note_sent`: the **first** time a local input tick goes out, store the time
   in a 64-slot ring.
2. `note_ack`: the first time an ack covers that tick, `now - sent_time` is
   one RTT sample.
3. Exponential moving average: `rtt = 0.9 × rtt + 0.1 × sample`.

This includes up to a frame of waiting on each side, and spikes on loss
(the ack waits for the resend). That's fine: it's the delay the game actually
experiences.

### Frame advantage (time sync)

Whoever starts second is permanently behind by about the one-way latency. The
peer that's **ahead** predicts more, rolls back more and hits the stall limit
first. Same link, very different experience.

```
remote_now = peer_sender_tick + (rtt/2 + time_since_that_packet_arrived) / STEP
advantage  = next_tick - remote_now
```

- The "time since arrival" term stops a stale `sender_tick` from making us
  look further and further ahead when packets stop.
- **Correction:** if advantage > 1 tick, push `start_time` forward by
  `STEP / 10`. That skips one tick every ~10 frames until we're level. Only
  the peer that's ahead adjusts.
- It changes **when** ticks run, never **what** they compute, so it can't
  cause a desync.
- Measured (100 ms, 30 ms jitter, 10% loss): rollbacks went from 113 / 43
  (lopsided) to 92 / 91 (shared). The total stays about the same, because
  latency sets that. What changes is that both players share them.

### Disconnect

No valid packet for 3 s → red "Peer disconnected". It's display only: if
packets resume (a Wi-Fi blip), the game carries on.

---

## 9. Determinism and desync detection

Rollback only works if the same state + same inputs = the **same bytes** on
both machines. The sim therefore:

- uses **integer / fixed-point math** (subpixels), never floats,
- reads input **only from the input ring**, never the keyboard,
- keeps **all** state in `GameState`, with no static variables in the sim, no
  `rand()` and no OS clock,
- keeps padding bytes zero (`memset` in `sim_init`, `memcpy` instead of
  struct assignment), because the checks compare raw bytes.

Two checks:

| | Local desync check (`desync_check.c`) | Cross-machine sync check (`sync_check.c`) |
|---|---|---|
| Compares | Rollback's snapshot vs a second copy that only runs on confirmed inputs | My hash vs the peer's hash for the same tick |
| Catches | Rollback bugs, state hidden outside `GameState`, padding garbage | Machines computing different results from the same inputs |
| Method | `memcmp`, asserts on mismatch | FNV-1a hash of `GameState` in each packet, 64-slot ring, compared when both arrive |
| Shows | Assert + log | Overlay: "sync OK (tick N)" or red "DESYNC at tick N" |

A snapshot is **final** (safe to hash) once every input before it is
confirmed. After that it can never be rolled back. Both checks run **after
every tick**, not once per frame, so the snapshot is still in the 16-slot
ring.

A desync can't be repaired: rollback re-simulates but never sends real state.
A desync means a sim bug, not a network problem.

---

## 10. Network simulator (`src/net/netsim.c`)

Makes a good network behave like a bad one, for testing and the demo.

- Sits between `session.c` and `net.c`. With everything at 0 it's a
  pass-through.
- **Both directions** on the machine where it's enabled (half of `--lag` each
  way), so one laptop controls the whole link.
- **Jitter** varies each packet's delay by ±jitter/2. Delivery picks the
  earliest due packet, so jitter **reorders** packets, like a real network.
- **Loss** drops each packet with probability `loss%`, separately per
  direction.
- Two fixed 128-slot queues, no `malloc`. A full queue drops the packet, like
  an overloaded router.
- Its own **xorshift32** RNG. It must never share random state with the sim,
  or a dropped packet could change the game on one machine only.
- Delays are rounded up to the frame (~16 ms), since packets are only
  released when the loop checks.

Flags `--lag ms --jitter ms --loss pct`, or live keys 1/2, 3/4, 5/6, 0 to
reset.

---

## 11. LAN discovery (`src/net/discovery.c`)

`./build/game lan` finds the other machine without typing an IP.

**Why IPv6 multicast and not IPv4 broadcast:** the first version used IPv4
broadcast and failed on a T-Mobile phone hotspot, which is **IPv6-only**.
macOS fakes IPv4 there with a translator (**CLAT / 464XLAT**): both laptops
get `192.0.0.2`, and IPv4 traffic goes to the carrier, not the other laptop.
Discovery "heard" the translator (`192.0.0.1`) and connected to it. IPv6
**link-local** addresses (`fe80::`) exist on every IPv6 network (home LAN,
hotspot, direct cable), so one approach covers all of them.

- IPv6 has no broadcast. **Multicast** goes to a group, here `ff02::5242`.
  `ff02::` is link-local scope, so routers never forward it: LAN only, no
  server needed.
- `net_join_multicast` joins on every interface that's up, multicast-capable,
  and not loopback or a point-to-point tunnel (`utun` VPNs / iCloud Private
  Relay would swallow the hellos).
- The socket isn't `connect`ed yet, so it uses `recvfrom` to learn the
  peer's address (with scope, e.g. `[fe80::...%en0]`).

**Handshake** (both sides run the same code):

1. Pick a random nonzero 32-bit `id`.
2. Every 100 ms, send `hello{id, seen_id}` to the group on every interface,
   and directly to the candidate once there is one.
3. On receive:
   - Our own `id` → our own looped-back multicast, ignore.
   - First other `id` → that's the **candidate**. Stick with it and ignore
     everyone else.
   - Candidate's hello with `seen_id == my id` → both have heard each other,
     **done**.
   - An **input packet** from the candidate → it already finished (which it
     only does after hearing us), **done**. This covers the case where the
     final hellos were lost.
4. `connect` to the candidate. **Lower id = player 1.** Both sides know both
   ids, so they get opposite players without any further messages.

Limits: two machines needed (both use port 5000); two pairs searching on the
same LAN at once can get mixed up; networks that block client-to-client
traffic (campus Wi-Fi) block this too.

---

## 12. Delay mode (`--delay`) and the comparison

The only code difference is `may_simulate` in `match.c`: delay mode allows
**zero** guessed ticks (`tick <= last_confirmed_remote_tick`) instead of
`can_advance`'s 8. Same 3-tick input delay, so only one variable changes. It
never rolls back, and it stalls instead.

Why delay mode slows down under steady lag: my tick `t` needs the peer's
input for `t`, which it sent at its tick `t-3`, which needed my input from my
tick `t-6`. That's at most **6 ticks per round trip**, so at 200 ms RTT it
runs 30 ticks/s (half speed). Rollback's slack is
`INPUT_DELAY + MAX_ROLLBACK` = 11 each way, so 22 ticks per RTT, which is why
it only breaks down around 500 ms.

From `tests/smoothness_test.c` (worse of the two players):

| RTT | Rollback speed | Delay speed |
|---|---|---|
| ≤ 50 ms | 100% | 100% (identical: the input delay hides everything) |
| 100 ms | 100% | 81% |
| 200 ms | 100% | 43% |
| 200 ms + 30 jitter + 10% loss | 100% | 41% |
| 500 ms | 67% | 18% |

Rollback costs nothing when there's nothing to hide. It keeps a steady 50 ms
of input latency up to 200 ms RTT, in exchange for ~6–7 rollbacks a second.

---

## 13. Lobby and `--no-lobby`

The ready-up lobby lives **inside the sim** (`in_lobby`, `ready[]`,
`lobby_countdown_left` in `GameState`). Pressing fire toggles ready, which is
just an input, so rollback keeps the lobby in sync like the rest of the game.
It needs **no extra packet types**, and both peers start the match on the
same tick.

`--no-lobby` sets `skip_lobby` in the initial state, so **both peers must
pass it** (unlike `--delay`, which only changes timing). Nothing checks this:
a mismatch would show up as a DESYNC from the hash check.

---

## 14. Testing

```sh
ctest --test-dir build --output-on-failure
```

| Test | Checks |
|---|---|
| `packet` | Encode/decode round trips; bad packets rejected |
| `netcode` | Two real peers on localhost, scripted random input, 4 links (clean; 100 ms + 10%; 250 ms + 20%; delay mode). Fails on desync, assert, hang, hashes not compared, no rollbacks on a lossy link, or a spinning loop |
| `smoothness` | Rollback vs delay table above; fails unless rollback is clearly smoother from 100 ms |

---

## 15. Known gaps / honest limitations

- **No real start handshake.** Each peer starts when the first packet arrives;
  frame-advantage sync absorbs the error. A GGPO-style "host says start, waits
  RTT/2" plan is in `follow_ups.md`.
- **Catch-up after a long hitch** uses a wall-clock budget (`CATCHUP_MARGIN`)
  instead of capping ticks per frame.
- **No render interpolation**, so motion is slightly uneven on a 120 Hz
  display. Cosmetic only, it can't desync.
- **Peers don't verify matching settings** (`--no-lobby`, and the
  same build/struct layout). The hash check requires the same compiler and
  CPU type, which two Macs satisfy.
- **No security**: `connect` only filters by source address, which can be
  spoofed, and during LAN discovery any machine on the link can answer first.
  Nothing is authenticated or encrypted. Fine for a LAN demo.
- Doc drift: `networking.md` mentions `net_local_ip`, but the function is now
  `net_local_addr`. It skips macOS's `192.0.0.x` CLAT address and falls back
  to the IPv6 link-local address.

---

## 16. Questions worth being able to answer

- **Why UDP and not TCP?** TCP's in-order delivery means one lost packet
  blocks every later one (head-of-line blocking) until a retransmit after a
  timeout. We'd rather use the next packet, which already carries the same
  data. Old data is worthless here. Only the newest matters.
- **How do you handle packet loss?** Redundancy: every packet repeats all
  unacked inputs. Plus cumulative acks. No timers.
- **Reordering / duplicates?** Accept only the next missing tick; ignore old
  acks.
- **Why only send inputs?** Bandwidth (~7 KB/s max) and determinism make
  state unnecessary. The cost is that the sim must be perfectly deterministic.
- **What if a guess is wrong?** Load the saved state from before the wrong
  tick, re-simulate to the present.
- **Why have input delay at all with rollback?** It hides typical latency
  completely, so fewer and shallower rollbacks, at the cost of 50 ms of
  local lag.
- **What stops the peers drifting apart?** The stall rule (hard limit) plus
  frame-advantage sync (soft correction).
- **How would you know if the machines desynced?** FNV-1a hash of a final
  state in every packet, compared on the other side.
- **Why `connect()` on a UDP socket?** Default destination for `send`, and the
  kernel filters out packets from anyone else. It sends nothing.
- **Why big-endian and manual encoding?** Portable wire format, independent
  of struct padding and CPU byte order.
- **Why does the netsim have its own RNG?** Shared random state with the sim
  would let network events change the game on only one machine.
