#pragma once

// LAN discovery: find the other player by UDP broadcast instead of typing
// their IP. Runs before the match, on a socket from net_open that isn't
// connected yet. Both machines must use the same port, so it only works with
// two machines (two copies on one machine can't both bind the port).

#include <stdbool.h>
#include <stdint.h>
#include "net/net.h"

// Broadcast hellos on port until a peer answers and both sides know they
// have heard each other. Blocks (Ctrl-C to give up). On return, *peer is the
// other machine's address and *player is PLAYER_1 or PLAYER_2, opposite to
// what the peer gets. Call net_connect(*peer) afterwards.
void discovery_run(uint16_t port, NetAddr *peer, int *player);
