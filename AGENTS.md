# AGENTS.md — WoWReMac / Warden

Native macOS/arm64 client for World of Warcraft 3.3.5a (build 12340), reconstructed from the
decompilation in this repository. The client speaks the real 3.3.5a protocol; it is not a
private-server bot and must never fake a server-side check.

## What this repository is

- `Engine/Source/` — engine and platform layers (`Os`, `Storm`, `Gx`, `FrameScript`, `Event`, ...).
- `WoW/Source/` — the game client (glue/login, realm, world, UI, objects).
- `Platform/Mac/` — macOS support (paths, command line, window, Metal application init).
- `Engine/Source/Gx/CGxDeviceMetal/` — the native Metal backend inherited from `WowRe`;
  the old D3D9 compatibility device has been removed.
- `Tools/Wardendump/` — offline diagnostics (Warden emulator smoke test, buck-data smoke test,
  captured protocol fixtures).
- `ThirdParty/` — vendored dependencies (Lua 5.1, zlib, unicorn, ...).

The decompiled sources carry `// VERIFY:` / `// OFFSET:` comments. Keep them when editing; they are
how the port stays comparable with the original binary.

## Worktrees and branches (important)

- `../WrathModern-WowRe` — `WowRe`, the **main development/base branch** (native Metal,
  client patches and mods). Do not move Warden/server-specific changes onto this branch.
- `../WrathModern-warden` — `WoWReMac-Warden`, **Warden and Warmane buck-data on top of `WowRe`**.
  This branch was rebased onto `WowRe` at `3c7f82c0` on 2026-10-24; it tracks the local
  `WowRe` branch with rebase enabled. Keep it current with `git rebase WowRe`.
- `../WrathModern-Epoch` — `epoch-native`, **separate Epoch integration**. Do not copy its
  relay, authentication changes or uncommitted work into either `WowRe` or the Warden branch.
- `../WrathModern` — `warmane-login`, older work and an uncommitted mods/Metal experiment.
- Other feature worktrees may exist; use `git worktree list` for the complete list.

Check `git status` and `git log` in the relevant worktrees before changing anything. Keep their
uncommitted changes separate. Continue Warden/Warmane work in `../WrathModern-warden` and commit
there. `main` is not the development base. The external `upstream` remote remains the original
`wowdecomp` repository; `WowRe` is the local base branch, not a replacement remote.
The GitHub project is `fmustafayaman/WoWReMac`, with `WowRe` as its default branch.
The obsolete `macos` branch was removed; existing worktree directory names were kept intact.

## Build

```sh
cmake -S . -B build/mac -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWOWREMAC_GAME_DIR="" \
  -DWOWREMAC_APP_NAME=WoWReMac-Warden
cmake --build build/mac --target Wow LuxShoulderCam AwesomeWotlk CoAVolFog
```

An empty `WOWREMAC_GAME_DIR` builds without deploying. To deploy explicitly, set it to
`/Volumes/Yaman SSD/Warmane`; the app is copied as `WoWReMac-Warden.app`. Do not overwrite
`WoWReMac.app`, an Epoch build or their matching mods while testing this branch. Mods share the game's
`Mods/` folder, so use an isolated game directory for separate branch builds when needed.
The app must run with the game directory as its working directory; `WoW/Source/Mac/Main.mm`
`chdir`s there itself.

Useful options: `WOWREMAC_CLIENT_PATCHES` (macOS conveniences, default ON),
`WOWREMAC_WARDEN_EMULATION` (Unicorn-backed Warden, default ON when Unicorn is installed),
`WOWREMAC_MODS` (load `Mods/*.dylib`, default ON on macOS). Legacy cache variables are
compatibility aliases; runtime diagnostic environment variables retain their existing names.

Diagnostic tools (built by the same configure):

