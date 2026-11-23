# Development Guide

How to build, run and maintain this project on macOS.

## One-time setup

Install the tools (Homebrew is already installed on this machine):

```sh
xcode-select --install       # Apple's C compiler (clang) and linker, if not already installed
brew install cmake raylib
```

Check they're there:

```sh
cmake --version
brew list --versions raylib
```

In VS Code, install the **C/C++** and **CMake Tools** extensions. The project's
`.vscode/settings.json` already points them at Homebrew's `cmake` and at the
build's `compile_commands.json`.

## Build and run

From the project root:

```sh
cmake -S . -B build          # configure: generate build files into build/
cmake --build build          # compile
scripts/run_local.sh         # build and run two peers on this machine
```

The game needs a peer to play against. Each copy takes
`<local_port> <peer_ip> <peer_port> <player 0|1>`, and the two sides must use
mirrored ports and opposite players:

```sh
./build/game 5000 127.0.0.1 5001 0     # terminal 1
./build/game 5001 127.0.0.1 5000 1     # terminal 2
```

`scripts/run_local.sh` does exactly that, prefixes each peer's output with
`[p1]`/`[p2]`, and stops both when either window closes. Pass a different base
port if 5000/5001 are taken: `scripts/run_local.sh 6000`. Only the focused
window receives keys, so click the window you want to control.

### Two machines

One machine runs `player1.sh`, the other `player2.sh`, each with the other's
LAN IP. They build first, then start the game on port 5000. Extra flags
(`--lag` etc.) are passed through:

```sh
scripts/player1.sh <other_ip>     # machine A (blue)
scripts/player2.sh <other_ip>     # machine B (red)
```

That's the same as running the game directly with the same port on both and
opposite players:

```sh
./build/game 5000 <other_ip> 5000 0     # machine A
./build/game 5000 <other_ip> 5000 1     # machine B
```

To find out what to type, run `scripts/player1.sh` (or `player2.sh`) with no
arguments on each machine: it prints that machine's IP. The overlay also shows
it (the "me" line) once the game is running. Accept the macOS
"allow incoming connections" prompt on both. See `docs/networking.md`.

Or skip the IPs: run `scripts/lan.sh` (same as `./build/game lan`) on both
machines. They find each other by multicast on port 5000 and pick players
themselves. Two machines only. See "LAN discovery" in `docs/networking.md`.

### Simulating a bad network

Add `--lag <ms>`, `--jitter <ms>` and/or `--loss <pct>` after the four
arguments, on one machine only (it affects both directions):

```sh
./build/game 5000 127.0.0.1 5001 0 --lag 100 --jitter 20 --loss 10
```

Or change them live in the focused window: 1/2 lag, 3/4 jitter, 5/6 loss,
0 reset. `scripts/run_local.sh` doesn't pass flags through, so use the keys
there.

### Controls

| Action | Keys |
|---|---|
| Move | A / D |
| Jump | Space |
| Aim | WASD (falls back to facing) |
| Plant (hold, on ground) | K |
| Shoot | J |

Run the tests with `ctest --test-dir build --output-on-failure`:

| Test | What it checks | Time |
|---|---|---|
| `packet` | Encoding and decoding packets, rejecting bad ones. | instant |
| `netcode` | End to end: for each of four links (clean; 100 ms + 10% loss; 250 ms + 20% loss; delay mode at 60 ms + 10% loss) it starts two real peers on localhost and plays 8 s with scripted random input. Fails on a desync between the peers, a failed assert (including the local desync check), a peer that doesn't finish, hashes that stop being compared, no rollbacks on a lossy link (or any in delay mode), or a loop that spins instead of sleeping. | ~10 s |
| `smoothness` | Rollback vs delay mode on the same links (clean, 50, 100, 200, 500 ms RTT, and 200 ms with jitter and loss). Measures game speed, frozen frames, longest freeze and input latency, prints a table, and fails unless the modes match on a clean or 50 ms link and rollback is clearly smoother from 100 ms up. | ~10 s |

