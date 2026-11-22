# Networking

How bytes get between the two machines: the UDP socket, the packet format, the
glue between packets and the rollback module, the network simulator, and the
on-screen stats. For why the netcode works the way it does (redundant inputs,
cumulative acks), read `rollback_overview.md` first.

## Layers

```
session.c       session_receive / session_send: packets <-> rollback module
                and sync check, ping (RTT) from acks, disconnect timeout
   |
packet.c        InputPacket struct <-> bytes on the wire
   |
netsim.c        artificial lag, jitter, loss (pass-through when all 0)
   |
net.c           one non-blocking UDP socket, one peer
```

Each layer only knows about the one below it. `net.c` moves bytes and knows
nothing about ticks. `packet.c` knows the format but nothing about sockets.
`netsim.c` delays bytes without looking inside them.

## The socket (`src/net/net.c`)

`net_init(local_port, peer_ip, peer_port)`:

1. `socket(AF_INET, SOCK_DGRAM, 0)`: an IPv4 UDP socket.
2. `fcntl(..., O_NONBLOCK)`: `recv` returns immediately with `EAGAIN` when
   nothing has arrived, instead of freezing the game loop.
3. `bind` to `INADDR_ANY:local_port`: accept packets sent to this port on any
   of the machine's addresses (localhost, Wi-Fi, Ethernet).
4. `inet_pton` turns the peer's `"a.b.c.d"` text into a 32-bit address.
5. `connect`: on UDP this **sends nothing**. It sets the default destination,
   so plain `send()` works, and makes the kernel drop packets from any other
   address, so `recv()` only ever returns the peer's packets.

Errors worth knowing:

- **`ECONNREFUSED`**: an earlier packet got an ICMP "port unreachable" back,
  usually because the peer isn't running yet. Because the socket is
  connected, the OS reports it on a later `send` or `recv`. Both functions
  ignore it: the next frame's packet resends everything anyway, and in `recv`
  a real packet may be queued behind the error.
- **`ENOBUFS` / `EAGAIN` on send**: the OS send buffer is full. Ignored for the
  same reason.
- **`EINTR`**: a signal interrupted the call. Retry.
- A UDP datagram is sent whole or not at all, never partially.

`net_local_ip` walks `getifaddrs()` and returns the first IPv4 address on an
interface that's up and isn't loopback. It's shown on the overlay so players
can read off the address to type in. If a VPN is running it may pick the
VPN's address instead of the Wi-Fi one.

## The packet format (`src/net/packet.c`)

One packet type, sent once per frame in each direction:

| Offset | Size | Field | |
|---|---|---|---|
| 0 | 4 | magic | `0x52424632` ("RBF2"). Rejects stray packets and other versions. |
| 4 | 4 | ack | Newest tick we have **all** of the peer's inputs for. |
| 8 | 4 | sender_tick | The tick the sender simulates next. For frame advantage sync. |
| 12 | 4 | hash_tick | Tick of the confirmed state `hash` is for, or `NO_TICK` (-1). |
| 16 | 4 | hash | FNV-1a hash of that state. See "Sync check" below. |
| 20 | 4 | first_tick | Tick of `inputs[0]`. |
| 24 | 1 | count | Number of inputs, 0 to 32. |
| 25 | 2 × count | inputs | One `uint16` bitmask per tick. |

- **Big-endian** (network byte order), built from shifts (`p[0] = v >> 24`,
  ...). The result doesn't depend on the CPU's own byte order, so the code
  doesn't need `htonl`.
- **Never send a struct as-is.** Its layout in memory (padding, byte order)
  depends on the compiler and CPU. That's why there's a separate encode and
  decode instead of `send(&packet, sizeof packet)`.
- Ticks are signed (`NO_TICK` is -1). Converting to `uint32_t` wraps mod 2³²
  and decode converts back.
- **Decode validates before reading.** Too short, wrong magic, count over 32,
  or a length that doesn't match the count all return `false`. Each check
  makes the next read safe. This is data from the network, so a bad packet is
  dropped and logged, never asserted.
- Size: 25 + 2 × count bytes, so at most 89. With 28 bytes of IP and UDP
  headers and 60 packets a second, that's at most about 7 KB/s each way.
- Format 1 (`RBF1`) had no tick or hash fields. Changing the magic means an
  old build and a new build reject each other's packets instead of misreading
  them.
