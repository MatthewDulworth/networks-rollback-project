#include "net/session.h"
#include "net/netsim.h"
#include "net/packet.h"
#include "rollback/rollback.h"
#include "rollback/sync_check.h"
#include "sim/sim.h"
#include <assert.h>
#include <stdio.h>

// No packet for this long means the peer is probably gone (closed the game,
// lost Wi-Fi). Long enough that a short Wi-Fi hiccup doesn't trigger it.
#define DISCONNECT_TIMEOUT 3.0

// --------------------------------------------------------
// Round-trip time, measured from acks
// --------------------------------------------------------
// When a local input goes out for the first time, remember when. When the
// peer's ack first covers that tick, the time since then is one round trip.
//
// This measures more than the pure network delay: the peer only acks when it
// next sends (end of its frame), and we only read the ack at the start of our
// next frame, so up to a frame on each side is included. A lost packet also
// shows up as a spike, because the ack waits for the resend. That's fine for
// an overlay: it's the delay the game actually experiences.

// Must cover every unacked tick, and the rollback input ring is 32 slots, so
// 64 is plenty. Power of 2 so the slot is a mask, as in rollback.c.
#define SEND_TIME_RING 64

typedef struct {
    Tick tick;
    double time;
} SendTime;

static SendTime send_times[SEND_TIME_RING];
static Tick newest_timed_tick;  // Newest tick with a send time.
static Tick newest_rtt_ack;     // Newest ack already measured.
static double rtt_ms;           // Smoothed; -1 until first sample.

// Peer presence.
static bool peer_seen;
static double last_packet_time;

// Frame advantage
static Tick highest_sender_tick;
static double highest_sender_arrival_time;

void session_init(void) {
    for (size_t i = 0; i < SEND_TIME_RING; i++) {
        send_times[i].tick = NO_TICK;
        send_times[i].time = 0;
    }
    newest_timed_tick = NO_TICK;
    newest_rtt_ack = NO_TICK;
    rtt_ms = -1;
    peer_seen = false;
    last_packet_time = 0;
    highest_sender_tick = NO_TICK;
    highest_sender_arrival_time = 0;
}

static SendTime *send_time_slot(Tick tick) {
    return &send_times[(uint32_t)tick & (SEND_TIME_RING - 1)];
}

// Called for every tick in an outgoing packet. Only the first send counts.
static void note_sent(Tick tick, double now) {
    if (tick > newest_timed_tick) {
        SendTime *slot = send_time_slot(tick);
        slot->tick = tick;
        slot->time = now;
        newest_timed_tick = tick;
    }
}

static void note_ack(Tick ack, double now) {
    if (ack <= newest_rtt_ack) {
        return;  // old or duplicate ack: already measured
    }
    newest_rtt_ack = ack;

    SendTime *slot = send_time_slot(ack);
    if (slot->tick != ack) {
        return;  // a prefilled tick we never sent, or an overwritten slot
    }
    double sample = (now - slot->time) * 1000.0;
    // Exponential moving average: each sample moves the estimate 10% of the
    // way. Smooths jitter so the number on screen is readable.
    rtt_ms = rtt_ms < 0 ? sample : rtt_ms * 0.9 + sample * 0.1;
}

// --------------------------------------------------------
// Receive / send
// --------------------------------------------------------

void session_receive(double now) {
    uint8_t buf[PACKET_MAX_SIZE];
    int len;

    while ((len = netsim_receive(buf, sizeof buf)) > 0) {
        InputPacket packet;
        if (!packet_decode(buf, (size_t)len, &packet)) {
            fprintf(stderr, "net: dropped malformed packet (%d bytes)\n", len);
            continue;
        }
        peer_seen = true;
        last_packet_time = now;

        receive_ack(packet.ack);
        note_ack(packet.ack, now);
        sync_check_add_remote(packet.hash_tick, packet.hash);

        if (packet.sender_tick > highest_sender_tick) {
            highest_sender_tick = packet.sender_tick;
            highest_sender_arrival_time = now;
        }

        // Every packet repeats all inputs the peer hasn't seen acked, so
        // most of these are duplicates. add_remote_input ignores those (and
        // anything past a gap), so no filtering is needed here.
        for (int i = 0; i < packet.count; i++) {
            add_remote_input(packet.first_tick + i, packet.inputs[i]);
        }
    }
}

void session_send(double now, Tick next_tick) {
    InputPacket packet;
    packet.ack = get_last_confirmed_remote_tick();
    packet.sender_tick = next_tick;
    // Only the newest hash. One per packet is enough: packets go out about
    // once per tick, and a hash that's lost just means that tick isn't
    // compared.
    sync_check_newest_local(&packet.hash_tick, &packet.hash);
    find_inputs_to_resend(&packet.first_tick, &packet.count);

    // Keep the oldest inputs if there are too many: the peer needs them in
    // order, and the rest go out once these are acked.
    if (packet.count > PACKET_MAX_INPUTS) {
        packet.count = PACKET_MAX_INPUTS;
    }
    for (int i = 0; i < packet.count; i++) {
        packet.inputs[i] = get_local_input(packet.first_tick + i);
        note_sent(packet.first_tick + i, now);
    }

    uint8_t buf[PACKET_MAX_SIZE];
    size_t len = packet_encode(&packet, buf, sizeof buf);
    assert(len > 0);  // count is clamped and buf is max size, so can't fail
    netsim_send(buf, len);
}

// --------------------------------------------------------
// Status
// --------------------------------------------------------

bool session_peer_seen(void) {
    return peer_seen;
}

bool session_disconnected(double now) {
    // Display only: if packets start arriving again, the game just carries
    // on (stalled at the rollback limit until then).
    return peer_seen && now - last_packet_time > DISCONNECT_TIMEOUT;
}

double session_rtt_ms(void) {
    return rtt_ms;
}

double session_frame_advantage(double now, Tick next_tick) {
    // No estimate yet: no RTT sample, or the peer hasn't started ticking
    // (it sends sender_tick 0 while it waits for our first packet). 0 is
    // inside any dead zone, so the caller does nothing.
    if (rtt_ms < 0 || highest_sender_tick <= 0) {
        return 0;
    }

    // sender_tick was the peer's tick when it sent. Since then the packet
    // spent about half a round trip in flight, and it has been sitting here
    // since it arrived. The peer kept ticking through both.
    double one_way = rtt_ms / 1000.0 / 2;
    double since_arrival = now - highest_sender_arrival_time;
    double remote_now = highest_sender_tick + (one_way + since_arrival) / STEP;
    return next_tick - remote_now;
}