The netcode tests need UDP ports 47100-47107 and 47200-47221 to be free. Run
`./build/netcode_test` to see each peer's numbers (rollbacks, depth, stalls,
frames per second), and `./build/smoothness_test` for the comparison table.

You only need the configure step the first time, or after deleting `build/`.
After that, `cmake --build build` is enough. It re-runs the configure step
automatically if `CMakeLists.txt` has changed.

**In VS Code:** use the CMake Tools buttons in the bottom status bar (Build ⚙,
Run ▶), or the Command Palette: *CMake: Build*, *CMake: Run Without Debugging*.

### Debug vs. Release

The default is a **Debug** build (`-g`, no optimization), which is what you want
while developing. For an optimized build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Switch back with `-DCMAKE_BUILD_TYPE=Debug`. The value is cached in `build/`,
so it sticks until you change it or delete the folder.

### Clean rebuild

```sh
rm -rf build
cmake -S . -B build
cmake --build build
```

Do this when the build acts strangely, after upgrading raylib or CMake, or
after changing compiler or build-type settings.

## Editing CMakeLists.txt

### Adding a source file

Add it to the target's file list. Paths are relative to the project root:

```cmake
add_executable(game
    src/main.c
    src/render/render.c
)
```

Then run `cmake --build build` as usual. If a file lives in another folder and
you `#include` its header from elsewhere, tell CMake where the headers are:

```cmake
target_include_directories(game PRIVATE src/render)
```

### Adding a library (a group of files shared by several programs)

```cmake
add_library(sim STATIC src/sim/game.c)
target_include_directories(sim PUBLIC src/sim)   # PUBLIC: users of sim see these headers
target_compile_options(sim PRIVATE -Wall -Wextra)

target_link_libraries(game PRIVATE sim)
```

`PRIVATE` means "only for this target". `PUBLIC` means "for this target and
anything that links to it".

### Adding another program

```cmake
add_executable(replay tools/replay/replay.c)
target_compile_options(replay PRIVATE -Wall -Wextra)
```

It builds to `./build/replay`. To build just one target:

```sh
cmake --build build --target replay
```

## Updating tools

```sh
brew update                  # refresh Homebrew's package list
brew upgrade cmake raylib    # upgrade just these two
```

After upgrading **raylib**, do a clean rebuild. Check its changelog for
renamed or removed functions before upgrading in the middle of the project:
`/opt/homebrew/Cellar/raylib/<version>/CHANGELOG`.

To pin raylib so a general `brew upgrade` doesn't touch it:

```sh
brew pin raylib              # undo with: brew unpin raylib
```

Current versions when this guide was written: CMake 4.4.5, raylib 6.2.

## IntelliSense (red squiggles in VS Code)

VS Code learns include paths from `build/compile_commands.json`, which CMake
writes on every configure. If `#include` errors come back:

1. Make sure `build/` exists. If not, run `cmake -S . -B build`.
2. Command Palette → *C/C++: Reset IntelliSense Database*.
3. Command Palette → *Developer: Reload Window*.

## Git

`.gitignore` excludes `build/` and `.DS_Store`. Never commit `build/`: it's
machine-specific and always regenerated.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Bad CMake executable` in VS Code | `brew install cmake`, then *Developer: Reload Window* |
| `Could not find a package configuration file provided by "raylib"` | `brew install raylib`. If it's installed, configure with `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew` |
| `ld: warning: search path '.../MacOSX15.sdk/...' not found` | Harmless. It comes from Homebrew's raylib package and can be ignored |
| `xcrun: error: invalid active developer path` | Command Line Tools are missing or broken: `xcode-select --install` |
| Build uses old settings after editing CMake options | Clean rebuild (`rm -rf build`) |
| `./build/game: No such file or directory` | You're not in the project root, or the build failed. Check the build output |
