#pragma once

// UDP connection to a single peer. This module only moves bytes: it knows
// nothing about ticks, inputs or acks. Packet encoding lives elsewhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// An IPv4 address and port, both in host byte order, so callers don't need
// the socket headers. ip 0xFFFFFFFF is the broadcast address (NET_BROADCAST).
typedef struct {
    uint32_t ip;
    uint16_t port;
} NetAddr;

#define NET_BROADCAST 0xFFFFFFFFu  // 255.255.255.255: everyone on this LAN

// Open a non-blocking UDP socket on local_port and remembers the peer's
// address (peer_ip is dotted IPv4, e.g. "127.0.0.1"). Returns false if any
// step fails (bad address, port already in use, ...).
// Same as net_open followed by net_connect.
bool net_init(uint16_t local_port, const char *peer_ip, uint16_t peer_port);

// The two halves of net_init, for LAN discovery, which has to talk to
// anyone (net_send_to / net_receive_from) before it knows who the peer is.
// net_open: non-blocking UDP socket bound to local_port, allowed to send
// broadcasts. net_connect: from now on net_send goes to peer, and packets
// from anyone else are dropped by the kernel.
bool net_open(uint16_t local_port);
bool net_connect(NetAddr peer);

// Send one packet to a given address (NET_BROADCAST for everyone on the
// LAN). For use before net_connect.
bool net_send_to(NetAddr to, const uint8_t *data, size_t len);

// Like net_receive, and also reports who sent the packet in *from (may be
// NULL). For use before net_connect.
int net_receive_from(uint8_t *buf, size_t capacity, NetAddr *from);

// Write addr as "a.b.c.d:port" into out.
void net_addr_to_string(NetAddr addr, char *out, size_t capacity);

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

// Write this machine's LAN IPv4 address (dotted, e.g. "192.168.1.5") into
// out, for showing on screen so the other player knows what to connect to.
// Picks the first interface that is up, isn't loopback and has an IPv4
// address. Writes "unknown" if there is none.
void net_local_ip(char *out, size_t capacity);
