#include "net/net.h"

#include <arpa/inet.h>   // inet_pton, htons
#include <errno.h>
#include <fcntl.h>       // fcntl, O_NONBLOCK
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <stdio.h>       // perror, fprintf
#include <string.h>      // memset
#include <sys/socket.h>
#include <unistd.h>      // close

// The one socket this module owns. -1 means "not open".
static int sock = -1;

bool net_init(uint16_t local_port, const char *peer_ip, uint16_t peer_port) {
    // AF_INET = IPv4, SOCK_DGRAM = UDP (0 lets the OS pick the protocol,
    // which for an IPv4 datagram socket is UDP).
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("net: socket");
        return false;
    }

    // Non-blocking: recv returns immediately with EAGAIN when nothing has
    // arrived, instead of freezing the game loop until a packet shows up.
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("net: fcntl O_NONBLOCK");
        net_shutdown();
        return false;
    }

    // Bind our side: receive packets sent to local_port on any of this
    // machine's addresses (localhost, Wi-Fi, ...).
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(local_port);
    if (bind(sock, (struct sockaddr *)&local, sizeof local) < 0) {
        perror("net: bind");  // EADDRINUSE: another copy already has this port
        net_shutdown();
        return false;
    }

    // Peer address: text "a.b.c.d" -> 32-bit address in network byte order.
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof peer);
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip, &peer.sin_addr) != 1) {
        fprintf(stderr, "net: invalid peer address '%s'\n", peer_ip);
        net_shutdown();
        return false;
    }

    // connect() on UDP sends nothing. It sets the default destination (so
    // plain send() works) and makes the kernel drop packets from any other
    // address, so recv() only ever returns the peer's packets.
    if (connect(sock, (struct sockaddr *)&peer, sizeof peer) < 0) {
        perror("net: connect");
        net_shutdown();
        return false;
    }

    return true;
}

void net_shutdown(void) {
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
}

bool net_send(const uint8_t *data, size_t len) {
    ssize_t sent = send(sock, data, len, 0);
    if (sent < 0) {
        // ECONNREFUSED: an earlier packet got an ICMP "port unreachable"
        // back (peer not running yet). ENOBUFS / EAGAIN: OS send buffer
        // full. Either way, nothing to do: next frame's packet resends
        // every unacked input anyway.
        if (errno != ECONNREFUSED && errno != ENOBUFS && errno != EAGAIN) {
            perror("net: send");
        }
        return false;
    }
    // A UDP datagram is sent whole or not at all, never partially.
    return (size_t)sent == len;
}

int net_receive(uint8_t *buf, size_t capacity) {
    for (;;) {
        ssize_t received = recv(sock, buf, capacity, 0);
        if (received >= 0) {
            return (int)received;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // nothing waiting
        }
        if (errno == ECONNREFUSED) {
            // Reported for an earlier send (peer's port was closed), not
            // for a packet. The error is cleared once read, so try again:
            // a real packet may be queued behind it.
            continue;
        }
        if (errno == EINTR) {
            continue;  // interrupted by a signal before anything arrived
        }
        perror("net: recv");
        return -1;
    }
}
