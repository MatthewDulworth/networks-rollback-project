#!/usr/bin/env bash
# Play as player 2 (red) against another machine running player1.sh.
#
# Usage: scripts/player2.sh <other_machine_ip> [--lag ms] [--jitter ms] [--loss pct]
#   e.g. scripts/player2.sh 192.168.1.42
#
# The other machine's IP is shown on its overlay ("me ...").

exec "$(dirname "$0")/run_peer.sh" 1 "$@"