- `tests/packet_test.c` checks round trips and rejection of bad packets. Run
  it with `ctest --test-dir build`. `tests/netcode_test.c` tests the whole
  netcode end to end (see `DEVELOPMENT.md`).

## Packets ↔ rollback (`src/net/session.c`)

**`session_receive(now)`** reads every waiting packet each frame:

1. Decode it. Drop it with a warning if it's malformed.
2. `receive_ack(packet.ack)`: the peer has our inputs up to that tick.
3. `add_remote_input` for every input in it. Most are duplicates (each packet
   repeats everything unacked), and the rollback module ignores those and
   anything past a gap, so no filtering is needed here.
4. `sync_check_add_remote(hash_tick, hash)`.
5. Remember the highest `sender_tick` seen and when it arrived, for frame
   advantage (below).

**`session_send(now, next_tick)`** sends once per frame, **even when no tick advanced**:

- `ack = get_last_confirmed_remote_tick()`.
- `sender_tick = next_tick`, and our newest confirmed-state hash.
- Inputs = every local input the peer hasn't acked (`find_inputs_to_resend`).
  If that's more than 32, keep the oldest: the peer needs them in order.
- Sending while stalled matters. After a lost packet, this resend is the only
  way the peer ever gets those inputs. If we only sent when a tick advanced,
  both sides could end up waiting for each other forever.
- Sending while waiting for the peer matters too. An empty packet (count 0) is
  what lets the peer see us and start the match.

## Network simulator (`src/net/netsim.c`)

Makes a good network behave like a bad one, for testing and the demo.

- **Where it sits:** `session.c` calls `netsim_send`/`netsim_receive`, which call
  `net_send`/`net_receive`. With all settings at 0 it's a pass-through.
- **Both directions:** packets we send wait before going out, and packets we
  receive are read off the socket right away but wait before the game sees
  them. Each direction gets half of `lag`, so `--lag 100` adds about 100 ms of
  round trip. One machine's settings control the whole link, and the other
  machine can leave everything at 0.
- **Jitter:** each packet's delay varies by up to ±jitter/2. Because delivery
  picks the due packet with the earliest release time, jitter **reorders**
  packets, like a real network. That exercises the out-of-order handling in
  `add_remote_input` and `receive_ack`.
- **Loss:** each packet is dropped with probability `loss%`, separately in
  each direction.
- **Queues:** two fixed arrays of 128 slots, no `malloc`. At ~60 packets a
  second that's about 2 s of one-way delay, more than the 1000 ms lag cap.
  If a queue is ever full the packet is dropped, which is what an overloaded
  router does too.
- **`netsim_update(now)`** moves packets along: it reads everything from the
  socket into the incoming queue, and sends outgoing packets whose time has
  come. `match.c` calls it at the start of the frame (before receiving) and
  right after `session_send` (so packets with no delay go out immediately).
- **Precision:** packets are only released when the loop checks, about every
  16 ms, so delays are rounded up to the frame. Fine at demo-level lag.
- **Random numbers:** a private xorshift32 generator, not `rand()`. The sim
  must never share random state with anything. If it did, a dropped packet
  here could change the game on one machine only.

**Controls:** `--lag ms`, `--jitter ms`, `--loss pct` after the four normal
arguments, or live with number keys: 1/2 lag −/+20 ms, 3/4 jitter −/+10 ms,
5/6 loss −/+5 %, 0 resets everything. Number keys instead of F-keys because on
a Mac laptop F-keys need fn held down.

The keys use `key_went_down`, a "just pressed" check built from `IsKeyDown`.
raylib's own `IsKeyPressed` doesn't work here, because the loop polls input
twice per frame (see `main_loop.md`).

## Ping (RTT)

Measured from acks, with no extra packet fields:

1. `note_sent`: when a local input is sent for the **first** time, record the
   time in a 64-slot ring keyed by tick.
2. `note_ack`: when an ack first covers a tick, `now - send time` is one
   round-trip sample.
3. Smoothed with an exponential moving average (each sample moves the
   estimate 10% of the way), so the number on screen is readable.

It measures more than the raw network delay:

- The peer only acks on its next send (end of its frame), and we only read
  the ack at the start of our next frame, so up to a frame on each side is
  included.
- A lost packet shows up as a spike, because the ack waits for the resend.

So it's the delay the game actually experiences, which is what matters here.

## Overlay

Top left of the window:

