# LuxShoulderCam — porting specification (Wow.exe 3.3.5a build 12340 → WrathModern macOS/arm64)

Sources examined
- Repo: https://github.com/Stormhand-dev/Lux-Shoulder-Cam @ `c596d30` — contains only the binary
  `LuxShoulderCam.dll` (no C++ source), the addon (`Addon/LuxShoulderCam/`), an older addon copy
  (`LuxShoulderCam/LuxShoulderCam.lua`, 339 lines, lacks the druid-only form logic) and README.
- Installed: `/Volumes/Yaman SSD/WoW_Wrath/mods/LuxShoulderCam.dll` — **SHA-256 identical** to the repo DLL
  (`5aeeebd3…fa7`, PE32 i386, compiled `Aug 9 2026 00:09:01`, string `Build: %s %s`).
  Installed addon `Interface/AddOns/LuxShoulderCam/*` is byte-identical to repo `Addon/LuxShoulderCam/*`.
- Installed config `/Volumes/Yaman SSD/WoW_Wrath/LuxShoulderCam.cfg` = `height=0.000000\nhorizontal=0.000000`.
- Everything below was recovered by disassembling the DLL with radare2 (no source exists) — marked
  **[V]** verified from disassembly / decompiled source, **[I]** inferred.

Loader: README says it is loaded by Lexara (`Lexara/src/dllmain.cpp` does `LoadLibraryA("LuxShoulderCam.dll")`
after its own init). In Reforged it lives in `mods/` and is loaded by the Reforged DLL loader (`dlls.txt`).
Dependencies: MinHook 1.3.4 (MIT, statically linked; code at 0x10002620–0x10003FFF), MSVC CRT, d3d9.dll
(only to build a dummy device and read its vtable). Licence of the mod itself: README badge says MIT.

---

## 1. Hooks / patches installed

### 1.1 Camera target offset — inline patch inside `CGCamera::CalcTargetCamera` [V]

| | |
|---|---|
| Patch address | `0x006070CB` (6 bytes, `D9 05 70 16 9F 00` = `fld dword [0x009F1670]`, the constant `-500.0f`) |
| Containing function | `CGCamera::CalcTargetCamera(CGObject_C* target, uint32_t time)`, OFFSET `0x00606F90`, `WoW/Source/UIUtil/Camera.cpp:2324` |
| Exact source point | immediately **after** `C3Vector safePos = this->GetSafeWorldPos(target);` (Camera.cpp:2366) and **before** `float flightLower = -500.0f;` (Camera.cpp:2367). In the binary: `call 0x603090` (GetSafeWorldPos, result in `[ebp-0x34..-0x2C]` = safePos.x,y,z) returns at 0x6070CB. |
| Patch | `E9 <rel32 to trampoline>` + `90`. Check-before-patch: byte at 0x6070CB must be `D9` and 0x6070CC must be `05`; the original operand dword at 0x6070CD (`0x009F1670`) is copied into the trampoline. Patch status byte (`E9` = applied, `D9` = original) is what Lua command 2 reports. |
| Trampoline (VirtualAlloc 0x100 RWX, 0x67 bytes) | see below |

Trampoline semantics (decoded byte-by-byte, `esi` = `this` CGCamera*, `ebp` = CalcTargetCamera frame):
```
capCamX   = this->m_position.x      // [esi+0x08]  CSimpleCamera::m_position (CSimpleCamera.h:17)
capCamY   = this->m_position.y      // [esi+0x0C]
capCamZ   = this->m_position.z      // [esi+0x10]   (captured, unused)
capSafeY  = safePos.y               // [ebp-0x30]   value BEFORE the offset is added
capSafeX  = safePos.x               // [ebp-0x34]
safePos.z += appliedHeight          // g_b8
safePos.y += appliedHoriz * kY      // g_bc * g_dirY (DLL .data 0x10007000, initial 1.0f)
safePos.x += appliedHoriz * kX      // g_bc * g_dirX (0x100072C4)
fld [0x009F1670]; jmp 0x006070D1    // re-executes the displaced instruction
```
Because CalcTargetCamera later derives the camera eye from this target (`base - Forward()*m_distance`,
collision, etc.), shifting the target shifts the whole orbit — that *is* the shoulder cam. Camera
collision (`CollideCameraWithWorld`) runs against the shifted target, so walls still stop the camera.

**Port:** add a host hook at Camera.cpp:2366/2367:
`ModsCameraTargetHook(CGCamera* camera, CGObject_C* target, C3Vector& safePos)`; the mod captures
`camera->m_position` and the unmodified `safePos`, then adds its offset. No machine-code patching.

### 1.2 `IDirect3DDevice9::BeginScene` detour (per-frame tick) [V]
- Creates `Direct3DCreate9(32)` + a dummy device on a hidden `"STATIC"` window, reads vtable slot
  `+0xA4` (index 41, BeginScene), MinHooks it (5 attempts, 1.5 s apart). Detour `0x10002090`.
