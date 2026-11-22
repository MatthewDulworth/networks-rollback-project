# Networking

How bytes get between the two machines: the UDP socket, the packet format, the
glue between packets and the rollback module, the network simulator, and the
on-screen stats. For why the netcode works the way it does (redundant inputs,
cumulative acks), read `rollback_overview.md` first.

## Layers

```
main.c          receive_packets / send_packet: packets <-> rollback module
                ping (RTT) from acks, overlay, disconnect timeout
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
| 0 | 4 | magic | `0x52424631` ("RBF1"). Rejects stray packets and other versions. |
| 4 | 4 | ack | Newest tick we have **all** of the peer's inputs for. |
| 8 | 4 | first_tick | Tick of `inputs[0]`. |
| 12 | 1 | count | Number of inputs, 0 to 32. |
| 13 | 2 × count | inputs | One `uint16` bitmask per tick. |

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
- Size: 13 + 2 × count bytes, so at most 77. With 28 bytes of IP and UDP
  headers and 60 packets a second, that's at most about 6 KB/s each way.
- `tests/packet_test.c` checks round trips and rejection of bad packets. Run
  it with `ctest --test-dir build`.

## Packets ↔ rollback (`main.c`)

**`receive_packets(now)`** reads every waiting packet each frame:

1. Decode it. Drop it with a warning if it's malformed.
2. `receive_ack(packet.ack)`: the peer has our inputs up to that tick.
3. `add_remote_input` for every input in it. Most are duplicates (each packet
   repeats everything unacked), and the rollback module ignores those and
   anything past a gap, so no filtering is needed here.

**`send_packet(now)`** sends once per frame, **even when no tick advanced**:

- `ack = get_last_confirmed_remote_tick()`.
- Inputs = every local input the peer hasn't acked (`find_inputs_to_resend`).
  If that's more than 32, keep the oldest: the peer needs them in order.
- Sending while stalled matters. After a lost packet, this resend is the only
  way the peer ever gets those inputs. If we only sent when a tick advanced,
  both sides could end up waiting for each other forever.
- Sending while waiting for the peer matters too. An empty packet (count 0) is
  what lets the peer see us and start the match.

## Network simulator (`src/net/netsim.c`)

Makes a good network behave like a bad one, for testing and the demo.

- **Where it sits:** `main.c` calls `netsim_send`/`netsim_receive`, which call
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
  come. `main.c` calls it at the start of the frame (before receiving) and
  right after `send_packet` (so packets with no delay go out immediately).
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
| ping | Smoothed RTT, above. |
| rollbacks/s, max depth, stalls/s | Counted over one-second windows; the last finished window is shown so the numbers don't flicker. Depth is how many ticks were re-simulated (at most `MAX_ROLLBACK`, 8). A stall is a frame that hit the rollback limit and stopped advancing. |
| sim lag / jitter / loss (dropped) | Current netsim settings (red when any are on) and how many packets it has dropped. |
| me / peer | This machine's address and port, and the peer's. |

What to expect: on a clean connection with under ~50 ms of one-way delay,
rollbacks stay at 0, because the 3-tick input delay covers the latency and the
real input always arrives before it's needed. As lag goes up, rollbacks and
depth climb. Once depth is regularly 8, stalls start.

## Disconnect

`last_packet_time` updates whenever a valid packet arrives. After 3 seconds
with nothing, a red "Peer disconnected" message appears. It's display only:
if packets start arriving again (a short Wi-Fi drop), the game just carries
on. Until then the game is stalled at the rollback limit anyway.

## Running on two machines

```sh
./build/game 5000 <other_machine_ip> 5000 0     # machine A
./build/game 5000 <other_machine_ip> 5000 1     # machine B
```

- Both can use the same port, since they're different machines. Players must
  be opposite (0 and 1).
- The overlay shows each machine's own IP to read off.
- macOS asks whether to allow incoming connections the first time. Accept on
  both.
- Some networks (campus or guest Wi-Fi) block traffic between devices. A phone
  hotspot is a good fallback.
