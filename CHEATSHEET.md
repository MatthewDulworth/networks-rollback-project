# Cheat sheet

Quick reference. Details in `DEVELOPMENT.md`.

## Build

```sh
cmake -S . -B build                              # configure (first time / after rm -rf build)
cmake --build build                              # compile
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # or =Debug (default)
rm -rf build && cmake -S . -B build && cmake --build build   # clean rebuild
```

## Run

```sh
scripts/run_local.sh [base_port] [flags]   # both peers on this machine (5000/5001)
scripts/player1.sh                         # print this machine's IP
scripts/player1.sh <other_ip> [flags]      # machine A (blue)
scripts/player2.sh <other_ip> [flags]      # machine B (red)
scripts/lan.sh [flags]                     # both machines, no IPs

./build/game <local_port> <peer_ip> <peer_port> <0|1> [flags]
./build/game lan [flags]
```

## Flags

| Flag | | Where |
|---|---|---|
| `--lag <ms>` | added delay | one peer |
| `--jitter <ms>` | random extra delay | one peer |
| `--loss <pct>` | packet loss | one peer |
| `--delay` | delay netcode, no rollback | both peers |

`run_local.sh` sends netsim flags to p1 and `--delay` to both.

## Keys

| | |
|---|---|
| A / D | move |
| Space | jump (hold = higher) |
| WASD | aim (else facing) |
| K (hold) | plant: aim without moving (ground only) |
| J | shoot |
| 1 / 2 | lag -/+ 20 ms |
| 3 / 4 | jitter -/+ 10 ms |
| 5 / 6 | loss -/+ 5% |
| 0 | reset netsim |
| M | toggle delay netcode / rollback |
| Tab | stats overlay |
| Enter | fullscreen |

## Tests

```sh
ctest --test-dir build --output-on-failure   # all (~20 s)
ctest --test-dir build -R packet             # one: packet | netcode | smoothness
```