```sh
ninja -C build/mac WardenSmoke BuckSmoke
./Tools/Wardendump/WardenSmoke
./Tools/Wardendump/BuckSmoke --handler Tools/Wardendump/captures/handler-41b.bin \
  Tools/Wardendump/captures/challenge-official-40.bin 5660000
./Tools/Wardendump/BuckSmoke --seq Tools/Wardendump/captures/handler-41b.bin \
  Tools/Wardendump/captures/client-40.bin Tools/Wardendump/captures/client-660.bin 04d30000 \
  "/Volumes/Yaman SSD/Warmane/Wow.exe"
python3 Tools/Wardendump/test-buck-lua.py
python3 Tools/Wardendump/test-buck-framework.py
python3 Tools/Wardendump/test-client-image-runtime.py
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
- 0x4B9 Lua: the first payload byte is the rolling-XOR seed (`CDataStore::GetByte`, 0x464560).
  Decode **all** NUL-terminated chunks and execute each in order; the key advances across each
  NUL and is never reset between chunks. The captured 1808-byte packet contains five chunks.
- 0x2E9 bootstrap and 0x4B8 installers now execute their actual x86 code in one persistent
  machine. They allocate/register handlers and produce request packets themselves; do not
  restore opcode scanning, fabricated tokens or captured module lists.
- The 0x41D content hook edits real native DBC rows via the original 32-bit layouts; its
  FrameXML hook runs at the original ExecuteFile call site. The captured version-200 stream's
  eight databases are supported. Unknown schemas/version-reset operations fail explicitly.
- `ClientImageRuntime.h` derives the original `.text` startup-cache values from the PE in both
  emulators. Warden's own memory checks read these too: seeding only BuckCodeEmu was wrong.
- Warden executes with a five-second wall-time bound and no instruction-count hook by default.
  `WARDEN_STEP_LIMIT` and `WARDEN_TRACE_BLOCKS=1` are diagnostic options, not live defaults;
  tracing every instruction/block made real ADT checks stall rendering. Never suppress checks.
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
- `unhandled-packets.log` — complete packets without a registered handler.
- `warden.log`, `warden-traffic.log` — Warden emulator and module I/O.
- `connection.log`, `FrameXML.log`, `GlueXML.log` — glue/login/UI.
- `session-health.log` — optional native world observer (`WRATHMODERN_SESSION_HEALTH=1`, also
  armed by autologin): PID, connection, active-player GUID/position, actual presented frames,
  event-loop gaps and uninterrupted ready time. Process uptime alone is not proof of play.

The Warden test directory is `/Volumes/Yaman SSD/Warmane`, the user's clean 3.3.5a installation.
Do not modify the separate `/Volumes/Yaman SSD/WoW_Wrath` development installation or its mods.
Paths in historical captures are evidence, not current deployment paths. Captured
fixtures live in `Tools/Wardendump/captures/`; `wardenrealm.log` in the Wine prefix
(`/Users/mustafayaman/WoWSilicon/drive_c/`) is a capture of the **official** client and is the
ground truth for protocol comparisons.

## Current state (2026-10-24)

- Clean Warmane installation: account authentication, character list and native world entry
  work with `Maheta` in Camp Narache. Older runs disconnected or received an unauthorized-
  software warning. Those were failed tests, regardless of green offline replay results.
- The warning run contained a Warden read of `0x00D415B8+8`; its response was eight zero
  bytes because only BuckCodeEmu initialized `.text` bounds. The shared PE-derived startup
  initialization now fixes both. A subsequent uninterrupted 12-minute run had no warning.
- Full delivered framework/install/content execution and native DBC/FrameXML hooks are in
  place. Toolhelp reports only the PE image actually mapped into the guest, not a captured
  Wine DLL inventory. `GetDiskFreeSpaceExA` now queries the backing filesystem (the IAT slot
  was previously misidentified as FILETIME).
- Offline regressions: 6 Lua, 12 framework/content/API/thread, 5 shared PE-runtime
  and 8 session-verifier tests. Instrumented
  and bounded-JIT Warden replays produce identical 14 responses; removing the diagnostic
  instruction counter reduced a captured replay from 12.69 s to 1.92 s (6.62x).
- The bounded-JIT 30-minute live run **failed at 1210.222 seconds**: 9 world buck
  challenges arrived but only 8 were answered. The 288-byte challenge called client
  `0x006B3856`, which was an unresolved GetCurrentThreadId import-name RVA. Logs
  are preserved under the run's `failure-20m/`.
  The run had minimum interval FPS 56 and a maximum event-loop gap of 401 ms before
  this compatibility failure; that run did **not** achieve the objective. PID 18078
  is no longer running. The captured 288-byte challenge now runs offline: real guest
  worker threads and GetThreadContext read the saved Unicorn debug registers and
  produce a 36-byte reply. Nonzero DR0/DR3 and failure cases have regression coverage.
  No canned "clean context" is allowed. A later native run now verifies the objective:
  **1800.869 continuous world seconds**, PID 26418, GUID `060000000080713a`, 105599
  presented frames between samples, 10779 server packets, 13/13 buck and 58/58 Warden
  replies. Both live and immutable-snapshot audits passed with no issues. Real movement,
  character/backpack/world-map UI and the 288-byte guest worker were observed.
  Mean interval FPS 58.95, minimum 51.42; maximum event-loop gap 783 ms. This is not
  an everywhere/always-60-FPS or full-game-content guarantee. The client remains running
  and the read-only observer continues, as requested. Run pointer:
  `/tmp/wowremac-stability-run.path`; private evidence is under
  `$HOME/Library/Logs/WoWReMac-Warden/stability-30m-threads-20261024-atdcyp5p/`.
  The frozen 30-minute `Logs/` and audit JSON must not be overwritten by later monitoring.
  A separate `checkpoint-1h/` snapshot also passed at **3862.186 seconds** with 26/26
  buck and 125/125 Warden replies; same process/player/binary, no issues. The spellbook
  UI was also checked normally. Its snapshot/hash manifest are private alongside the run.
  The active user objective is now **continue observing and fix discovered errors until the
  user says enough**. Passing the earlier 30-minute sub-goal does not complete that ongoing
  monitoring objective; keep the observer running and inspect fresh results.
  `verify-session.py <private-run-directory> --live` continues to enforce >=1800 seconds,
  the same PID/GUID, traffic, movement and no warning/failure; do not relax its gates.
- Remaining compatibility limits: the captured version-200 content's known schemas only;
  unknown content operations fail honestly. No canned check results, token/table overrides
  or module-list matching are allowed.
- Preserve session keys with their traffic **before another login**: each login overwrites
  `Logs/warden-session-key.bin`. Private captures from this test are under
  `$HOME/Library/Logs/WoWReMac-Warden/` (older runs remain under `WrathModern-Warden/`);
  never commit keys or credentials.
- New builds use `Warmane/WoWReMac-Warden.app`; archived runs used the old application name.
  Check the exact process path before starting/stopping it. Never kill another branch's client.
- The existing stable development certificate is retained, keeping its signing identity.
  See `WARDEN.md` for packet times, capture locations and the next offline investigation.

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
`kill <test-pid>` after checking its exact executable path. Do not stop another branch's client.

When a live session drops, read `buck-data.log` first: a missing bridge appears as
`delivered code called client XXXXXXXX with no bridge`, followed by a failed challenge. Implement
the bridge by identifying the client function at that address (decompiled source or the official
capture), add a fixture under `Tools/Wardendump/captures/`, and reproduce it offline with
`BuckSmoke` before asking for another live run.

### macOS permissions and code signing

The game lives on an external volume; macOS asks for removable-volume access. An ad-hoc signature
changes every build, so the permission would be forgotten each time. `Tools/sign-app.sh` signs both
the built bundle and the copy in the game directory with the existing stable development
certificate from the login keychain; the build runs it automatically. If the certificate is
missing the entire bundle receives an ad-hoc signature. After a signature change, approve the volume prompt once; subsequent rebuilds keep the
grant. The script also removes ExFAT AppleDouble `._*` files before signing, because they are
unsealed content and make `codesign` refuse the bundle.

## Conventions

- The user speaks Turkish; answer in Turkish unless the task is code-only.
- Prefer pragmatic scope: optional client features that are infeasible or disproportionately
  expensive may be skipped with a clear, fixed "not supported" message. State the limitation
  rather than spending indefinitely or claiming the feature works. This fallback must not
  fabricate server/Warden check results, suppress server warnings, or count a failed check/session
  as successful; those failures remain explicit and the stability goal remains unfulfilled.
- Honesty is a hard rule for this project: no fake Warden results, no canned challenge answers, no
  bypassing server checks. Execute the delivered code and bridge the real client functions.
- Keep the decompiled style (braces, spacing, `VERIFY`/`OFFSET` comments) and add comments that
  explain protocol evidence, not just what the code does.
- Prefer offline reproduction (`BuckSmoke`, `WardenSmoke`, captured fixtures, logs) over asking the
  user to log in repeatedly.
