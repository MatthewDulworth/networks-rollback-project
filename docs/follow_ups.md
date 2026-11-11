# Follow-ups

## Main loop: catch-up after falling behind

Right now, after a stall (breakpoint, window drag) the loop tries to run every missed tick, and the time budget (`CATCHUP_MARGIN`) never forgives the debt.

- Local standard (Fiedler, "Fix Your Timestep"): cap ticks per frame for small spikes, and cap total debt (~0.25 s) for big stalls by moving the time origin forward. A tick count is easier to reason about than a wall-clock budget.
- Discarding time doesn't discard ticks. Tick numbers stay contiguous and every tick gets a real input. Once an input for tick N has been sent, it can never change.
- With networking, the prediction threshold limits how far apart peers can get, so a local stall stalls both peers. After resuming you're only ~N ticks behind, and frame-advantage sync handles the rest: the peer that's ahead slows slightly (GGRS: ~10% per frame ahead, capped at 3).
- Open question: when catch-up runs several ticks in one frame, they all get the same sampled input. Check that quick taps still behave correctly (`previous_input` should cover press/release).

Refs: https://gafferongames.com/post/fix_your_timestep/, https://docs.rs/crate/ggrs/0.12.0/source/docs/time-synchronization.md
