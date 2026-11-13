# Building Steppe

Steppe builds with CMake and Ninja on Linux, macOS and Windows. The GUI needs
SDL3, OpenGL and zlib; Linux also needs the X11 and EGL development libraries.
The other library dependencies are vendored under `third_party/`. Once the
platform's build tools and system dependencies are installed (including the
first vcpkg install on Windows), the normal build works offline.

Binary names follow `PRODUCT_SLUG` in `cmake/Product.cmake` (`steppe` today).
Each preset builds into `build/<preset>/`. The examples use `release` on
Linux and the platform presets on macOS and Windows.

For prebuilt downloads, see [Installing Steppe](installing.md) instead. Using
the app, plugins and agents is covered in the [manual](manual.md).

## Linux (x86_64)

You need:

- CMake 3.25 or newer, Ninja, Python 3 and Git;
- Clang with C++20 support (CI uses Clang 22; GCC also builds, through the
  `gcc` preset);
- the development packages for SDL3 (3.2 or newer), OpenGL/EGL, zlib and X11.

On Arch Linux:

```sh
sudo pacman -S --needed cmake ninja clang python git sdl3 mesa zlib libx11
```

On Debian or Ubuntu:

```sh
sudo apt install cmake ninja-build clang python3 git \
  libgl-dev libegl-dev zlib1g-dev libx11-dev libxext-dev libxcursor-dev libxrandr-dev
```

Distributions without an SDL3 package (Ubuntu 24.04 and older, for example)
can build it into a private prefix, as CI does:

```sh
curl -fLO https://github.com/libsdl-org/SDL/releases/download/release-3.2.30/SDL3-3.2.30.tar.gz
tar -xzf SDL3-3.2.30.tar.gz
cmake -S SDL3-3.2.30 -B sdl3-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/sdl3" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build sdl3-build && cmake --install sdl3-build
export CMAKE_PREFIX_PATH="$HOME/.local/sdl3"   # before configuring Steppe
```

Build, test and start the app:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
build/release/daw/gui/steppe              # a new project
build/release/daw/gui/steppe song.json    # open one
```

At run time Steppe needs X11 or XWayland and a JACK server. PipeWire's JACK
support (`pipewire-jack`) counts. The JACK library is loaded when the engine
starts, so building needs no JACK development package.

## macOS (Apple Silicon, macOS 14.4+)

You need:

- the Xcode Command Line Tools (`xcode-select --install`), which provide
  Apple Clang and the SDK, including zlib and OpenGL;
- [Homebrew](https://brew.sh) for the rest:

```sh
brew install cmake ninja sdl3 python
```

The `macos` preset builds Debug by default. Pass a build type for an
optimized app:

```sh
cmake --preset macos -DCMAKE_BUILD_TYPE=Release
cmake --build --preset macos -j 8
ctest --preset macos
build/macos/daw/gui/steppe                # a new project
```

The `macos` build preset uses two jobs by default, the limit for GitHub's
hosted runners. Pass `-j` to use more cores. The app bundle is
`build/macos/daw/gui/steppe.app`; `build/macos/daw/gui/steppe` is a symlink to
the executable inside it. Only arm64 is supported.

If `build/macos` was configured with an older Homebrew LLVM toolchain,
reconfigure with `cmake --fresh --preset macos`.

## Windows (x64, Windows 10 1903+)

You need:

- Visual Studio 2022 or its Build Tools, with the **Desktop development with
  C++** workload and the **C++ Clang tools for Windows** component. This
  provides `clang-cl`, the Windows SDK, CMake and Ninja;
- Python 3 and Git for Windows. A few tests use Git's `bash`;
- [vcpkg](https://github.com/microsoft/vcpkg), cloned and bootstrapped, with
  the `VCPKG_ROOT` environment variable pointing at it. The first configure
  installs SDL3 and zlib from `vcpkg.json`, which needs a network connection.

Build from an **x64 Native Tools Command Prompt for VS 2022**, so that the
compiler, linker and SDK are on `PATH`:

```bat
cmake --preset windows -DCMAKE_BUILD_TYPE=Release
cmake --build --preset windows
ctest --preset windows
build\windows\daw\gui\steppe.exe
```

Like `macos`, the `windows` preset builds Debug unless you pass a build type.
Audio uses WASAPI by default, with ASIO available in **Edit > Audio device…**.

## Presets

| Preset | Platform | Purpose |
| --- | --- | --- |
| `dev` | Linux | Clang Debug, the everyday development build |
| `release` | Linux | Clang Release, warnings as errors |
| `linux-package` | Linux | Release distribution targets with isolated static C++ runtimes; tests off |
| `gcc` | Linux | GCC Debug, for GCC-only warnings |
| `asan` | Linux | AddressSanitizer with UndefinedBehaviorSanitizer and leak checks |
| `tsan` | Linux | ThreadSanitizer |
| `rtsan` | Linux | RealtimeSanitizer over the audio-path tests |
| `macos` | macOS | Apple Clang, arm64, Debug by default |
| `windows` | Windows | clang-cl with vcpkg, Debug by default |

`cmake --workflow --preset <name>` configures, builds and tests in one step
for every preset above except `linux-package`, which has no test or workflow
preset. `tools/ci/run-all.sh` runs the license check, the format check and the
six Linux test presets (`dev`, `release`, `gcc`, `asan`, `tsan`, `rtsan`), about
half an hour in all. Pass `-DSUPPLE_BUILD_TESTS=OFF` at configure time to omit
tests; the app, helpers and stock plugins still build. The sanitizer presets
disable the GUI; `gcc` and `asan` exclude the nested source-export test, and
`tsan`/`rtsan` exclude slow tests (`tsan` also excludes `tsan-skip`).
`asan` also runs LeakSanitizer: a test fails when a process it starts exits
with memory nothing points to any more.

`cmake --build --preset dev -j3 --target gui-app.tests` also stages the
stock plugins its cases use (when plugins are enabled), the MCP helper and the
agent skill tree.
Run it with `ctest --preset dev -R '^gui-app\.tests$'`.

Tests that open real hardware are skipped unless you opt in:
`SUPPLE_AUDIO_DEVICE_TEST=1` (the audio device), `SUPPLE_MIDI_DEVICE_TEST=1`
(native MIDI inputs on macOS and Windows) and `SUPPLE_GUI_DISPLAY_TEST=1`
(plugin editor windows on the desktop).

## What the build produces

| Path under `build/<preset>/` | What it is |
| --- | --- |
| `daw/gui/steppe` | The app |
| `host/engine/steppe-engine` | The headless engine: renders, scripts, a headless MCP server |
| `mcp/steppe-mcp` | The MCP adapter that agents connect to the running app through |
| `plugins/clap/` | The stock plugins, which the app loads from here in a build tree |

On Windows each executable has the `.exe` suffix. [The manual](manual.md#agents-mcp)
explains how to connect an agent to a build-tree copy.

Distribution packages (the Linux tarball, the macOS dmg and the Windows
installer) are built with the scripts in `tools/package/`; see
[CONTRIBUTING.md](../../CONTRIBUTING.md#building-distribution-packages).
