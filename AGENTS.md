# AGENTS.md — WrathModern

Native macOS/arm64 client for World of Warcraft 3.3.5a (build 12340), reconstructed from the
decompilation in this repository. The client speaks the real 3.3.5a protocol; it is not a
private-server bot and must never fake a server-side check.

## What this repository is

- `Engine/Source/` — engine and platform layers (`Os`, `Storm`, `Gx`, `FrameScript`, `Event`, ...).
- `WoW/Source/` — the game client (glue/login, realm, world, UI, objects).
- `Platform/Mac/` — macOS support (paths, command line, window, Metal application init).
- `Engine/Source/Gx/CGxDeviceMetal/` — the Metal implementation of the D3D9 device the client
  renders through.
- `Tools/Wardendump/` — offline diagnostics (Warden emulator smoke test, buck-data smoke test,
  captured protocol fixtures).
- `ThirdParty/` — vendored dependencies (Lua 5.1, zlib, unicorn, ...).

The decompiled sources carry `// VERIFY:` / `// OFFSET:` comments. Keep them when editing; they are
how the port stays comparable with the original binary.

## Worktrees and branches (important)

The repository is checked out in two worktrees:

- `../WrathModern` — `warmane-login` branch. Older Warmane work, plus an uncommitted mods/Metal
  experiment (`Mods/`, `ModHost`, `WMModAPI`).
- `../WrathModern-warden` — `grimfall-recovered` branch. **This is where current Warden and
  Warmane buck-data work happens.** Recent commits: character list/world entry, 0x41B handler
  execution, 0x4B9 Lua, 660-byte fingerprint, 192-byte module scan, 84-byte table.

Check both `git status` and `git log` before assuming where the live work is. When continuing
Warden/Warmane work, work in `../WrathModern-warden` and commit there.

## Build

```sh
cmake -S . -B build/mac -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWRATHMODERN_GAME_DIR="/Volumes/Yaman SSD/wow3.3.5a"
ninja -C build/mac WrathModern
```

`WRATHMODERN_GAME_DIR` copies the built `WrathModern.app` into the game directory after linking.
The app must run with the game directory as its working directory; `WoW/Source/Mac/Main.mm`
`chdir`s there itself.

Useful options: `WRATHMODERN_CLIENT_PATCHES` (macOS conveniences, default ON),
`WRATHMODERN_WARDEN_EMULATION` (Unicorn-backed Warden, default ON when Unicorn is installed),
`WRATHMODERN_MODS` (load `Mods/*.dylib`, default ON on macOS).

Diagnostic tools (built by the same configure):

```sh
ninja -C build/mac WardenSmoke BuckSmoke
./Tools/Wardendump/WardenSmoke
./Tools/Wardendump/BuckSmoke --handler Tools/Wardendump/captures/handler-41b.bin \
  Tools/Wardendump/captures/challenge-official-40.bin 5660000
./Tools/Wardendump/BuckSmoke --seq Tools/Wardendump/captures/handler-41b.bin \
  Tools/Wardendump/captures/client-40.bin Tools/Wardendump/captures/client-660.bin 04d30000 \
  "/Volumes/Yaman SSD/wow3.3.5a/Wow.exe"
```

`BuckSmoke --seq` is the important regression: it runs two challenges through **one** emulator
instance exactly as the live client does. A single-challenge test missed the Unicorn translation
cache bug that made the 660-byte challenge reply with the previous code.

## Warden and Warmane buck-data

- Warden module execution: `Engine/Source/Os/Mac/WardenEmu.cpp` (BLL loader + 32-bit Unicorn),
  `WoW/Source/Game/GameClient/Mac/WardenClient.cpp` (client bridge), `WardenSystemExports.h`.
  The emulator runs the module's own code in a Windows-shaped address space; do not fabricate
  check results or hash values.
- Warmane's buck-data code stream: `Engine/Source/Os/Mac/BuckCodeEmu.cpp` executes the x86 code
  the realm/world delivers (0x2E9/0x4B8 segments, 0x41B challenges). `WoW/Source/Net/NetClient/
  BuckData.cpp` is the client half: request tokens, challenge key chain, 0x4B9 Lua, 0x41C/0x41D
  data, module scan.
- 0x41B protocol (verified against the official client's realmhook capture):
  `[nonce][codeLen][code][answer]`, encrypted with the framework's current key (install token
  first, then the previous nonce), rotating right one bit per dword. The handler reads nonce and
  length with `CDataStore::GetInt` (0x401170), runs the code, which reads the trailing answer with
  another `GetInt`; the reply is `[0x41B][0xD0BE63D1][answer]` encoded with the nonce.
- 0x4B9 Lua: the first payload byte is the rolling-XOR seed (`CDataStore::GetByte`, 0x464560);
  decode the rest and pass it to `FrameScript_Execute`.
- 0x41D/0x41C handlers are not installed natively yet; their data is logged. The 84-byte challenge
  returns the 40-byte table at `0x00DD0FD8`, which the 0x41C handler populates.
