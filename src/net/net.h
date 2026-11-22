#pragma once

// UDP connection to a single peer. This module only moves bytes: it knows
// nothing about ticks, inputs or acks. Packet encoding lives elsewhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Open a non-blocking UDP socket on local_port and remembers the peer's
// address (peer_ip is dotted IPv4, e.g. "127.0.0.1"). Returns false if any
// step fails (bad address, port already in use, ...).
bool net_init(uint16_t local_port, const char *peer_ip, uint16_t peer_port);

// Close the socket. Safe to call even if net_init failed.
void net_shutdown(void);

// Send one packet to the peer. UDP gives no delivery guarantee, so true only
// means the OS accepted it, not that it arrived.
bool net_send(const uint8_t *data, size_t len);

// Read one waiting packet into buf. Returns its length, 0 if nothing is
// waiting (never blocks), or -1 on a real error. Call in a loop until it
// returns 0 or less. A packet longer than capacity is cut off, so capacity
// should be at least the largest packet you ever send.
int net_receive(uint8_t *buf, size_t capacity);
