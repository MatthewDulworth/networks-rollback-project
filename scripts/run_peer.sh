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

# Called as run_peer.sh <player> [peer_ip ...]: a missing peer IP means the
# person forgot it on player1.sh / player2.sh, so describe that command.
if [ "$#" -lt 2 ]; then
    echo "usage: scripts/player$(( ${1:-0} + 1 )).sh <other_machine_ip> [--lag ms] [--jitter ms] [--loss pct]" >&2
    echo "  (the other machine's IP is on its overlay, after \"me\")" >&2
    exit 1
fi
PLAYER="$1"
PEER_IP="$2"
shift 2

# Rebuild every time, so both machines are sure to run the code they have
# checked out. A no-op build takes about a second.
cmake -S . -B build > /dev/null
cmake --build build

# exec: the game replaces this script, so Ctrl-C goes straight to it.
# Anything after the peer IP (--lag etc.) is passed through unchanged.
exec ./build/game "$PORT" "$PEER_IP" "$PORT" "$PLAYER" "$@"
