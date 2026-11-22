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