- Purpose: a once-per-frame callback on the render thread. **Port:** host "frame begin" callback
  (e.g. before `CGWorldFrame::OnWorldUpdate` or at the start of `CGWorldFrame::RenderWorld`,
  `WoW/Source/Ui/WorldFrame.cpp:2103`); nothing D3D-specific is needed.

Per-frame logic (detour body, verified):
1. Direction update (sets `kX,kY` = unit vector **perpendicular-left** of the camera→player direction):
   - If `capCamX != 0 || capCamY != 0` and the active player position `P` is available:
     - `d = (P.x - capCamX, P.y - capCamY)`, `dist = |d|`
     - `d2 = |(capSafeX - capCamX, capSafeY - capCamY)|`
     - if `d2 < 0.7` → **first-person lock**: `locked = true` (offsets forced to 0, SET returns -88888)
     - else if `2*|horizontal| + 1.0 < dist` → `kY = d.x/dist`, `kX = -d.y/dist` (otherwise keep last k)
   - else (no camera captured yet) and player facing `f` available → `kY = cos f`, `kX = -sin f`.
   - Note: `(kX,kY) = (-dy, dx)/|d|` is the forward vector rotated +90° about +Z (WoW: x north, y west,
     z up), i.e. **left** of the view direction. Positive `horizontal` therefore moves the orbit target
     (and camera) to the camera's left; the addon labels it "+ Right" (the character then sits right of
     centre). Port the math exactly; do not "fix" the sign.
2. `appliedHeight = locked ? 0 : height`; `appliedHoriz = locked ? 0 : horizontal`; `g_locked = locked`.
3. Lua (re)registration watchdog keyed on `FrameScript s_context` (`lua_State*` at `0x00D3F78C`,
   `Engine/Source/FrameScript/FrameScript.cpp:23`):
   - state changed (UI reload / login) → wait 30 frames → register `LSC` + print "Active -- /lsc to open panel"
     (or "Failed: no code cave found.").
   - same state → every 180 frames silently re-register and re-apply the 0x6070CB patch if missing.
   - drains a 16-entry ring of queued chat messages (`|cFF<color>[LuxShoulderCam]|r <text>`) through
     `FrameScript_Execute(msg, "CH335", 0)`.
   **Port:** register once from a host "Lua state created" callback (FrameScript_Initialize) — no polling.

### 1.3 Lua C-function code cave [V]
- Scans Wow.exe sections with `IMAGE_SCN_MEM_EXECUTE` from section start+0x10000 to 75 % of its size for
  24 consecutive `CC`/`90` bytes, writes `E9 <rel32 to 0x10001890>` at cave+8, and registers the cave address
  as the Lua C function. Reason: the client validates every C function pointer is inside Wow.exe `.text`.
- In the port the same check exists: `OsValidateFunctionPointer` (`Engine/Source/Os/Mac/System.mm:211`,
  called from `Engine/Source/lua-5.1/src/ldo.c:127,220,350,423` and CVar callbacks
  `Engine/Source/ConsoleBase/ConsoleVar.cpp:392,437,461`). A dylib function would be fatal
  ("Invalid function pointer"). **The host must provide Lua registration through trampolines that live in
  the main executable** (see ModAPIv2 R3). Also `WoW/Source/Mac/BotCheck.cpp:31` inspects that function — do
  not patch it.

## 2. Client globals / functions used

| Address | Use | Decompiled symbol |
|---|---|---|
| `0x0084E030` | `lua_tonumber(L, idx)` | `Engine/Source/lua-5.1/src/lapi.c:373` |
| `0x0084E0E0` | `lua_tolstring` (imported, unused in the handler) | `lapi.c:409` |
| `0x0084E2A0` | `lua_pushnumber` | `lapi.c:507` |
| `0x0084E400` | `lua_pushcclosure(L, cave, 0)` | `lapi.c:576` |
| `0x0084E900` | `lua_setfield(L, LUA_GLOBALSINDEX(-10002), "LSC")` | `lapi.c:781` |
| `0x00819210` | `FrameScript_Execute(src, "CH335", 0)` | `Engine/Source/FrameScript/FrameScript.cpp:952` |
| `0x00D3F78C` | current `lua_State*` | `FrameScript.cpp:23` `s_context` |
| `0x00C79CE0` → `+0x2ED0` | `g_clientConnection->m_objMgr` | `ClientServices.cpp:153`, `Net/NetClient/NetClient.h:106` |
| objMgr `+0xC0/+0xC4` | active player GUID | `ObjectMgr::m_playerGuid` (`ObjectMgrClient.h:52`) |
| objMgr `+0xAC`, obj `+0x3C`, obj `+0x30` | walks visible-object list (≤5000) comparing obj GUID | `ObjectMgr::m_visibleObjects` (0xA4), `CGObject_C::m_link` (0x38), hash key GUID (0x30). Port: `ClntObjMgrGetActivePlayerObj()` (`ObjectMgrClient.cpp:1763`) |
| unit `+0x798/+0x79C/+0x7A0` | player position | `CGUnit_C::m_movementData` (0x788, `Unit_C.h:245`) → `CPassenger::m_position` (0x10, `Passenger.h:18`) |
| unit `+0x7A8` | player facing | `CPassenger::m_facing` (0x20, `Passenger.h:20`) |
| camera `+0x08..+0x10` | camera eye position | `CSimpleCamera::m_position` (`CSimpleCamera.h:17`) |
| `0x009F1670` | float `-500.0f` (displaced operand) | `flightLower` init, Camera.cpp:2367 |

