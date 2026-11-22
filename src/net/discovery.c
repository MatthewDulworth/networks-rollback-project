#include "net/discovery.h"

#include <stdio.h>
#include <stdlib.h>  // arc4random
#include <time.h>    // nanosleep
#include "net/packet.h"
#include "sim/sim.h"  // PLAYER_1, PLAYER_2

// How often to send a hello, and so how often to check for replies.
#define HELLO_INTERVAL_NS 100000000L  // 100 ms

// The link-local multicast group hellos go to. IPv6 has no broadcast:
// instead a packet goes to a group, and reaches the machines on the link
// that joined it. ff02:: = link-local scope (routers never forward it, like
// a broadcast). The rest is an arbitrary number of our own, so other
// programs' multicast traffic doesn't reach us.
#define LAN_GROUP "ff02::5242"

// One destination per network interface (Wi-Fi, cable, ...).
#define MAX_INTERFACES 16

static void send_hello(NetAddr to, uint32_t my_id, uint32_t seen_id) {
    HelloPacket hello = { .id = my_id, .seen_id = seen_id };
    uint8_t buf[HELLO_SIZE];
    size_t len = hello_encode(&hello, buf, sizeof buf);
    net_send_to(to, buf, len);
}

void discovery_run(uint16_t port, NetAddr *peer, int *player) {
    // A random id tells our own hellos (multicast loops back to us) apart
    // from the peer's, and decides who is player 1. 0 means "nobody" in
    // seen_id, so don't pick it.
    uint32_t my_id;
    do {
        my_id = arc4random();
    } while (my_id == 0);

    // The first machine we hear from. We stick with it, and ignore anyone
    // else, so a third machine can't switch us mid-handshake.
    bool have_candidate = false;
    NetAddr candidate = {0};
    uint32_t candidate_id = 0;

    fprintf(stderr, "discovery: looking for the other player on port %u"
                    " (Ctrl-C to give up)\n", port);

    NetAddr group[MAX_INTERFACES];
    int group_count = net_join_multicast(LAN_GROUP, port, group, MAX_INTERFACES);
    if (group_count == 0) {
        fprintf(stderr, "discovery: no network interface to search on\n");
    }

    bool done = false;
    while (!done) {
        // Hello to the group on every interface, and directly to the
        // candidate once we have one. seen_id tells the candidate we've
        // heard it. Sent every interval, so a lost hello is just replaced
        // by the next one.
        uint32_t seen_id = have_candidate ? candidate_id : 0;
        for (int i = 0; i < group_count; i++) {
            send_hello(group[i], my_id, seen_id);
        }
        if (have_candidate) {
            send_hello(candidate, my_id, seen_id);
        }

        uint8_t buf[PACKET_MAX_SIZE];
        NetAddr from;
        int len;
        while (!done && (len = net_receive_from(buf, sizeof buf, &from)) > 0) {
            HelloPacket hello;
            if (hello_decode(buf, (size_t)len, &hello)) {
                if (hello.id == my_id) {
                    continue;  // our own hello, looped back
                }
                if (!have_candidate) {
                    have_candidate = true;
                    candidate = from;
                    candidate_id = hello.id;
                    char text[80];
                    net_addr_to_string(from, text, sizeof text);
                    fprintf(stderr, "discovery: heard %s\n", text);
                } else if (hello.id != candidate_id) {
                    continue;  // someone else on the LAN
                }
                // The candidate has heard us too, so both sides know each
                // other's address.
                if (hello.seen_id == my_id) {
                    done = true;
                }
                continue;
            }
            // Not a hello. If it's an input packet from the candidate, the
            // candidate already finished and started its match. That
            // happens when our hellos with seen_id reached it but its
            // replies to them were lost: it stops sending hellos, so its
            // input packets are the only sign left that it heard us.
            InputPacket input;
            if (have_candidate && net_addr_equal(from, candidate)
                && packet_decode(buf, (size_t)len, &input)) {
                done = true;
            }
        }

        if (!done) {
            struct timespec wait = { .tv_sec = 0, .tv_nsec = HELLO_INTERVAL_NS };
            nanosleep(&wait, NULL);
        }
    }

    // Both sides know both ids, so both compute the same answer and get
    // opposite players. (Equal ids can't happen: we'd have ignored the
    // peer's hellos as our own and never finished.)
    *peer = candidate;
    *player = my_id < candidate_id ? PLAYER_1 : PLAYER_2;

    char text[80];
    net_addr_to_string(candidate, text, sizeof text);
    fprintf(stderr, "discovery: matched with %s, we are player %d\n",
            text, *player + 1);
}
