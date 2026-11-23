#!/usr/bin/env bash
# Play against another machine on the same LAN without typing its IP. Run
# this on both machines: they find each other by IPv6 multicast on port 5000
# and pick who is player 1 / player 2 themselves.
#
# Usage: scripts/lan.sh [--lag ms] [--jitter ms] [--loss pct] [--delay] [--no-lobby]
#
# Two machines only: two copies on one machine can't share the port. On one
# machine use run_local.sh instead.

set -euo pipefail

cd "$(dirname "$0")/.."

# Rebuild every time, same as run_peer.sh.
cmake -S . -B build > /dev/null
cmake --build build

# exec: the game replaces this script, so Ctrl-C goes straight to it.
exec ./build/game lan "$@"
