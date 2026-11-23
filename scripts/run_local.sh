#!/usr/bin/env bash
# Build, then run two peers on this machine talking over localhost UDP.
# Output from each is prefixed with [p1] / [p2]. Ctrl-C (or closing either
# window) stops both.
#
# Usage: scripts/run_local.sh [base_port] [--lag ms] [--jitter ms] [--loss pct] [--delay]
#   base_port: default 5000; uses base_port and base_port+1
#   e.g. scripts/run_local.sh --lag 100 --jitter 20 --loss 10
#
# --lag/--jitter/--loss go to p1 only: one peer's netsim already affects both
# directions, so giving it to both would double it. Every other flag
# (--delay) goes to both, since both sides must run the same netcode.

set -euo pipefail

cd "$(dirname "$0")/.."

usage() {
    echo "usage: $0 [base_port] [--lag ms] [--jitter ms] [--loss pct] [--delay]" >&2
    echo "  base_port: default 5000; uses base_port and base_port+1" >&2
    exit 1
}

# Optional base port first: anything not starting with "-". It must be a
# number in 1..65534.
BASE_PORT=5000
if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
    BASE_PORT="$1"
    shift
fi
if ! [[ "$BASE_PORT" =~ ^[0-9]+$ ]] || [ "$BASE_PORT" -lt 1 ] || [ "$BASE_PORT" -gt 65534 ]; then
    usage
fi

# Split the remaining flags: netsim ones (with their value) for p1 only,
# the rest for both. The game itself checks the values.
P1_FLAGS=()
BOTH_FLAGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --lag|--jitter|--loss)
            if [ "$#" -lt 2 ]; then
                usage
            fi
            P1_FLAGS+=("$1" "$2")
            shift 2
            ;;
        *)
            BOTH_FLAGS+=("$1")
            shift
            ;;
    esac
done

PORT_A="$BASE_PORT"
PORT_B="$((BASE_PORT + 1))"

cmake -S . -B build > /dev/null
cmake --build build

# On exit (Ctrl-C, or one peer quitting), kill everything this script
# started. `kill 0` signals the script's whole process group.
trap 'trap - EXIT; kill 0 2>/dev/null' EXIT INT TERM

# ${arr[@]+"${arr[@]}"} expands to nothing for an empty array. Plain
# "${arr[@]}" would be an "unbound variable" error under set -u in macOS's
# bash 3.2.
./build/game "$PORT_A" 127.0.0.1 "$PORT_B" 0 \
    ${P1_FLAGS[@]+"${P1_FLAGS[@]}"} ${BOTH_FLAGS[@]+"${BOTH_FLAGS[@]}"} 2>&1 | sed -l 's/^/[p1] /' &
PID_A=$!
./build/game "$PORT_B" 127.0.0.1 "$PORT_A" 1 \
    ${BOTH_FLAGS[@]+"${BOTH_FLAGS[@]}"} 2>&1 | sed -l 's/^/[p2] /' &
PID_B=$!

# Return as soon as either peer exits; the trap then stops the other.
# ($! is the pipeline's last command, sed, which exits when its game does.
# macOS ships bash 3.2, which has no `wait -n`, hence the polling.)
while kill -0 "$PID_A" 2>/dev/null && kill -0 "$PID_B" 2>/dev/null; do
    sleep 0.5
done