| Line | Meaning |
|---|---|
| ping, advantage | Smoothed RTT, above. Frame advantage in ticks, below: positive means this machine is ahead. |
| rollbacks/s, max depth, stalls/s (or DELAY MODE, stalls/s) | Counted over one-second windows; the last finished window is shown so the numbers don't flicker. Depth is how many ticks were re-simulated (at most `MAX_ROLLBACK`, 8). A stall is a frame that hit the rollback limit and stopped advancing. |
| sim lag / jitter / loss (dropped) | Current netsim settings (red when any are on) and how many packets it has dropped. |
| me / peer | This machine's address and port, and the peer's. |

What to expect: on a clean connection with under ~50 ms of one-way delay,
rollbacks stay at 0, because the 3-tick input delay covers the latency and the
real input always arrives before it's needed. As lag goes up, rollbacks and
depth climb. Once depth is regularly 8, stalls start.

## Sync check (`src/rollback/sync_check.c`)

The local desync check (`src/rollback/desync_check.c`) only proves that rollback matches a
replay on the **same** machine. The sync check proves the **two** machines
agree with each other.

- **Which state.** Snapshot `t` (the state before tick `t`) is final once
  every input before it is confirmed (`t - 1 <= last_confirmed_remote_tick`)
  and it has been saved (`t < next_tick`). It can never be rolled back after
  that, so both machines must have the exact same bytes for it.
  `sync_check_after_tick` hashes each snapshot once it becomes final. It runs
  after every tick, like the desync check, so any rollback for this frame's
  inputs has already happened and the snapshot is still in the state ring.
- **Hash.** FNV-1a over the raw bytes of `GameState`: XOR in each byte, then
  multiply by a prime. Raw bytes include padding, so padding must be zero
  everywhere: `sim_init` uses `memset` and the sim copies states with
  `memcpy`. Both machines also need the same struct layout (same compiler
  and CPU type; two Macs are fine).
- **Exchange.** Each packet carries our newest hash. Received hashes and our
  own go into a 64-slot ring keyed by tick, and whichever of the two arrives
  second triggers the comparison. That works whether the peer is ahead of us
  or behind. A hash that's lost or falls out of the ring just means that tick
  isn't compared.
- **Overlay.** "sync --" until the first comparison, green "sync OK (tick N)"
  while everything matches, red "DESYNC at tick N" for good after the first
  mismatch.

## Frame advantage

Each peer starts its clock when the other's first packet arrives, so whichever
starts second is behind by about the one-way latency, for good. The peer that's
ahead keeps simulating ticks whose remote input hasn't arrived yet: it predicts
more, rolls back more, and hits the stall limit first. The one behind barely
rolls back. Same link, very different experience.

**Estimate** (`session_frame_advantage`): the peer's newest `sender_tick` is
where it was when it sent. Since then the packet spent about half the RTT in
flight and has been sitting here since it arrived, and the peer kept ticking:

```
remote_now = sender_tick + (rtt/2 + (now - arrival_time)) / STEP
advantage  = next_tick - remote_now
```

- The arrival-time term matters when packets stop: without it a stale
  `sender_tick` would make us look further and further ahead.
- It's called **after** the simulate loop. `sender_tick` is the peer's
  `next_tick` after it simulated, so ours has to be too. Called before
  simulating, every estimate came out about 1 tick too low on both sides.
- Check: the two machines' values should add up to about 0. Measured, they
  add up to about -0.4 (small leftover frame-timing effects).
- Returns 0 until there's an estimate: no RTT yet, or the peer's
  `sender_tick` is still 0 because it hasn't started.

**Correction** (`match.c`): if the advantage is over 1 tick, push `start_time`
forward by `STEP / 10`. Every later tick is due a little later, so the peer
that's ahead skips one tick every 10 frames until it's level. Only the peer
that's ahead adjusts. Changing `start_time` only changes when ticks run, never
what they compute, so it can't cause a desync.

Measured on a 100 ms / 30 ms jitter / 10% loss link (headless, 15 s):

| | Avg advantage (A / B) | Rollbacks (A / B) |
|---|---|---|
| Off | +0.70 / -1.20 | 113 / 43 |
| On | -0.24 / -0.22 | 92 / 91 |

The total number of rollbacks doesn't drop, since that's set by the latency.
What changes is that they're shared evenly instead of one player taking most of
them. Constants were picked by measuring:
- Threshold 0.5 doubled the nudges with no better balance.
- Threshold 2 barely fired.
- A nudge of `STEP / 2` balanced just as well, but runs at visibly half speed
  while correcting.

## Delay mode (`--delay`)

