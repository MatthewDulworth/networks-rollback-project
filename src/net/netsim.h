#pragma once

// Network condition simulator: a layer between the game and net.h that adds
// artificial latency, jitter and packet loss, so bad connections can be tested
// (and demoed) on a good network.
//
// It acts on both directions on this machine: packets we send wait before
// going out, and packets we receive wait before the game sees them. So one
// machine's settings control the whole link, and the other machine can run
// with everything at 0.
//
// With all settings at 0 it's a pass-through.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int lag_ms;     // Added round-trip time. Half is added in each direction.
    int jitter_ms;  // Each packet's delay varies randomly by up to +-jitter/2.
    int loss_pct;   // Chance (0-100) that a packet is dropped, per direction.
} NetsimSettings;

// Start with everything off. Call once, after net_init.
void netsim_init(void);

// Change settings. Values are clamped to sensible ranges. Packets already
// waiting keep the delay they were given.
void netsim_set(NetsimSettings settings);
NetsimSettings netsim_get(void);

// Use these instead of net_send / net_receive. Same meaning, but packets pass
// through the delay queues.
void netsim_send(const uint8_t *data, size_t len);
int netsim_receive(uint8_t *buf, size_t capacity);

// Move packets along: read everything the socket has into the incoming queue,
// and actually send outgoing packets whose delay is over. `now` is the time in
// seconds (any clock, as long as it's always the same one).
//
// Call before netsim_receive, and after netsim_send so packets with no delay
// go out in the same frame.
void netsim_update(double now);

// Packets dropped on purpose so far (both directions), for the overlay.
int netsim_dropped_count(void);
