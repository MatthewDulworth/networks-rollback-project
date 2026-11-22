#!/usr/bin/env bash
# Play as player 1 (blue) against another machine running player2.sh.
#
# Usage: scripts/player1.sh <other_machine_ip> [--lag ms] [--jitter ms] [--loss pct] [--delay]
#   e.g. scripts/player1.sh 192.168.1.42
#
# Run it with no arguments first: it prints this machine's IP to give to the
# other player.

exec "$(dirname "$0")/run_peer.sh" 0 "$@"
