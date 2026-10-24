# Epoch — native macOS transport

## Isolation / scope

Development lives on **`epoch-native`**, worktree `~/Projects/WrathModern-Epoch`, based on
`WowRe` commit `33b67424`. The original `~/Projects/WrathModern-WowRe` checkout stays on
`WowRe`; its pre-existing HDR/render/map changes were not copied, committed, or changed.
Epoch work uses its own `build/epoch` and `bin/Mac`; no original build/deployment target is invoked.

This ports the **connection transport**, not a Windows DLL loader. The existing native
Metal client reads Epoch's game assets. It does not execute `Wow.exe`, `EpochConnection.dll`,
`libDllLdr.dll`, `libSiliconPatch.dll`, DXVK or CrossOver. The two executables/DLLs remain
untouched. Native SRP/world authentication and the OSX/x86 platform identity remain unchanged;
only the wire build field is selected as 12341 for sockets to our current Epoch listeners.
Server acceptance, anti-cheat compatibility and complete custom gameplay are separate checks;
WebSocket upgrade success alone does **not** establish them. No account login is automated by this feature.

## Observed protocol and implementation

The supplied `EpochConnection.dll` is a 160768-byte x86 PE:

```
SHA-256 9af04f7afd21b0bc93860d66e6ccdc96ccf1b039ea31871119a3deaa271352c3
```

Static DLL observations (Hopper/REA, not original source or a runtime trace):

- Its hostname hook recognizes `game.project-epoch.org` / final-dot variant, case-insensitively;
  the connect hook redirects only port 3724. It creates two ephemeral loopback listeners.
  Evidence: `ev_f120cc25365b5619612688f0c9a49b34f6bc041a29dfa642507eb83ee950e0c4`,
  `ev_942eecc050a4ec9670db1fb3ad0b5f61a68a9080c6e89b81374a337336324961`,
  `ev_ad294ce7431e92fe392c40f6c57ac3bf8edb24a3471b3ddcf140354fc5deae2b`.
- Auth: `wss://game.project-epoch.org/relay/auth?worldPort=<local world listener>`.
  World: `wss://game.project-epoch.org/relay/world`. Binary WebSocket messages carry TCP bytes.
  Evidence: `ev_bbec445ad77669cb5d601b309de525dc52b22ddf65713841a930cb26383169c4`
  (previous investigation; large pseudocode), and string inventory
  `ev_2181a5ce256ebc327fddd2140fabbf1fc9b315c752e8398ccbfce3485884a090`.
- Its request-header builder uses an embedded bearer configuration, not the player's password.
  The reviewed x86 instruction at VA `0x10002d1e` references a 64-byte ASCII field at
  VA `0x1001b260`, file offset `0x1a660`; a NUL follows it. Evidence:
  `ev_9fd14de82e56b4a2f5058d638e99eae684c33afc3af6aa045b353f2a843d1ba6`.
  **The field value is deliberately absent from source, docs, binaries, fixtures and logs.**

`Platform/Mac/EpochRelayConfig.cpp` imports that field only after verifying the entire DLL's
SHA-256 and length. An unknown/changed DLL fails closed; update the importer only after reviewing
a new revision. There is no string-scanning fallback. The credential is not written to disk.
Use the original DLL next to the native app; this is configuration reading, not DLL execution.

`WowConnection::Connect(host, port)` recognizes only the exact Epoch login host/3724 and routes
it to `EpochRelay::LoginPort()`. Other realms, ports and server addresses are unchanged. The
relay's `worldPort` parameter advertises the local world listener, as in the original DLL.
No forced rewrite of arbitrary world endpoints or selected realm is added. The login and world
auth packet serializers use `EpochRelay::ProtocolBuild(address, port)`: 12341 only for our current
127.0.0.1 auth/world listener ports, stock 12340 otherwise. A server switch or relay shutdown
does not leave a global Epoch build override behind. Neither OS nor processor identity is changed.

`Platform/Mac/EpochRelay.mm` uses BSD loopback sockets and `NSURLSessionWebSocketTask`:

- Fixed production WSS origin, normal system TLS trust, redirects refused, ephemeral session,
  no cookie persistence. No TLS-verification disable or plaintext/direct fallback.
- One serial transport queue, no synchronous network wait on the rendering/game thread.
  A maximum of 16 streams; one <=64 KiB send and <=1 MiB receive pending per stream.
  Partial TCP writes and WebSocket fragmentation are handled without assuming game packet boundaries.
- 10-second handshake deadline, WebSocket keepalive, idempotent close, bounded backpressure,
  descriptor closure after both socket-source cancellation handlers, shutdown on normal client exit.
- `Logs/EpochRelay.log` contains fixed diagnostics / numeric errors only, never the credential,
  game packets, account passwords or session keys. TCP/WS closure is surfaced through the existing
  client's disconnect handling.

## Build / install

From **this worktree only**:

```sh
cd ~/Projects/WrathModern-Epoch
cmake -S . -B build/epoch -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWRATHMODERN_APP_NAME=WrathModern-Epoch \
  -DWRATHMODERN_BUNDLE_NAME=WrathModern-Epoch \
  -DWRATHMODERN_BUNDLE_ID=org.wrathmodern.epoch \
  -DWRATHMODERN_GAME_DIR="/Volumes/Yaman SSD/Epoch"
ninja -C build/epoch -j 6 Wow
```