The old way of doing netcode, kept for comparison. Never predict: a tick only
runs once the peer's input for it has arrived. The only code difference is
`may_simulate` in `match.c`, which asks for zero guessed ticks
(`tick <= last_confirmed_remote_tick`) instead of `can_advance`'s up to
`MAX_ROLLBACK`. Nothing is ever mispredicted, so nothing ever rolls back, and
when input is late the game stalls instead.

- Same `INPUT_DELAY` (3 ticks) as rollback, so the comparison only changes
  one thing. Delay mode plays smoothly as long as the peer's input arrives
  within those 3 ticks (about 50 ms one way, minus jitter). After that it
  stalls.
- `--lag` is the round trip, so `--lag 100` is 50 ms each way.
- Under steady lag it plays slower than real time: my tick `t` needs the
  peer's input for `t`, which the peer sent at its tick `t - 3`, which needed my
  input from my tick `t - 6`. So at most 6 ticks per round trip, which works
  out to 30 ticks/s (half speed) at a 200 ms RTT.
- It only changes *when* ticks run, never what they compute, so it can't
  desync and the hash check still applies. The two peers don't even have to
  use the same mode.
- The overlay shows "DELAY MODE" and stalls/s, and the window title says so.

`tests/smoothness_test.c` measures the difference. Each row is the worse
of the two players. "Speed" is game ticks per real second as a share of 60,
"frozen" is frames where no tick ran, and latency runs from sampling the
keyboard until that input shows on screen. One run:

| RTT | Mode | Speed | Frozen | Longest freeze | Input latency avg / max |
|---|---|---|---|---|---|
| clean, 50 ms | both | 100% | 0% | 18 ms | 50 / 52 ms |
| 100 ms | rollback | 100% | 0% | 19 ms | 50 / 53 ms |
| | delay | 81% | 14-53% | 38-89 ms | 62 / 74 ms |
| 200 ms | rollback | 100% | 0% | 19 ms | 50 / 53 ms |
| | delay | 43% | 57% | 106 ms | 115 / 159 ms |
| 200 ms, 30 jitter, 10% loss | rollback | 100% | 0% | 19 ms | 50 / 54 ms |
| | delay | 41% | 82% | 248 ms | 121 / 248 ms |
| 500 ms | rollback | 67% | 29-44% | 107 ms | 75 / 147 ms |
| | delay | 18% | 81% | 250 ms | 275 / 290 ms |

- Up to 50 ms RTT, the 3-tick input delay hides the whole trip, and the two
  modes are identical. Rollback costs nothing when there's nothing to hide.
- From 100 ms, delay mode slows down and freezes while rollback stays at full
  speed with a steady 50 ms of input latency. The price is about 6-7
  rollbacks a second, which delay mode never needs.
- At 500 ms, rollback is past its own limit too. Then
  `INPUT_DELAY + MAX_ROLLBACK` = 11 ticks of slack each way gives at most 22
  ticks per round trip (73%), so it stalls as well, but much less.
- Delay mode breaks down at 100 ms RTT, before the theoretical 50 ms one
  way. Netsim rounds delays up to whole frames, and a packet sent at the end
  of one frame is only read at the start of a later one, so the real trip is a
  frame or two longer than the setting.

```sh
scripts/player1.sh <other_machine_ip> --lag 100 --delay
```

Restart both games to switch modes.

## Disconnect

`last_packet_time` updates whenever a valid packet arrives. After 3 seconds
with nothing, a red "Peer disconnected" message appears. It's display only:
if packets start arriving again (a short Wi-Fi drop), the game just carries
on. Until then the game is stalled at the rollback limit anyway.

## Running on two machines

```sh
scripts/player1.sh <other_machine_ip>     # machine A
scripts/player2.sh <other_machine_ip>     # machine B
```

These build, then run `./build/game 5000 <other_machine_ip> 5000 <0 or 1>`.
Extra flags (`--lag`, `--jitter`, `--loss`, `--delay`) are passed through.

- Both can use the same port, since they're different machines. Players must
  be opposite (0 and 1).
- To swap addresses, run the script with no arguments first: it prints this
  machine's IP (the interface the default route uses, so Wi-Fi, hotspot or
  Ethernet). The overlay also shows it, as a check once the game is running.
- macOS asks whether to allow incoming connections the first time. Accept on
  both.
- Some networks (campus or guest Wi-Fi) block traffic between devices. A phone
  hotspot is a good fallback.
