#!/usr/bin/env bash
# Build, then run two peers on this machine talking over localhost UDP.
# Output from each is prefixed with [p1] / [p2]. Ctrl-C (or closing either
# window) stops both.
#
# Usage: scripts/run_local.sh [base_port]   (default 5000; uses base and base+1)

set -euo pipefail

cd "$(dirname "$0")/.."

usage() {
    echo "usage: $0 [base_port]   (default 5000; uses base_port and base_port+1)" >&2
    exit 1
}

# At most one argument, and it must be a number in 1..65534.
if [ "$#" -gt 1 ]; then
    usage
fi
BASE_PORT="${1:-5000}"
if ! [[ "$BASE_PORT" =~ ^[0-9]+$ ]] || [ "$BASE_PORT" -lt 1 ] || [ "$BASE_PORT" -gt 65534 ]; then
    usage
fi

PORT_A="$BASE_PORT"
PORT_B="$((BASE_PORT + 1))"

cmake -S . -B build > /dev/null
cmake --build build

# On exit (Ctrl-C, or one peer quitting), kill everything this script
# started. `kill 0` signals the script's whole process group.
trap 'trap - EXIT; kill 0 2>/dev/null' EXIT INT TERM

./build/game "$PORT_A" 127.0.0.1 "$PORT_B" 0 2>&1 | sed -l 's/^/[p1] /' &
PID_A=$!
./build/game "$PORT_B" 127.0.0.1 "$PORT_A" 1 2>&1 | sed -l 's/^/[p2] /' &
PID_B=$!

# Return as soon as either peer exits; the trap then stops the other.
# ($! is the pipeline's last command, sed, which exits when its game does.
# macOS ships bash 3.2, which has no `wait -n`, hence the polling.)
while kill -0 "$PID_A" 2>/dev/null && kill -0 "$PID_B" 2>/dev/null; do
    sleep 0.5
done