## 3. Lua API, files, addon contract

### Global `LSC(cmd, arg)` (numeric protocol, return is always one number) [V]
`cmd = lua_tonumber(L,1)` truncated to int; `arg = lua_tonumber(L,2)` (float). A Lua error inside
tonumber is caught (SEH) → returns `-99999`.

| cmd | Addon name | Behaviour / return |
|---|---|---|
| 1 | PING | `1` |
| 2 | PATCH_STATUS | `1` patch applied, `0` original bytes, `-1` otherwise |
| 3 | GET_F | current `height` |
| 4 | GET_FACING | player facing (radians) or `-999` |
| 10 | GET_HEIGHT | `height` |
| 11 | SET_HEIGHT | if locked → `-88888`; else `height = arg` (**no clamp in DLL**), save cfg, return arg |
| 12 | GET_HORIZ | `horizontal` |
| 13 | SET_HORIZ | if locked → `-88888`; else `horizontal = clamp(arg, -1, 1)`, save cfg, return it |
| 14 | RESET | height = horizontal = 0, save cfg, return 0 |
| other | — | 0 |

(README's ±2 horizontal limit is wrong; DLL and addon clamp to ±1; height is clamped only by the addon to ±1.)

### Config file `<game dir>/LuxShoulderCam.cfg` [V]
Text, two lines, read at DLL attach with `sscanf("height=%f")` / `("horizontal=%f")` (128-byte lines),
written on every SET/RESET with `"height=%f\n" "horizontal=%f\n"`. Path = directory of the **process
executable** (`GetModuleFileNameA(NULL)`) + `\LuxShoulderCam.cfg`. Port: `host.gameDirectory`.

### Companion addon (unmodified) expectations
- `LuxShoulderCam.toc` (Interface 30300, `SavedVariablesPerCharacter: LSC_Settings`), `.xml` panel
  (`lscFrame`), `Bindings.xml` (header `LUXSHOULDERCAM`: LSCTOGGLE, LSCRAISE, LSCLOWER, LSCRIGHT, LSCLEFT, LSCRESET).
- Detection: `type(LSC) == "function"` (`dll_ok`). Must exist by PLAYER_ENTERING_WORLD + 30 s at the latest
  (the addon polls every frame for 30 s and then calls `apply_saved()` → `LSC(11,h)`, `LSC(13,x)`).
- Sentinels: `-99999` error, `-88888` locked; any other number is accepted as the stored value.
- Slash commands are defined by the addon (`/lsc`, `/luxshouldercamp`; `reload`, `status`, `reset`).
- Per-form heights: addon polls `GetShapeshiftForm()` each frame (druids only) and sends SET_HEIGHT after 0.1 s.
- Chat messages the DLL prints: green "Active  --  /lsc to open panel", red "Failed: no code cave found.",
  orange hook failures. The port should print the "Active" line once per Lua state.

## 4. Rendering
None. The D3D9 device is only used as a per-frame tick. No resources, shaders or passes.

## 5. Config/data
Only `LuxShoulderCam.cfg` (above). Defaults 0/0.

## 6. Third-party
MinHook 1.3.4 (BSD-2/MIT-style) — not needed in the port.

## 7. macOS porting plan
Pure logic (port as is, ~150 lines): LSC command dispatcher, cfg read/write, per-frame direction/lock math,
offset application.
Needs host API:
1. `OnCameraTarget(CGCamera*, CGObject_C* target, C3Vector* safePos)` at Camera.cpp:2366→2367 (R5 in ModAPIv2).
2. Per-frame callback before the camera update (R1 `WM_HOOK_FRAME_BEGIN`, or simply do the direction
   math inside the camera hook — it has both `camera->m_position` and the player via the target).
3. Lua C function registration that survives `OsValidateFunctionPointer` and re-registers on every new
   Lua state (R3).
4. Read access to active player position/facing (R4: `ClntObjMgrGetActivePlayerObj`, `GetPosition()`,
   `GetFacing()` — or pass `target` in the camera hook).
5. `gameDirectory` for the cfg (exists in v1).
No Metal work. Risks: (a) the hook must see `safePos` before the flight-bounds clamp exactly as the x86
patch does; (b) the direction uses the *previous frame's* camera eye and *unshifted* target — keep that to
avoid feedback; (c) `-88888`/`-99999` sentinels must be returned as Lua numbers; (d) arm64 float math vs x87
80-bit intermediates — differences are sub-millimetre, acceptable.
