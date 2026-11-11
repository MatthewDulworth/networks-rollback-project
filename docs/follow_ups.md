# Follow-ups

## Main loop: catch-up after falling behind

Right now, after a stall (breakpoint, window drag) the loop tries to run every missed tick, and the time budget (`CATCHUP_MARGIN`) never forgives the debt.

- Local standard (Fiedler, "Fix Your Timestep"): cap ticks per frame for small spikes, and cap total debt (~0.25 s) for big stalls by moving the time origin forward. A tick count is easier to reason about than a wall-clock budget.
- Discarding time doesn't discard ticks. Tick numbers stay contiguous and every tick gets a real input. Once an input for tick N has been sent, it can never change.
- With networking, the prediction threshold limits how far apart peers can get, so a local stall stalls both peers. After resuming you're only ~N ticks behind, and frame-advantage sync handles the rest: the peer that's ahead slows slightly (GGRS: ~10% per frame ahead, capped at 3).
- Open question: when catch-up runs several ticks in one frame, they all get the same sampled input. Check that quick taps still behave correctly (`previous_input` should cover press/release).

Refs: https://gafferongames.com/post/fix_your_timestep/, https://docs.rs/crate/ggrs/0.12.0/source/docs/time-synchronization.md

## Shared start time (tick 0)

Each peer's `GetTime()` is its own local clock, so "start at time T" first needs the clock offset. The NTP-style estimate assumes equal delay in both directions, and any asymmetry becomes error you can't measure.

- Precision isn't needed: frame-advantage sync is needed anyway for drift, and it absorbs a startup error within about a second.
- Plan (roughly GGPO): ping handshake to confirm the connection and measure RTT. The host sends "start", the guest starts on receipt, and the host waits ~RTT/2 then starts.
- Frame advantage: the peer is probably at about `last_received_tick + (RTT/2) / STEP`, and advantage = local tick minus that.
- In the loop, "start" just means setting the time origin to now. The same origin variable handles start, catch-up and slowing down.
- Open questions: what if the UDP "start" packet is lost? What do peers do with input before tick 0?

## Input: sample per frame, record per tick

Sample hardware once per frame, but record one input per tick. The sim only ever reads inputs from the buffer, never the keyboard, so rollback re-simulation replays exactly what was recorded.

- Catch-up ticks in the same frame all get that frame's sample. Frames that run zero ticks just drop their sample.
- `previous_input` for press/release detection should be the previous tick's buffered input, not the previous frame's sample.
- Local input delay: write the sample to `next_tick + DELAY` instead of `next_tick`.

## Input history as a fixed ring buffer

Use a fixed ring buffer, not the heap. History is bounded by MAX_ROLLBACK (~8–10) + input delay + margin, so 64–128 slots is plenty. A full match log for replays or desync debugging is a separate thing (file).

- Store the tick number in each slot to catch stale entries (the slot for tick N is also the slot for N + SIZE).
- Watch signed `%` on negative ticks: use a power-of-two size and mask an unsigned value.
- Remote slots need a confirmed/predicted flag to find the earliest misprediction.
- Saved `GameState` snapshots use the same ring idea with the same bound. They're bigger, so keep the window tight.

## Stalling at the rollback limit

Don't advance when `next_tick - last_confirmed_remote_tick > MAX_ROLLBACK`. You can't see the peer's actual tick, only their last confirmed input. That one check covers both a slow peer and lost/late packets.

- While stalled, keep polling input, rendering, receiving, and **resending unacked inputs every frame**. Sending only when a tick advances can deadlock after a lost packet.
- The hard stall is a fallback. Without frame-advantage time sync, a peer that's consistently slightly ahead keeps hitting the wall and stutters. Build the stall first, then add sync (see "Shared start time" above).
