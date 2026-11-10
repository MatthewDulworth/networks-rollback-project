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
./build/game                 # run
```

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
add_executable(netsim tools/netsim/netsim.c)
target_compile_options(netsim PRIVATE -Wall -Wextra)
```

It builds to `./build/netsim`. To build just one target:

```sh
cmake --build build --target netsim
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

The project isn't a git repository yet. To start one:

```sh
git init
git add .
git commit -m "Initial project setup"
```

`.gitignore` already excludes `build/` and `.DS_Store`. Never commit `build/`:
it's machine-specific and always regenerated.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Bad CMake executable` in VS Code | `brew install cmake`, then *Developer: Reload Window* |
| `Could not find a package configuration file provided by "raylib"` | `brew install raylib`. If it's installed, configure with `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew` |
| `ld: warning: search path '.../MacOSX15.sdk/...' not found` | Harmless. It comes from Homebrew's raylib package and can be ignored |
| `xcrun: error: invalid active developer path` | Command Line Tools are missing or broken: `xcode-select --install` |
| Build uses old settings after editing CMake options | Clean rebuild (`rm -rf build`) |
| `./build/game: No such file or directory` | You're not in the project root, or the build failed. Check the build output |