- When Unicorn writes code from inside a hook, invalidate translations
  (`uc_ctl_remove_cache`) or the previous challenge's code will keep running. This is already done
  in `StoreGetArray`, `RunHandlerChallenge`, `RunChallenge` and `RunDataHandler`.

Read `../WrathModern-warden/WARDEN.md` before changing any of this; it is the running protocol
notebook (Turkish) with commit-level evidence.

## Diagnostics and logs

The client writes under the game directory's `Logs/`:

- `realm-packets.log` — every realm connection packet (sent and received).
- `world-packets.log` — world connection packets (when produced; the same `ClientConnection`
  object serves both phases).
- `buck-data.log` — buck-data requests, challenge replies, Lua scripts, missing bridges.
- `net-events.log` — connect/connected/auth-challenge/disconnect and dropped sends.
- `warden.log`, `warden-traffic.log` — Warden emulator and module I/O.
- `connection.log`, `FrameXML.log`, `GlueXML.log` — glue/login/UI.

The game directory is `/Volumes/Yaman SSD/wow3.3.5a` unless the user says otherwise. Captured
fixtures live in `Tools/Wardendump/captures/`; `wardenrealm.log` in the Wine prefix
(`/Users/mustafayaman/WoWSilicon/drive_c/`) is a capture of the **official** client and is the
ground truth for protocol comparisons.

## Current state (2026-10-19 evening)

- Warmane login, character list and **world entry work** with the native client. The last live
  run (autologin, character `Hendell`) stayed in the world for more than twelve minutes while the
  Warden keepalives continued.
- Buck-data challenges answered in that run: 40, 660, 192, 88, 92 and 84 bytes. The 192 reply is
  the 56-byte single-module scan; the 88/92 replies are memory-read challenges whose pointers
  point at client image/global addresses, so their **contents and lengths differ from the official
  capture** (the server tolerated them but this is the first place to look if a later check
  rejects the session). The 84 reply uses the zeroed `0x00DD0FD8` table because the 0x41C data
  handler is not installed yet.
- The client is launched unattended with `WRATHMODERN_AUTOLOGIN=1`; the current test instance may
  still be running. Kill it before starting another login:
  `pkill -f 'WrathModern.app/Contents/MacOS/WrathModern'`.
- The external-volume TCC prompt is fixed by stable signing with the `WrathModern Dev` identity
  (see below); one approval survives rebuilds.
- Next fidelity steps if needed: run the delivered 0x41C/0x41D data handlers with
  `BuckCodeEmu::RunDataHandler` (`BuckSmoke --data`) so `0x00DD0FD8` holds the real table, and
  reproduce the 88/92 memory-read payloads from the official capture.

## Testing the live server

`WRATHMODERN_AUTOLOGIN=1` makes the client log in by itself and enter the world with its first
character. Credentials are read from `$HOME/.wrathmodern-autologin` (or
`WRATHMODERN_AUTOLOGIN_FILE`), one value per line:

```
account
password
Lordaeron        (optional realm)
```

The file must be user-only (`chmod 600`); never commit it or print the password. The mode uses
`CGlueMgr::LoginServerLogin` rather than a bare `ClientServices::Logon`, because the glue state
drives the version proof and realm list. Progress goes to `Logs/autologin.log`. The process is
left running after the character login is sent, so the session can be observed and stopped with
`pkill -f WrathModern.app/Contents/MacOS/WrathModern`.

When a live session drops, read `buck-data.log` first: a missing bridge appears as
`delivered code called client XXXXXXXX with no bridge`, followed by a failed challenge. Implement
the bridge by identifying the client function at that address (decompiled source or the official
capture), add a fixture under `Tools/Wardendump/captures/`, and reproduce it offline with
`BuckSmoke` before asking for another live run.

### macOS permissions and code signing

The game lives on an external volume; macOS asks for removable-volume access. An ad-hoc signature
changes every build, so the permission would be forgotten each time. `Tools/sign-app.sh` signs both
the built bundle and the copy in the game directory with the stable `WrathModern Dev` identity from
the login keychain; the build runs it automatically. If the identity is missing the script does
nothing. After a signature change, approve the volume prompt once; subsequent rebuilds keep the
grant. The script also removes ExFAT AppleDouble `._*` files before signing, because they are
unsealed content and make `codesign` refuse the bundle.

## Conventions

- The user speaks Turkish; answer in Turkish unless the task is code-only.
- Honesty is a hard rule for this project: no fake Warden results, no canned challenge answers, no
  bypassing server checks. Execute the delivered code and bridge the real client functions.
- Keep the decompiled style (braces, spacing, `VERIFY`/`OFFSET` comments) and add comments that
  explain protocol evidence, not just what the code does.
- Prefer offline reproduction (`BuckSmoke`, `WardenSmoke`, captured fixtures, logs) over asking the
  user to log in repeatedly.
