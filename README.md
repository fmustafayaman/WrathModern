# wowdecomp

A C++ decompilation of the World of Warcraft 3.3.5a client (`Wow.exe`, build 12340, June 2010).

Join the discord for other clients and to show off some cool stuff you build.

https://discord.gg/DsZMGUD8aT

Every function is reconstructed from the original binary and keeps the address it came from in an
`// OFFSET:` tag, so each one can be compared against the original. The source tree follows the layout
of the original client (`Engine/Source/<Module>`, `WoW/Source/<Module>`, original file names).

x64 is the main target. Win32 still builds and is used to check struct layouts against the 32-bit
original.

## Progress

The percentages below measure how much of the original binary has a counterpart in this source tree. They do
**not** say how much of the game works, and the missing part is not missing gameplay: the client builds into a
complete executable and the game is 100% playable. Bugs could still be hiding, though; a ported function can behave differently
from the original.

Measured against the 12340 binary on 2026-10-15. A function counts as ported when its address is
tagged in the source.

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

The total leaves out 658 functions that IDA identifies as compiler library code and 132 import thunks.
Most of the untagged remainder is code the build gets elsewhere: compiler-generated code (static
initializers, template instances) that our compiler emits on its own, C runtime helpers, and third-party
libraries that are linked from their upstream sources instead of being decompiled (FreeType, zlib, PCRE,
parts of FMOD Ex).

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK)
- CMake 3.10 or newer (the one that ships with Visual Studio works)
- For Win32 builds only: [NASM](https://www.nasm.us/), on `PATH` or passed as `-DNASM_EXECUTABLE=<path>`, and
  `DivxDecoder.dll` from your 3.3.5a client copied to `ThirdParty/DivxDecoder/` (the build copies it next to the
  executables; the x64 build uses its own decoder instead)
- A World of Warcraft **3.3.5a (12340)** client. No game data is included in this repository.

## Building

From a Developer PowerShell or any shell with `cmake` on `PATH`:

```powershell
git clone https://github.com/SatyPardus/wowdecomp.git
cd wowdecomp

# x64 (main target)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --target WowClient --parallel

# Win32 (optional, needs NASM)
cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build/win32 --config Release --target WowClient --parallel
```

In-source builds are refused, so keep the build directory separate (`build/` above).

The client is written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger: it opens a console window and writes
`WowClient_crash.log` when the client crashes. `Wow` is the same client without it. You can also turn the
logger off in `WowClient` with `-DWOWCLIENT_CRASH_LOG=OFF`.

### Options

| CMake option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` | Skip the MPQ file and `.toc.sig` signature checks, so modified game data and interface files load |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Name the `WowClient` executable something other than `WowClient64` / `WowClient32` |

## Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Point `Data/<locale>/realmlist.wtf` at the server you want to connect to.
3. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

## macOS (WowRe branch)

The `WowRe` branch builds a native macOS client for Apple Silicon. It renders through Metal directly, with no
Direct3D or OpenGL translation layer, and it can load optional mods from the game's `Mods` folder.

### Requirements

- macOS on Apple Silicon (tested on an M4)
- Xcode Command Line Tools, CMake 3.10 or newer and Ninja
- The Metal toolchain (the `metal` shader compiler)
- A World of Warcraft **3.3.5a (12340)** client

### Building

```sh
# Release build
cmake -S . -B build/mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/mac-release

# Development build: copies the app and mods into a game directory after each build
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWRATHMODERN_GAME_DIR="/path/to/World of Warcraft" -DWRATHMODERN_APP_NAME=WrathModern-WowRe
ninja -C build/mac
```

The optimised configurations are built with `-O3 -mcpu=apple-m1`. The client is written to
`bin/Mac/WrathModern.app` and the mods to `<build dir>/lib/*.dylib`.

| CMake option | Default | Effect |
|---|---|---|
| `WRATHMODERN_CLIENT_PATCHES` | `ON` | Client improvements, such as keeping the loading screen up until the first world frames are steady |
| `WRATHMODERN_MODS` | `ON` | Load mods from the game's `Mods` folder |
| `WRATHMODERN_BUILD_MODS` | `ON` | Build the mods in `Mods/` with the client |
| `WRATHMODERN_GAME_DIR` | empty | Game directory that the app and mods are copied into after each build |
| `WRATHMODERN_APP_NAME` | `WrathModern` | Name of the app copied into the game directory |

### Installing

Copy the app, `Mods/` and `Interface/AddOns/` into the 3.3.5a game folder, next to `Data/`. Prebuilt packages
are attached to the releases on the project's Git server. Each package also has a `README.txt` with the same
steps.

### Mods

| Mod | Files | Description |
|---|---|---|
| CoAVolFog | `CoAVolFog.dylib`, `fogdata.bin`, `waterdata.bin`, `CoAVolFog.ini` | Volumetric fog, god rays, local lights in fog, colour grading, and FFT water with ripples. Settings: `/coavolfog` or Option+F7 |
| AwesomeWotlk | `AwesomeWotlk.dylib`, `Interface/AddOns/AwesomeCVar` | AwesomeWotlk API extensions with a settings panel (`/awesome`) |
| LuxShoulderCam | `LuxShoulderCam.dylib`, `Interface/AddOns/LuxShoulderCam` | Over-the-shoulder camera (`/lsc`) |

To turn a mod off, remove its `.dylib` from `Mods/`.

### Testing and measuring

- `Tools/autotest.sh <out dir> "world@N,..." [timeout]` logs in, takes screenshots and quits. It only stops the
  process it started, and it restores the player's `WTF` folder afterwards.
- `WRATHMODERN_FRAMESTATS=1` prints a line every 5 seconds with:
  - frame time, plus CPU and GPU time;
  - frames that reached the display, refreshes that showed no new frame (`missed refreshes`), and drops;
  - every long frame, with the time it spent waiting for a drawable and compiling shaders.
- `WRATHMODERN_NO_VSYNC=1` turns vsync off, to measure uncapped frame times.
- `COAVOLFOG_GPU_PROFILE=1` logs the GPU time of each CoAVolFog pass to `Mods/CoAVolFog.log`.
- `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` turns on Metal validation. It slows the client down heavily, so
  turn it off when measuring performance.

## Layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, Frame/FrameXML UI, SoundEngine, Lua, ... |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `Tools/WowClient/` | The `WowClient` executable target |
| `Tools/XvidDecoder/` | x64 movie decoder that replaces the original 32-bit `DivxDecoder.dll` |

## Special thanks

- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- Everyone who has reported bugs so far
- The WoW modding community
