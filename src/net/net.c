#include "net/net.h"

#include <arpa/inet.h>   // inet_ntop, htons
#include <errno.h>
#include <fcntl.h>       // fcntl, O_NONBLOCK
#include <ifaddrs.h>     // getifaddrs
#include <net/if.h>      // IFF_UP, if_nametoindex, if_indextoname
#include <netdb.h>       // getaddrinfo
#include <netinet/in.h>  // sockaddr_in6, in6addr_any, IPV6_JOIN_GROUP
#include <stdio.h>       // perror, fprintf
#include <string.h>      // memset, memcpy
#include <sys/socket.h>
#include <unistd.h>      // close

// The one socket this module owns. -1 means "not open".
static int sock = -1;

// IPv4 a.b.c.d as an IPv4-mapped IPv6 address ::ffff:a.b.c.d. A dual-stack
// socket sends these as plain IPv4 packets.
static void map_ipv4(const struct in_addr *v4, uint8_t out[16]) {
    memset(out, 0, 10);
    out[10] = 0xff;
    out[11] = 0xff;
    memcpy(out + 12, &v4->s_addr, 4);  // already network byte order
}

bool net_init(uint16_t local_port, const char *peer_ip, uint16_t peer_port) {
    if (!net_open(local_port)) {
        return false;
    }

    // Peer address text -> NetAddr. getaddrinfo with AI_NUMERICHOST parses
    // IPv4 and IPv6 text (including "fe80::1%en0", whose interface becomes
    // the scope id) without doing a DNS lookup.
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    struct addrinfo *result;
    if (getaddrinfo(peer_ip, NULL, &hints, &result) != 0) {
        fprintf(stderr, "net: invalid peer address '%s'\n", peer_ip);
        net_shutdown();
        return false;
    }
    NetAddr peer = { .port = peer_port };
    if (result->ai_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)result->ai_addr;
        map_ipv4(&v4->sin_addr, peer.ip);
    } else {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)result->ai_addr;
        memcpy(peer.ip, &v6->sin6_addr, 16);
        peer.scope_id = v6->sin6_scope_id;
    }
    freeaddrinfo(result);
    return net_connect(peer);
}

// NetAddr -> sockaddr_in6, the form the socket calls take.
static struct sockaddr_in6 to_sockaddr(NetAddr addr) {
    struct sockaddr_in6 out;
    memset(&out, 0, sizeof out);
#ifdef SIN6_LEN
    out.sin6_len = sizeof out;  // BSD/macOS have a length field; Linux doesn't
#endif
    out.sin6_family = AF_INET6;
    out.sin6_port = htons(addr.port);
    memcpy(&out.sin6_addr, addr.ip, 16);
    // For link-local addresses: which interface to send on. fe80::1 on
    // Wi-Fi and fe80::1 on the cable are different machines.
    out.sin6_scope_id = addr.scope_id;
    return out;
}

bool net_open(uint16_t local_port) {
    // AF_INET6 = IPv6, SOCK_DGRAM = UDP (0 lets the OS pick the protocol,
    // which for a datagram socket is UDP).
    sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("net: socket");
        return false;
    }

    // Dual-stack: also send and receive IPv4, as IPv4-mapped addresses. The
    // default differs between systems, so set it explicitly.
    int v6only = 0;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only) < 0) {
        perror("net: setsockopt IPV6_V6ONLY");
        net_shutdown();
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

    // Bind our side: receive packets sent to local_port on any of this
    // machine's addresses, IPv4 and IPv6 (localhost, Wi-Fi, cable, ...).
    struct sockaddr_in6 local;
    memset(&local, 0, sizeof local);
#ifdef SIN6_LEN
    local.sin6_len = sizeof local;
#endif
    local.sin6_family = AF_INET6;
    local.sin6_addr = in6addr_any;
    local.sin6_port = htons(local_port);
    if (bind(sock, (struct sockaddr *)&local, sizeof local) < 0) {
        perror("net: bind");  // EADDRINUSE: another copy already has this port
        net_shutdown();
        return false;
    }
    return true;
}