This creates this worktree's `bin/Mac/WrathModern.app` and copies it to
`Epoch/WrathModern-Epoch.app`. It must NOT deploy to `WoW_Wrath`, and it does not replace `Wow.exe`.
Keep the complete Epoch data/patches and `EpochConnection.dll` next to the app. The existing
`Data/enUS/realmlist.wtf` should contain `SET realmlist "game.project-epoch.org"`.
Do not change it to localhost or run the Windows launcher concurrently for the native test.
Open the native app and log in manually with the intended Epoch account.

Windows `mods/*.dll` / `dlls.txt` do not apply to the native client. Optional native graphical
mods must be built against this worktree before installation; they are not needed for transport testing.

## Downloading / disconnect investigation (2026-10-23)

The user's first manual login reached `LOGIN_STATE_DOWNLOADFILE` directly from
`LOGIN_STATE_AUTHENTICATING`, then disconnected. No SRP/version-proof phase was reached.
The transport had opened successfully; extending its timeouts would not fix this response.

The supplied Epoch EXE's **PE file version is 3.3.5.12340, but both authentication packets
advertise build 12341**. Static instructions:

- Grunt login, VA `0x008ca5ec` (file offset `0x4c99ec`):
  `mov word [ebp+var_C], 0x3035`. Evidence:
  `ev_6bbeb4cf2cec230aa2677e1296e1832629856f9e4c3f735436a0bbfdda4ce060`.
- World auth, VA `0x004648b2`: `push 0x3035`. Evidence:
  `ev_19d503c45dfcd4897e8757da2b492e6268503f39c8eb08661d9e9993f7ee3091`.
- `0x3035` is 12341, not the native base client's hardcoded 12340.

Two bounded, unauthenticated live probes used the intended Epoch account **identifier** from
that game's Config.wtf; no password, SRP proof or session key was read or sent. Both kept
OSX/x86 and the other challenge fields unchanged:

| Build | Initial response | Interpretation |
|---|---|---|
| 12340 | opcode 0, protocol 0, result 10 | `GRUNT_RESULT_DOWNLOAD_FILE` |
| 12341 | opcode 0, protocol 0, result 0 | Initial metadata accepted; SRP challenge can follow |

Raw credential/account data is not stored in the probe report:
`build/tests/epoch/login-profile-probe.json`. This establishes the cause of the initial
Downloading state, **not successful account authentication**. Both native auth serializers
now select the scoped Epoch build. The prior native app is backed up under
`Epoch/.wrathmodern-backups/WrathModern-Epoch-before-build12341-*.app`.

## Tests

```sh
# Offline, fake credentials, temporary logs and local WebSocket server; no pip dependencies:
python3 Tools/MacDev/test-epoch-relay.py --sanitize \
  --dll "/Volumes/Yaman SSD/Epoch/EpochConnection.dll"

# Explicit network probe: real auth + world TLS/WS upgrades, NO game packets or account login:
python3 Tools/MacDev/test-epoch-relay.py --sanitize \
  --dll "/Volumes/Yaman SSD/Epoch/EpochConnection.dll" --probe
```

Tests cover exact host scoping, missing/changed/malformed/FIFO/symlink DLL rejection, SHA-256
import, scoped login/world build selection and reset on shutdown, byte-exact multi-megabyte
full-duplex transport, binary NULs, empty/fragmented messages,
server-first bytes, stream cancellation/listener cleanup/restart, non-binary/oversize messages,
401, redirect refusal, handshake timeout and untrusted TLS rejection before credential transmission.
The testing-only origin override accepts loopback only and is not compiled into the game.

### Validation status

- Native standalone transport with ASan/UBSan: PASS for the offline transport/error cases.
- Actual Epoch auth and world endpoints: both TLS/WebSocket upgrades PASS, without game login.
- Full native ARM64 game build: PASS (1451 build steps, signed bundle). Existing Xvid static
  library deployment-target warnings are present; they did not prevent this machine's smoke test.
- Untrusted self-signed TLS: PASS, connection rejected before an HTTP credential was transmitted.
- Isolated Epoch data/login-screen smoke: PASS with Metal API + GPU validation enabled, no
  validation errors in the process log; `glue@8` and `glue@16` screenshots show Epoch's custom
  login UI. Exit 0; no game/account login was attempted. Artifacts:
  `build/autotest/epoch-glue-h81xus1n/{smoke-result.json,process.log,Logs/autotest.log,Screenshots/}`.
  Original Epoch `WTF/Config.wtf`, realmlist, Wow.exe and DLL hashes were unchanged.
- Installed: `/Volumes/Yaman SSD/Epoch/WrathModern-Epoch.app`, native ARM64, bundle ID
  `org.wrathmodern.epoch`, code signature verified. It is separate from both `Wow.exe` and
  the original project's `WoW_Wrath/WrathModern-WowRe.app`.
- First manual account attempt failed at the initial build check (Downloading); the scoped
  12341 fix has been rebuilt, installed and code-signature verified. Offline ASan/UBSan tests
  including both channel profiles / other-server isolation / shutdown reset PASS.
- Successful account authentication, character list, world entry, custom gameplay: **not yet
  verified after the fix**; requires a new manual test. No passwords or saved credentials
  from other servers or branches are reused.
