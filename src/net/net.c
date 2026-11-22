#include "net/net.h"

#include <arpa/inet.h>   // inet_pton, htons
#include <errno.h>
#include <fcntl.h>       // fcntl, O_NONBLOCK
#include <ifaddrs.h>     // getifaddrs
#include <net/if.h>      // IFF_UP, IFF_LOOPBACK
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <stdio.h>       // perror, fprintf
#include <string.h>      // memset
#include <sys/socket.h>
#include <unistd.h>      // close

// The one socket this module owns. -1 means "not open".
static int sock = -1;

bool net_init(uint16_t local_port, const char *peer_ip, uint16_t peer_port) {
    if (!net_open(local_port)) {
        return false;
    }

    // Peer address: text "a.b.c.d" -> 32-bit address in network byte order.
    struct in_addr ip;
    if (inet_pton(AF_INET, peer_ip, &ip) != 1) {
        fprintf(stderr, "net: invalid peer address '%s'\n", peer_ip);
        net_shutdown();
        return false;
    }
    NetAddr peer = { .ip = ntohl(ip.s_addr), .port = peer_port };
    return net_connect(peer);
}

// NetAddr (host byte order) -> sockaddr_in (network byte order).
static struct sockaddr_in to_sockaddr(NetAddr addr) {
    struct sockaddr_in out;
    memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(addr.ip);
    out.sin_port = htons(addr.port);
    return out;
}

bool net_open(uint16_t local_port) {
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

    // macOS: when send() fails with EPIPE (e.g. the peer is unreachable),
    // the kernel also raises SIGPIPE, whose default action kills the
    // process silently, before net_send can look at errno. This option
    // turns the signal off for this socket so send() just returns -1.
    // Linux and Windows don't have the option (Linux raises SIGPIPE only
    // for stream sockets; Windows has no SIGPIPE), hence the #ifdef.
#ifdef SO_NOSIGPIPE
    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) < 0) {
        perror("net: setsockopt SO_NOSIGPIPE");
        net_shutdown();
        return false;
    }
#endif

    // Sending to a broadcast address fails with EACCES unless the socket
    // asks for it first: a guard against broadcasting by accident, since a
    // broadcast reaches every machine on the LAN. Used by LAN discovery.
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof broadcast) < 0) {
        perror("net: setsockopt SO_BROADCAST");
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
    return true;
}

bool net_connect(NetAddr peer_addr) {
    struct sockaddr_in peer = to_sockaddr(peer_addr);

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

bool net_send_to(NetAddr to, const uint8_t *data, size_t len) {
    struct sockaddr_in addr = to_sockaddr(to);
    // sendto: send with an explicit destination, for a socket that isn't
    // connected (or to go somewhere other than the connected peer).
    ssize_t sent = sendto(sock, data, len, 0, (struct sockaddr *)&addr, sizeof addr);
    if (sent < 0) {
        // Same harmless errors as net_send. The caller resends on a timer.
        if (errno != ECONNREFUSED && errno != ENOBUFS && errno != EAGAIN) {
            perror("net: sendto");
        }
        return false;
    }
    return (size_t)sent == len;
}

int net_receive(uint8_t *buf, size_t capacity) {
    return net_receive_from(buf, capacity, NULL);
}

int net_receive_from(uint8_t *buf, size_t capacity, NetAddr *from) {
    for (;;) {
        // recvfrom also fills in the sender's address. On a connected socket
        // that's always the peer; before connecting it can be anyone.
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof sender;
        ssize_t received = recvfrom(sock, buf, capacity, 0,
                                    (struct sockaddr *)&sender, &sender_len);
        if (received >= 0) {
            if (from != NULL) {
                from->ip = ntohl(sender.sin_addr.s_addr);
                from->port = ntohs(sender.sin_port);
            }
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

void net_addr_to_string(NetAddr addr, char *out, size_t capacity) {
    struct in_addr ip = { .s_addr = htonl(addr.ip) };
    char text[INET_ADDRSTRLEN];  // longest "a.b.c.d" plus '\0'
    inet_ntop(AF_INET, &ip, text, sizeof text);
    snprintf(out, capacity, "%s:%u", text, addr.port);
}

void net_local_ip(char *out, size_t capacity) {
    snprintf(out, capacity, "unknown");

    // getifaddrs gives a linked list of every address on every network
    // interface (en0 = Wi-Fi on most Macs, lo0 = loopback, utun = VPNs...).
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) {
        return;
    }
    for (struct ifaddrs *ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;  // no address, or not IPv4
        }
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;  // interface down, or 127.0.0.1
        }
        const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)capacity);
        break;
    }
    freeifaddrs(list);
}
