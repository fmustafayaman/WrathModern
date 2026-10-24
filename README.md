# WoWReMac — Warden

Native macOS / Apple Silicon client for **World of Warcraft 3.3.5a (build 12340)**,
with a direct Metal renderer and optional native mods. No Direct3D or OpenGL translation layer.

Built from the [wowdecomp](https://github.com/SatyPardus/wowdecomp) reconstruction.
Game data is not included; use your own compatible 3.3.5a installation.

[Releases](https://github.com/fmustafayaman/WoWReMac/releases)

## Branches

| Branch | Purpose |
|---|---|
| **`WowRe`** | Default development branch: native client, Metal renderer and mods |
| `WoWReMac-Warden` | Separate Warden / Warmane compatibility work on top of the native client |
| `epoch-native` | Separate Epoch integration; not merged into the default or Warden branch |

Server-specific integrations stay on their own branches. The old `macos` branch is obsolete.

## Warden branch status

This branch executes the server-delivered 32-bit Warden/buck-data code through Unicorn;
it does not fabricate successful checks. Install Unicorn (`brew install unicorn`) and keep
your own matching build-12340 `Wow.exe` with the game data for the emulated client image.

**A 30-minute native Warmane stability window is verified** (2026-10-24): continuous world
connection, advancing frames, normal movement/UI, all captured buck/Warden replies and no
server warning or emulator failure. The formerly failing thread-context challenge also ran live.
The test used the clean installation without optional native mods. This is evidence for that
test window, not a guarantee for every area, feature or future server check.
See [WARDEN.md](WARDEN.md) for the measured results, limitations and ongoing monitoring.

## Requirements

- macOS on Apple Silicon (tested on Apple M4)
- Xcode Command Line Tools and the Metal shader compiler
- CMake and Ninja
- A World of Warcraft **3.3.5a (12340)** game directory

## Build

```sh
git clone https://github.com/fmustafayaman/WoWReMac.git
cd WoWReMac
git switch WoWReMac-Warden

# Build without copying anything into an existing game installation.
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWOWREMAC_GAME_DIR="" -DWOWREMAC_APP_NAME=WoWReMac-Warden
cmake --build build/mac --target Wow LuxShoulderCam AwesomeWotlk CoAVolFog
```

The application is written to `bin/Mac/WoWReMac-Warden.app`.
To deploy after each build, configure `-DWOWREMAC_GAME_DIR="/path/to/World of Warcraft"`.
Keep different branches in separate game directories so their mods and settings do not conflict.

| CMake option | Default | Purpose |
|---|---|---|
| `WOWREMAC_CLIENT_PATCHES` | `ON` | Native client improvements and conveniences |
| `WOWREMAC_MODS` | `ON` | Load native libraries from the game's `Mods/` folder |
| `WOWREMAC_BUILD_MODS` | `ON` | Build the bundled mods |
| `WOWREMAC_GAME_DIR` | empty | Optional deployment directory |
| `WOWREMAC_APP_NAME` | `WoWReMac-Warden` | Deployed application name, without `.app` |

Older build-cache variable names remain compatibility aliases. Private compile-time identifiers
and diagnostic environment variables are retained to avoid breaking existing tools.

## Install and run

Copy `WoWReMac-Warden.app`, the matching `Mods/` files and optional `Interface/AddOns/` files
into the game directory, alongside `Data/`. Set `Data/<locale>/realmlist.wtf` for your server,
then open the application. A release package may include additional installation instructions.

## Native mods

| Mod | Purpose | Settings |
|---|---|---|
| CoAVolFog | Volumetric fog, god rays, local lights, colour grading and FFT water | `/coavolfog` or Option+F7 |
| AwesomeWotlk | API extensions and client settings | `/awesome` |
| LuxShoulderCam | Over-the-shoulder camera | `/lsc` |

Mods are optional. Remove a mod's `.dylib` from `Mods/` to disable it.
See [mod documentation](Docs/Mods/) for the corresponding data and addon files.

## Testing and measuring

- `Tools/autotest.sh <output dir> "world@N,..." [timeout]` captures a bounded test run.
  It stops only its own process and restores the player's `WTF/` directory afterwards.
- `WRATHMODERN_FRAMESTATS=1` records CPU/GPU frame times and presentation statistics.
- `WRATHMODERN_NO_VSYNC=1` allows uncapped performance measurements.
- `COAVOLFOG_GPU_PROFILE=1` records individual fog/water passes.
- `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` enables Metal validation; disable it for timing runs.

Performance and compatibility are measured per scene and server; this project does not claim
that every feature or server integration is complete. Warden and Epoch results belong to their
respective branches, not to the default client.

## Source layout

- `Engine/Source/` — platform, rendering, UI framework, audio and engine modules
- `WoW/Source/` — client networking, world, objects, game UI and databases
- `Platform/Mac/` — macOS application support
- `Mods/` — optional native mods
- `Tools/` — development and regression-test tools
- `ThirdParty/` — vendored dependencies

The reconstruction retains original `// OFFSET:` and `// VERIFY:` comments for comparison
with build 12340. Upstream Windows support remains in the source tree but is not this project's
primary target.

## Acknowledgements

Based on [wowdecomp](https://github.com/SatyPardus/wowdecomp), with thanks to
[binana](https://github.com/thunderbrewhq/binana),
[thunderbrew](https://github.com/thunderbrewhq/thunderbrew),
[whoa](https://github.com/whoahq/whoa), TOMRUS and the WoW modding community.
