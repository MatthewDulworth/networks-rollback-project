#pragma once

// UDP connection to a single peer. This module only moves bytes: it knows
// nothing about ticks, inputs or acks. Packet encoding lives elsewhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The socket is IPv6, with IPv4 allowed too ("dual-stack"): an IPv4 address
// a.b.c.d is carried as the IPv4-mapped IPv6 address ::ffff:a.b.c.d, so one
// code path handles both.

// An IPv6 address (16 bytes, network byte order), port (host byte order) and
// scope: the interface index a link-local (fe80::) or link-local multicast
// (ff02::) address belongs to, 0 for other addresses. Callers don't need
// the socket headers.
typedef struct {
    uint8_t ip[16];
    uint16_t port;
    uint32_t scope_id;
} NetAddr;

// Open a non-blocking UDP socket on local_port and remembers the peer's
// address. peer_ip is IPv4 ("192.168.1.5") or IPv6 ("2607:fb91::1", or
// "fe80::1%en0" for link-local, which needs the interface). Returns false if
// any step fails (bad address, port already in use, ...).
// Same as net_open followed by net_connect.
bool net_init(uint16_t local_port, const char *peer_ip, uint16_t peer_port);

// The two halves of net_init, for LAN discovery, which has to talk to
// anyone (net_send_to / net_receive_from) before it knows who the peer is.
// net_open: non-blocking dual-stack UDP socket bound to local_port.
// net_connect: from now on net_send goes to peer, and packets from anyone
// else are dropped by the kernel.
bool net_open(uint16_t local_port);
bool net_connect(NetAddr peer);

// Join the link-local multicast group (an ff02:: address, e.g. "ff02::5242")
// on every interface that can reach the LAN: up, multicast-capable, not
// loopback and not a point-to-point tunnel (VPNs). Writes one destination
// per interface (group, that interface's scope, port) into dests, up to
// max, and returns how many. A packet sent to one of them reaches every
// machine on that interface's link that has joined the group. Call after
// net_open.
int net_join_multicast(const char *group, uint16_t port, NetAddr *dests, int max);

// Send one packet to a given address. For use before net_connect.
bool net_send_to(NetAddr to, const uint8_t *data, size_t len);

// Like net_receive, and also reports who sent the packet in *from (may be
// NULL). For use before net_connect.
int net_receive_from(uint8_t *buf, size_t capacity, NetAddr *from);

// Same address and port (and scope, for link-local).
bool net_addr_equal(NetAddr a, NetAddr b);

// Write addr into out: "a.b.c.d:port" for IPv4, "[ipv6%interface]:port"
// for IPv6.
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

// Write this machine's LAN address and port into out (e.g.
// "192.168.1.5:5000"), for showing on screen so the other player knows what
// to connect to. Prefers IPv4 on an interface that can reach the LAN,
// skipping the 192.0.0.x address macOS invents on IPv6-only networks.
// Otherwise the IPv6 link-local address ("[fe80::1%en0]:5000"). Writes
// "unknown" if there is neither.
void net_local_addr(uint16_t port, char *out, size_t capacity);
