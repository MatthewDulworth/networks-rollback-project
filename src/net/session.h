#pragma once

// The connection to the peer, as the game sees it: turns received packets
// into rollback inputs, acks and hashes, builds the packet we send each frame,
// and keeps track of ping and whether the peer is there.
//
// Sits on top of the rest of src/net (packet.c, netsim.c, net.c) and is the
// only part of it that knows about ticks, rollback or the sync check.

#include <stdbool.h>
#include "sim/sim.h"

// Reset everything. Call once before the first session_receive.
void session_init(void);

// Drain every waiting packet into the rollback module and the sync check.
// Call once per frame, after netsim_update and before rollback.
void session_receive(double now);

// Send our ack, newest state hash and every local input the peer hasn't
// acked. Call once per frame, even when no tick advanced: after a lost
// packet this resend is the only way the peer ever gets those inputs.
void session_send(double now, Tick next_tick);

// True once any valid packet from the peer has arrived.
bool session_peer_seen(void);

// No packet for DISCONNECT_TIMEOUT seconds after the peer was seen.
bool session_disconnected(double now);

// Smoothed round-trip time in milliseconds, or -1 before the first sample.
double session_rtt_ms(void);

// How many ticks we're ahead of the peer right now (negative: behind),
// estimated from the peer's newest sender_tick, half the RTT and how long ago
// that packet arrived. Returns 0 until there's an estimate (no RTT sample
// yet, or the peer hasn't started ticking).
//
// Call it after this frame's simulate loop, with the next_tick we're about to
// send. sender_tick is the peer's next_tick at that same point in its frame
// (after simulating), so both sides compare like with like. Called before
// simulating, next_tick is one tick behind that and every estimate comes
// out about 1 tick too low.
double session_frame_advantage(double now, Tick next_tick);
