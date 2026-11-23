#!/usr/bin/env bash
# Play as player 2 (red) against another machine running player1.sh.
#
# Usage: scripts/player2.sh <other_machine_ip> [--lag ms] [--jitter ms] [--loss pct] [--delay] [--no-lobby]
#   e.g. scripts/player2.sh 192.168.1.42
#
# Run it with no arguments first: it prints this machine's IP to give to the
# other player.

exec "$(dirname "$0")/run_peer.sh" 1 "$@"
