#!/usr/bin/env bash
# Build, then run this machine's side of a two-machine match. Called by
# player1.sh / player2.sh; use those instead of running this directly.
#
# Usage: scripts/run_peer.sh <player 0|1> <peer_ip> [--lag ms] [--jitter ms] [--loss pct]
#
# Both machines use port 5000. That's fine because they're different
# machines; on one machine use run_local.sh instead.

set -euo pipefail

cd "$(dirname "$0")/.."

PORT=5000

# This machine's LAN IP, or "unknown". Asks which interface the default route
# (where traffic to other machines goes) uses, so it works on Wi-Fi, a phone
# hotspot or Ethernet alike. Falls back to en0/en1, the usual Wi-Fi and
# Ethernet names, if that interface has no IPv4 address (e.g. a VPN tunnel).
local_ip() {
    local iface
    iface=$(route -n get default 2>/dev/null | awk '/interface:/ { print $2 }')
    for i in $iface en0 en1; do
        if ipconfig getifaddr "$i" 2>/dev/null; then
            return
        fi
    done
    echo "unknown"
}

# Called as run_peer.sh <player> [peer_ip ...]. No peer IP: show this
# machine's IP so the two players can swap addresses, then how to start.
if [ "$#" -lt 2 ]; then
    echo "This machine's IP: $(local_ip)"
    echo "Give it to the other player, get theirs, then run:"
    echo "  scripts/player$(( ${1:-0} + 1 )).sh <other_machine_ip> [--lag ms] [--jitter ms] [--loss pct]"
    exit 1
fi
PLAYER="$1"
PEER_IP="$2"
shift 2

# Rebuild every time, so both machines are sure to run the code they have
# checked out. A no-op build takes about a second.
cmake -S . -B build > /dev/null
cmake --build build

echo "This machine: $(local_ip):$PORT   connecting to: $PEER_IP:$PORT"

# exec: the game replaces this script, so Ctrl-C goes straight to it.
# Anything after the peer IP (--lag etc.) is passed through unchanged.
exec ./build/game "$PORT" "$PEER_IP" "$PORT" "$PLAYER" "$@"