bool net_connect(NetAddr peer_addr) {
    struct sockaddr_in6 peer = to_sockaddr(peer_addr);

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

// An interface that can reach other machines on the LAN: up, has a link,
// can multicast, and isn't loopback (lo0) or a point-to-point tunnel (utun:
// VPNs, iCloud Private Relay).
static bool lan_interface(unsigned flags) {
    return (flags & IFF_UP) && (flags & IFF_RUNNING) && (flags & IFF_MULTICAST)
        && !(flags & (IFF_LOOPBACK | IFF_POINTOPOINT));
}

int net_join_multicast(const char *group, uint16_t port, NetAddr *dests, int max) {
    struct in6_addr group_addr;
    if (inet_pton(AF_INET6, group, &group_addr) != 1) {
        fprintf(stderr, "net: invalid multicast group '%s'\n", group);
        return 0;
    }

    // getifaddrs lists every address on every interface. Every interface
    // with IPv6 has exactly one link-local (fe80::) address, so visiting
    // only those visits each IPv6 interface once.
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) {
        perror("net: getifaddrs");
        return 0;
    }
    int count = 0;
    for (struct ifaddrs *ifa = list; ifa != NULL && count < max; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET6) {
            continue;
        }
        const struct sockaddr_in6 *addr = (const struct sockaddr_in6 *)ifa->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) {
            continue;
        }
        if (!lan_interface(ifa->ifa_flags)) {
            continue;
        }
        unsigned index = if_nametoindex(ifa->ifa_name);
        if (index == 0) {
            continue;
        }

        // Ask the kernel to deliver packets sent to the group on this
        // interface to our socket. The network card also starts accepting
        // the group's Ethernet multicast address.
        struct ipv6_mreq join;
        memset(&join, 0, sizeof join);
        join.ipv6mr_multiaddr = group_addr;
        join.ipv6mr_interface = index;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &join, sizeof join) < 0) {
            fprintf(stderr, "net: join %s on %s: %s\n", group, ifa->ifa_name, strerror(errno));
            continue;
        }

        NetAddr *dest = &dests[count++];
        memcpy(dest->ip, &group_addr, 16);
        dest->port = port;
        dest->scope_id = index;  // send on this interface
    }
    freeifaddrs(list);
    return count;
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
    struct sockaddr_in6 addr = to_sockaddr(to);
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
        // sockaddr_storage is big enough for any address type. A
        // dual-stack socket reports IPv4 senders as IPv4-mapped IPv6.
        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof sender;
        ssize_t received = recvfrom(sock, buf, capacity, 0,
                                    (struct sockaddr *)&sender, &sender_len);
        if (received >= 0) {
            if (from != NULL) {
                memset(from, 0, sizeof *from);
                if (sender.ss_family == AF_INET6) {
                    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&sender;
                    memcpy(from->ip, &v6->sin6_addr, 16);
                    from->port = ntohs(v6->sin6_port);
                    from->scope_id = v6->sin6_scope_id;
                } else if (sender.ss_family == AF_INET) {
                    const struct sockaddr_in *v4 = (const struct sockaddr_in *)&sender;
                    map_ipv4(&v4->sin_addr, from->ip);
                    from->port = ntohs(v4->sin_port);
                }
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

bool net_addr_equal(NetAddr a, NetAddr b) {
    return memcmp(a.ip, b.ip, 16) == 0 && a.port == b.port && a.scope_id == b.scope_id;
}

void net_addr_to_string(NetAddr addr, char *out, size_t capacity) {
    struct in6_addr ip;
    memcpy(&ip, addr.ip, 16);
    if (IN6_IS_ADDR_V4MAPPED(&ip)) {
        // Show IPv4 the familiar way: the last 4 bytes, "a.b.c.d:port".
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, addr.ip + 12, text, sizeof text);
        snprintf(out, capacity, "%s:%u", text, addr.port);
        return;
    }
    char text[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &ip, text, sizeof text);
    // "%en0" for link-local: the address alone doesn't say which interface.
    char scope[IF_NAMESIZE + 1] = "";
    char name[IF_NAMESIZE];
    if (addr.scope_id != 0 && if_indextoname(addr.scope_id, name) != NULL) {
        snprintf(scope, sizeof scope, "%%%s", name);
    }
    // Brackets keep the port's ':' apart from the address's colons.
    snprintf(out, capacity, "[%s%s]:%u", text, scope, addr.port);
}

void net_local_addr(uint16_t port, char *out, size_t capacity) {
    snprintf(out, capacity, "unknown");

    // getifaddrs gives a linked list of every address on every network
    // interface (en0 = Wi-Fi on most Macs, lo0 = loopback, utun = VPNs...).
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) {
        return;
    }

    // First choice: a real IPv4 address, the easiest to read off and type.
    // Second: the IPv6 link-local address, what LAN discovery connects to.
    bool have_v4 = false;
    bool have_v6 = false;
    NetAddr v4 = { .port = port };
    NetAddr v6 = { .port = port };
    for (struct ifaddrs *ifa = list; ifa != NULL && !have_v4; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || !lan_interface(ifa->ifa_flags)) {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET) {
            const struct sockaddr_in *addr = (const struct sockaddr_in *)ifa->ifa_addr;
            // 192.0.0.x is reserved. On an IPv6-only network (some phone
            // hotspots) macOS gives itself 192.0.0.2 for its IPv4-to-IPv6
            // translator (CLAT). Every machine gets the same one, so it's
            // no use to the other player.
            const uint8_t *bytes = (const uint8_t *)&addr->sin_addr.s_addr;
            if (bytes[0] == 192 && bytes[1] == 0 && bytes[2] == 0) {
                continue;
            }
            map_ipv4(&addr->sin_addr, v4.ip);
            have_v4 = true;
        } else if (ifa->ifa_addr->sa_family == AF_INET6 && !have_v6) {
            const struct sockaddr_in6 *addr = (const struct sockaddr_in6 *)ifa->ifa_addr;
            if (!IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) {
                continue;
            }
            memcpy(v6.ip, &addr->sin6_addr, 16);
            // getifaddrs may leave the scope id 0 for link-local addresses,
            // so look up the interface by name instead.
            v6.scope_id = if_nametoindex(ifa->ifa_name);
            have_v6 = true;
        }
    }
    freeifaddrs(list);

    if (have_v4) {
        net_addr_to_string(v4, out, capacity);
    } else if (have_v6) {
        net_addr_to_string(v6, out, capacity);
    }
}
