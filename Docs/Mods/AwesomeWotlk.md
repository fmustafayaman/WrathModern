# Awesome WotLK (AwesomeWotlkLib.dll + AwesomeCVar addon) — porting specification

Sources examined
- Repo: https://github.com/noname08662/awesome_wotlk @ `849229d` (2026-10-11, "addon version broadcast"), fork of
  FrostAtom/awesome_wotlk. Library: `src/AwesomeWotlkLib/*` (~10.5 k lines), client headers `include/**`
  (addresses declared with `HOOKKIT_HOOK(name, addr, conv, ret, args…)`), hook framework `tools/hookkit`
  (MinHook + asmjit call-site trampolines), patcher `src/AwesomeWotlkPatch` (adds the DLL to Wow.exe's import
  table — not needed on macOS), addon `addons/AwesomeCVar`, docs `docs/readme.md`, `docs/api_reference.md`.
- Installed: `/Volumes/Yaman SSD/WoW_Wrath/mods/AwesomeWotlkLib.dll` (1 897 984 B, compiled **2026-10-07 14:19**,
  SHA-256 `9f2bbdc4…410a`). Release build (no `debugbreak` global). All source CVar/API strings present.
  Version = **v38** (`luaOpenAwesomeWotlk` pushes 38; commit `03e5f35 v38` 2026-10-07 02:09). It predates
  `2b633f5` (keybind registration key fix, 16:24), `9db5eeb` (sub-1px quad expansion / geometry rebuild on CVar
  change, 20:09) and the addon-side `849229d`. **[I]** from compile time; the port should follow HEAD.
- Installed addon `Interface/AddOns/AwesomeCVar` (toc v3.0, locales + `Skins/ElvUI.lua`) = repo addon **before**
  `849229d` (no group/guild version broadcast in `Core.lua`); `Constants.lua` `MOD_VERSION = 38` in both.
- Game dir already has `Cache_AwesomeWotLK/Fonts/` and empty `Fonts_AwesomeWotLK/` → MSDF ran on Windows.

Legend **[V]** verified in source, **[I]** inferred. Port decompiled locations from
`/Users/mustafayaman/Projects/WrathModern-WowRe` (`file:line` = line after the `// OFFSET:` comment).

---

## 0. Global patches at attach (`Entry.cpp onAttach`) [V]
| Address | Write | Decompiled | Purpose / port |
|---|---|---|---|
| 0x00D415B8 / 0x00D415BC | `s_textStart = 1`, `s_textEnd = 0x7FFFFFFF` | `Engine/Source/Os/Win32/System.cpp:51-55` (Mac: `Os/Mac/System.mm s_textStart/s_textEnd`) | disables the "Invalid function pointer" check for Lua C functions and CVar callbacks. **Port: never widen the check; host trampolines (ModAPIv2 R3).** Note `WoW/Source/Mac/BotCheck.cpp` / Warden scan these. |
| 0x00B6AF54 / 0x00B6AF5C | `1` | `CGlueMgr::m_acceptedTOS / m_acceptedEULA` (`WoW/Source/Glue/CGlueMgr.cpp:197,203`) | auto-accept TOS/EULA — optional CVar-free convenience |
Lua global `AwesomeWotlk = 38` (number) is set on every Lua-state open (see §3.1).

## 1. Hooks by feature

hookkit conventions: `HOOKKIT_BIND(x_hook, fn)` = full-function detour (fn may call original);
`HOOKKIT_NAMED_BIND_RAW(site, addr, {jmpback…})` = mid-function call-site trampoline; `patchBytes`/`forceWrite` =
data/code byte patches; `reinstall` = hot swap at runtime when a CVar changes.

### 1.1 Extension infrastructure (`Extensions.cpp`) [V]
| Wow.exe | Original → decompiled | Mod action | Host service needed |
|---|---|---|---|
| 0x007663F0 | `CVar::init` per mod header; decompiled symbol at this OFFSET is `CVar::ArchiveCodeRegisteredOnly` (`Engine/Source/ConsoleBase/ConsoleVar.cpp:202`) — **name disagreement, verify** | after original: register all mod CVars (`CVar::Register` 0x00767FC0) with a C callback thunk + user data | R3 CVar registration at client CVar init |
| 0x00495810 | `PaintScreen` (`Engine/Source/Frame/CSimpleTop.cpp:37`) | before: fire `kOnUpdate` (per-frame UI tick) | R1 frame tick |
| 0x0081B5F0 | `FrameScript_CreateEvents` (`FrameScript.cpp:1609`) | append mod event names to the client's event table | R3 event registration + `FrameScript_SignalEvent` |
| 0x0060BB70 | `Script_GetTokenIDsFromGUID` (`WoW/Source/Ui/ScriptEvents.cpp:864`) | after: append `nameplateN` tokens for a GUID | R6 unit-token extension |
| 0x0060AFAA (→0x0060AD57 resolved / 0x0060AD44 orig) | inside `Script_GetGUIDFromToken` (0x0060ABF0+0x3BA, `ScriptEvents.cpp:99`) | resolve custom tokens (`nameplate1..N`) to GUIDs | R6 |
| 0x0051226D | call site inside `LoadScriptFunctions` (0x005120E0+0x18D, `WoW/Source/Ui/GameUI_Scripts.cpp:6535`) — replaces the call to `registerBNScriptFunctions` 0x00530F60 | call original, then open all mod Lua libraries into the current state | R3 "game Lua state ready" |
| 0x00528010 / 0x00528C30 | `CGGameUI::EnterWorld` / `LeaveWorld` (`WoW/Source/Ui/GameUI.cpp:3674 / 8116`) | before: fire kOnEnter / kOnLeave | R1 world enter/leave |
| `glue::loadGlueXML_site` (0x004DA9AC area) | inside `CGlueMgr::Resume` (0x004DA5F0+0x3BC, `WoW/Source/Glue/CGlueMgr.cpp:680`) | fire `glue::kPostLoad` (used by auto-login) | R1 glue-loaded |
| 0x004E47E5 | inside `CCharacterSelection::UpdateCharacterList` (`WoW/Source/Glue/CharacterSelection.cpp:617`) | fire `glue::kCharEnum` (auto-login picks character) | R1 char-list ready |

### 1.2 MSDF fonts (`MSDF*.cpp`, `D3D.cpp`) [V]
Same algorithm and lineage as Lexara (see `Lexara.md`, read it first); differences:
- Gated by CVar `MSDFMode` (0 off, 1 on, 2 include unsafe fonts) — evaluated in a detour of
  **0x006BE230 `GxuFontInitialize`** (`Engine/Source/Gxu/GxuFontMain.cpp:950`), which the mod **reimplements**:
  creates `g_fontVertexShader[2]`/`g_fontPixelShader` (`Shaders\Vertex|Pixel`, "UI"), attaches the hooks below,
  creates the font index pool (`kFontIndexPoolBytes`/`kFontIndexCount` when MSDF, else 0x1800/0xC00), then
  `FT_Init_FreeType`, `registerFontFaces`, `onWindowsSizeChanged`, `createTextureQuads`.
- Hooks attached when enabled: FreeType 0x00991320/0x00993370/0x009931A0/0x00992610/0x00992780/0x009911A0/
  0x00992DA0/0x00991050/0x00992CB0 (as Lexara); `font_util::initGlyph` 0x006C8CC0 (`IGxuFontGlyphRenderGlyph`,
  `IGxuFontGlyph.cpp:54`); `CGxFont::getOrCreateGlyphEntry` 0x006C3FC0 (`CGxFont::NewCodeDesc`,
  `GxuFontFont.cpp:336`); `allocateFontIndexBuffer_site` 0x006C480C (`InitializeIndexBuff`); `glyphBearingY_site`
  0x006C8C71 (`CalculateYOffset`); **`BATCHEDRENDERFONTDESC::processBatch` 0x006C4AD0 replaced entirely**
  (`GxuFontMiscClasses.cpp:289`; two template variants `processBatch<outlinePass>` selected by `MSDFOutlinePass`,
  hot-swapped with `reinstall`); `CGxString::checkGeometry` 0x006C7480 (`GxuFontString.cpp:578`);
  `initTextLine` 0x006C6CD0 (`:251`); `setValues` **0x006C74D0 `CGxString::Initialize`** (`:601`).
  (No inline bufalloc sites — the replaced RenderBatch handles the dynamic vertex budget itself.)
- Shaders through the mod's **CGxShaderExt** system (detour **0x00684970 `CGxDevice::IShaderLoad`**,
  `Engine/Source/Gx/CGxDevice/CGxDevice.cpp:1673`): when the client loads a shader by (type, name) that the mod
  `define()`d, the mod compiles its HLSL (with `#define`s, e.g. `OUTLINE_PASS`) via D3DCompile and stores the
  bytecode into the `CGxShader` instead of the `.bls` from MPQ; `use_fallback` loads the original `.bls`.
  MSDF constants moved: `c220` = (is3d, outline mode, spread, atlas size), `c221` = face metrics (stem,
  hairline, smallest counter in atlas texels, px/em) — from `MSDFFaceSweep.cpp`; samplers s12-s15 atlases.
  Outline pass (`MSDFOutlinePass=1`): text and outline drawn as two passes so outlines don't overlap
  neighbours (2× vertices).
- Cache `Cache_AwesomeWotLK\Fonts\<locale>\<font>\` (same block/manifest scheme as Lexara `MSDFCache`);
  blacklist dir `Fonts_AwesomeWotLK\` (font files or empty files named `Family` / `Family_Style`).
- `/msdfpregen` (slash `MSDFPREGENCMD`, registered on world enter) opens a Win32 console that pre-generates glyph
  ranges with a CPU limit (`MSDFPregen.cpp`). Port: an in-game progress UI or a separate CLI tool.
- CJK: MSDF used only after pregen of the standard range (≥95 %).

### 1.3 UI rendering fixes (`BugFixes.cpp`) [V]
| Wow.exe | Decompiled | Mod action | CVar |
|---|---|---|---|
| 0x004BEFE9 → 0x004BF01E | inside `CameraSetupScreenProjection` (0x004BEE60+0x189, `Engine/Source/Services/Camera.cpp:82`) | replaces the ortho matrix build without the D3D9 half-pixel offset | `uiHalfPixelFix` (attach/detach live) |
| 0x00483220 (reinstall) | `CSimpleTexture::SetPosition` / `calcQuadVertices` (`Engine/Source/Frame/CSimpleRender.cpp:2285`, userpurge ABI) | widen sub-pixel thin quads when snapping | `uiPixelSnap` |
| 0x00484DF5 (call site) | `CTexture::fetchGxTex` call inside `CSimpleRender::DrawBatch` (0x00484B00+0x2F5, `CSimpleRender.cpp:655`) | intercept texture fetch to set sampler/texel-size constant `c222` for the sampling shader | `uiTextureSampling` |
| 0x00483060 | `CSimpleTexture::Init` (`CSimpleRender.cpp:2217`) — creates UI shaders | after: (re)create UI shaders through CGxShaderExt | — |
| 0x00820809 (6 B) | inside `CM2SceneRender::DrawBatchProj` (0x00820720+0xE9, `Engine/Source/Model2/CM2SceneRender.cpp:496`) | `fld [0x00A393A8]` (50.0f) instead of 2.0f: projected-texture depth range, fixes AoE circles clipping on slopes | — |
| 0x008726F0 / 0x008727E0 | `IOsClipboardGetString` / `IOsClipboardPutString` (`Engine/Source/Os/Win64/OsClipboard.cpp:11/51`) | UTF-8 ↔ UTF-16 clipboard | — (port: Mac clipboard already Unicode; verify) |
Shaders (`BugFixesShaders.h`): VS `mainSnap`/`mainSnapStereo` (c0-c3 transform, `c222` viewport size → snap
to pixel centres) replacing vertex shader "UI"; PS `mainNormal`/`mainDesaturate` (s0 UI texture, `c222`
texel size, box/tent footprint filter, define `TENT`) replacing the UI pixel shaders
(`CGxDevice::ui_pixel_shaders_desc[0..1]`).

### 1.4 Camera (`Camera.cpp`) [V]
| Wow.exe | Decompiled | Mod action |
|---|---|---|
| 0x00607C20 | `CSimpleCamera::CSimpleCamera` (`WoW/Source/UIUtil/CSimpleCamera.cpp:37`) | after: apply `cameraFov` (`fov = π/200·cameraFov`) |
| 0x0082BE60 / 0x00832640 | `CM2Model::CM2Model` / `~CM2Model` (`Engine/Source/Model2/M2Model.cpp:3730/6574`) | per-model extension data (fade state) |
| 0x008216C0 / 0x00821850 | `CM2Scene` ctor/dtor (`M2Scene.cpp:1601/1629`) | per-scene fade list |
| 0x0081E0F5 → 0x0081E0FB | inside `CM2Scene::EndHitTest` (0x0081DF10+0x1E5, `M2Scene.cpp:698`) | record raycast model hits |
| **cameraIndirectVisibility=1 only:** 0x006060E6 → 0x00606103 | inside `CGCamera::CollideCameraWithWorld` (0x00605D60+0x386, `WoW/Source/UIUtil/Camera.cpp:3620`) | replace the world intersect call so doodads don't stop the camera; obstructing M2s fade to `cameraIndirectAlpha` |
| 0x007A2C60 | `CMap::SetHitTestDebug` (`WoW/Source/WorldClient/Map.cpp:4794`; mod calls it getDoodadHitDetails) | capture hit doodad |
| 0x007A279D (skip 0x007A2943) / 0x007A2A1C (skip 0x007A2A8A) | inside `CMap::VectorIntersectDoodadDefs` (`Map.cpp:1195`) / `VectorIntersectEntitys` (`Map.cpp:1249`) | per-doodad/entity verdict: skip collision for fadeable models |
CVars: `cameraFov` (float 100, 90-150), `cameraIndirectVisibility` (0/1), `cameraIndirectAlpha` (0.6-1),
`showPlayer` (0/1 → active player `scale_ = 0/1`, refreshed on enter/leave world).

### 1.5 Nameplates (`NamePlates*.cpp`, `NamePlateManager.cpp`) [V]
| Wow.exe | Decompiled | Mod action |
|---|---|---|
| 0x00725890 | `CGUnit_C::UnitNameplatesReposition` (`WoW/Source/Object/ObjectClient/Unit_C.cpp:20172`) | **replaced**: the mod's stacking/clamp/freeze/alpha layout manager `kPlates->update(worldFrame)` |
| 0x004F90E2 → 0x004F90EC | inside `CGWorldFrame::OnWorldRender` (0x004F8EA0+0x242, `WoW/Source/Ui/WorldFrame.cpp:1665`; the `m_namePlateFlags & 0x1` reposition branch) | always mark plates dirty |
| 0x00715720 + NOP 0x0072B2D0 (3 B `add esp,8`) | `GetNamePlateScreenPos` (`Unit_C.cpp:20052`) replaced (cleans its own args); caller `CGUnit_C::ShouldShowNamePlate` (0x0072B060+0x270, `:20331`) | distance up to `nameplateDistance`, `nameplatePlacement` height offset (forceWrite of the float pointer at 0x00715739) |
| 0x0071F8F0 | `CGUnit_C::UpdateReaction` (`Unit_C.cpp:18496`) | reaction/colour bookkeeping |
| 0x007271D0 | `CGUnit_C::SetNamePlateFocus` (`:20293`) | mouseover / hitbox (`nameplateHitbox*`, `nameplateMouseMode`) |
| 0x0055E550 | `CGUIBindings::EnableBindingMode` (`WoW/Source/Ui/UIBindings.cpp:924`) | nameplate show/hide binding modes |
| 0x00524BF0 / 0x005241B0 | `CGGameUI::Target` / `ClearTarget` (`GameUI.cpp:2766/2670`) | target-plate alpha, raise |
| 0x0098F790 | `CGNamePlateFrame::CGNamePlateFrame` (`WoW/Source/Ui/NamePlateFrame.cpp:528`) | assign `nameplateN`, fire `NAME_PLATE_CREATED`, add `unit` field + methods |
| 0x00727130 | `CGUnit_C::NamePlateClearAll` (`Unit_C.cpp:20269`) | fire `NAME_PLATE_UNIT_REMOVED` for all |
| 0x009DE370 | `CDataAllocator::wipe` (nameplate pool; **no decompiled OFFSET**, nearest unrelated) | reset pool state |
| 0x0098E9F9 → 0x0098EA27, 0x0098EAA7 → 0x0098EAD2 | inside `CGNamePlateFrame::OnLayerUpdate` (0x0098E9F0, `NamePlateFrame.cpp:240`) | frame level ordering; alpha = mod value (occlusion/non-target/fade) |
| `kTabTargetRangeCapSqAddr` (forceWrite float) | tab-target range² cap | follows `nameplateDistance` |
Line-of-sight occlusion uses `World::Intersect` 0x0077F310 (`TraceLine`). 29 CVars — see §3.3.

### 1.6 Misc (`Misc.cpp`, `MiscCaptureFrame*.h`, `Item/Spell/UnitAPI/Inventory/VoiceChat/CommandLine.cpp`) [V]
| Wow.exe | Decompiled | Mod action |
|---|---|---|
| 0x00564AE0 | `Script_SecureCmdOptionParse` (`WoW/Source/Ui/UIMacros_Scripts.cpp:147`) | adds macro conditionals `[@cursor]`/`[target=cursor]` and `[@playerlocation]` |
| 0x004F66C0 | `CGWorldFrame::OnLayerTrackTerrain` (`WorldFrame.cpp:299`) | track cursor terrain point for `@cursor` |
| 0x007FCC30 | `Spell_C_DestroySpellWorldObject` (cancel pending AoE targeting, `Magic/MagicClient/Spell_C.cpp:1234`) | finish queued cursor cast |
| 0x007E4370 | `ProjectTex::Render2d` (`WoW/Source/WorldClient/shadow.cpp:715`) | interaction-target highlight ring |
| 0x00743BC0 (reinstall) | `CGObject_C::HideHighlightType` (`Object_C.cpp:977`) | `objectHighlightMode` forced loot sparkle |
| 0x006180E0 → 0x006180E5 | inside `PortraitInitialize` (0x00618010+0xD0, `WoW/Source/UIUtil/PortraitButton.cpp:132`) | portrait render target size = `portraitResolution` (creates depth `PortraitDepth` via 0x005EEB70) |
| 0x00619B6A → 0x00619B72 | inside `RenderPortraitTexture` (0x00619580+0x5EA, `PortraitButton.cpp:564`) | viewport size for the bigger RT |
| forceWrite 0x00AC7A40 / 0x00AC7A44 | `s_chatLogNames[2]` (`WoW/Source/Ui/ChatFrame_Scripts.cpp:1473`) | timestamped `Logs\<date> WoWChatLog.txt` / `WoWCombatLog.txt` (`chatLogSessionKey`, `combatLogSessionKey`); sets Lua globals `CHATLOGENABLED`/`COMBATLOGENABLED` strings |
| (frame capture, attached only while a capture is pending) 0x00495410 `CSimpleTop::OnLayerRender` (`CSimpleTop.cpp:895`), 0x00490840 `CSimpleFrame::OnFrameRender` (`CSimpleFrame.cpp:1458`), 0x004858E0 `CRenderBatch::QueueCallback` (`CRenderBatch.cpp:86`), 0x005EBD20 `CGCooldown::OnFrameRender` (`WoW/Source/Ui/CGCooldown.cpp:384`) | re-render one frame subtree offscreen at N× scale |
| login 0x004D8A30 `CGlueMgr::LoginServerLogin` (`CGlueMgr.cpp:337`) | `-login -password -realmlist -realmname` command line auto-login (`CommandLine.cpp`) |
Interaction (`QueueInteract`, `/interact`, keybinding `INTERACTIONKEYBIND` in header `AWESOME_WOTLK_KEYBINDS`):
per-frame (kOnUpdate) nearest/forward-cone search over visible objects within 20 yd
(`interactionMode`, `interactionAngle`), highlight (`interactionHighlight`), then `CGGameUI::OnSpriteRightClick`
style interaction (loot/skin/use). Uses `CGUIBindings::registerBinding` (runtime binding registration).

### 1.7 Client functions/globals called (not hooked) — main ones
ObjectMgr enumerate 0x004D4B30 (`ClntObjMgrEnumVisibleObjects`), `ClntObjMgrObjectPtr` 0x004D4DB0,
`GetGUIDFromToken` 0x0060C1C0, GUID↔hex 0x0074D120/0x0074D0D0, unit reaction/attack/assist
0x007251C0/0x00729A70/0x007293D0, creature rank 0x00718A00, `CGGameUI::GetTarget` (0x00BD07B0 locked target),
`FrameScript_SignalEvent` 0x0081AA00/0x0081AC90, `FrameScript_Execute` 0x00819210, `FrameScript_GetText`
0x00819D40, `CScriptObject::RegisterScriptObject` 0x00819880, `GetRefTable` 0x00488380, ClientDB rows
0x004CFD20/0x004BB1C0, item cache `DBCache<ItemStats>::GetRecord` 0x0067CA30 (`WoW/Source/DB/DBClient/DBCache.h`),
`GetItemIdByName` 0x00709DE0, `GetInvArtById` 0x0070A910, Lua C API 0x0084xxxx (`lapi.c`), `luaL_*`
0x0084F960/0x0084F9F0/0x0084FAB0, Storm SFile/SMem 0x0042xxxx/0x0076xxxx, `OsGetAsyncTimeMs` 0x0086AE20,
window HWND 0x00D41620, `IsInWorld` 0x00BD0792, world frame 0x00B7436C, `CGWorldFrame::GetScreenCoordinates`
0x004F6D20, `World::Intersect` 0x0077F310, `CVar::Lookup` 0x00767460, `CVar::Set` 0x007667B0.
Resolve any further address with the decompiled `// OFFSET:` index (all 22 301 OFFSET comments; e.g.
`grep -rn "OFFSET: 0x00XXXXXX"`).

## 2. (merged into §1.7)

## 3. Lua API, CVars, events, slash commands, addon contract

### 3.1 Globals / functions [V]
- `AwesomeWotlk` (number, **38** for v38). AwesomeCVar `Constants.lua`: `ACVar.MOD_VERSION = 38`;
  `GetVersionStatus()` → `tonumber(_G.AwesomeWotlk)`: nil → `"missing"` ("Awesome WotLK is not loaded" text),
  greater → `"addon_outdated"`, smaller → `"mod_outdated"`, equal → `"ok"`. The port **must** set this global
  (as a number) in every new Lua state before addons' `ADDON_LOADED`, and report 38 (or the version whose
  behaviour it implements).
- `C_NamePlate.GetNamePlates()`, `.GetNamePlateForUnit(unit)`, `.GetNamePlateByGUID(guid)`,
  `.GetNamePlateTokenByGUID(guid)`; nameplate frames get field `unit = "nameplateN"` and methods
  `GetStackingEnabled/SetStackingEnabled/GetOcclusionEnabled/SetOcclusionEnabled`.
- `C_VoiceChat.SpeakText(voiceID, text[, dest=1, rate, volume])`, `.StopSpeakingText()`, `.GetTtsVoices()`,
  `.GetRemoteTtsVoices()`; `C_TTSSettings.GetSpeechRate/GetSpeechVolume/GetSpeechVoiceID/GetVoiceOptionName/
  SetDefaultSettings/SetSpeechRate/SetSpeechVolume/SetVoiceOption/SetVoiceOptionByName/RefreshVoices`.
- `UnitIsControlled`, `UnitIsDisarmed`, `UnitIsSilenced`, `UnitOccupations`, `UnitOwner`, `UnitTokenFromGUID`;
  `GetInventoryItemTransmog(unit, slot)`; `GetItemInfoInstant(item)`; `GetSpellBaseCooldown(spell)`;
  `FlashWindow`, `IsWindowFocused`, `FocusWindow`, `CopyToClipboard(text)`, `QueueInteract([unit])`,
  `CaptureFrame(frame[, size])`. Exact semantics: `docs/api_reference.md` (copied behaviour is normative).
- Unit tokens `nameplate1..N` accepted by every unit API (token registry hooks).
- Strings `CHATLOGENABLED`, `COMBATLOGENABLED` overwritten on world enter.

### 3.2 Events (added to the client event table) [V]
`NAME_PLATE_CREATED(frame)`, `NAME_PLATE_UNIT_ADDED(unit)`, `NAME_PLATE_UNIT_REMOVED(unit)`,
`VOICE_CHAT_TTS_PLAYBACK_STARTED(numConsumers, utteranceID, durationMS=0, destination)`,
`VOICE_CHAT_TTS_PLAYBACK_FINISHED(numConsumers, utteranceID, destination)`,
`VOICE_CHAT_TTS_PLAYBACK_FAILED(status, utteranceID, destination)`, `VOICE_CHAT_TTS_SPEAK_TEXT_UPDATE` (unused),
`VOICE_CHAT_TTS_VOICES_UPDATE`.

### 3.3 CVars (name: type default [min,max]) [V]
Nameplates: `nameplateDistance` f 41 [41,200]; `nameplatePlacement` f 0 [-1,2]; `nameplateMouseMode` i 0 [0,8];
`nameplateBandX` f .7 [.1,1]; `nameplateBandY` f 1 [.1,1.5]; `nameplateHitboxAnchor` i 1 [0,2];
`nameplateHitboxWidthE/HeightE/WidthF/HeightF` f 1 [0,1]; `nameplateRaiseSpeed` f 100 [1,250];
`nameplateLowerSpeed` f 100 [1,250]; `nameplatePullSpeed` f 50 [1,250]; `nameplateRaiseDistance` f 8 [1,20];
`nameplatePullDistance` f .25 [0,.75]; `nameplateOcclusionAlpha` f 1 [-1,1]; `nameplateOcclusionMode` i 0 [0,1];
`nameplateNonTargetAlpha` f .5 [0,1]; `nameplateAlphaSpeed` f .25 [.01,1]; `nameplateInertia` f 1 [0,20];
`nameplateHysteresisDecay` f 1 [.25,30]; `nameplateClampMode` i 0 [0,4]; `nameplateClampModeVOffset` f .01
[0,.05]; `nameplateClampModeHOffset` f .01 [0,.1]; `nameplateClampModeFilter` i 0 [0,3]; `nameplateMouseFreeze`
i 0 [0,2]; `nameplateMouseFreezeGrace` f .15 [0,1]; `nameplateMouseFreezeTime` f .3 [0,2]; `nameplateStacking`
i 0 [-3,3].
Camera: `cameraFov` f 100 [90,150]; `cameraIndirectVisibility` i 0; `cameraIndirectAlpha` f .6 [.6,1]; `showPlayer` i 1.
Interaction: `interactionMode` i 1; `interactionAngle` i 60 [15,160]; `interactionHighlight` i 1.
UI: `uiHalfPixelFix` i 1; `uiTextureSampling` i 1 [0,2]; `uiPixelSnap` i 1.
Fonts: `MSDFMode` i 1 [0,2] (restart); `MSDFOutlinePass` i 1.
Misc: `objectHighlightMode` i 0 [0,2]; `portraitResolution` i 64 [64,2048]; `chatLogSessionKey` i 1;
`combatLogSessionKey` i 1. TTS: `ttsVoice` 1, `ttsSpeed` 0 [-10,10], `ttsVolume` 100 [0,100].
All are registered through `CVar::Register` with a callback (thunk) → mod `on_change(value, changed)`.
Values clamp to [min,max]. Saved in `WTF/Config.wtf` like any client CVar (archived flag **[I]**).

### 3.4 Slash commands / bindings
`/msdfpregen` (`MSDFPREGENCMD`), `/interact` (`INTERACTCMD`, takes macro options), `/fcapture`,`/framecapture`
(Lua in `MiscCaptureFrameLua.h`, executed via `FrameScript_Execute(..., "FrameCapture")`), binding
`INTERACTIONKEYBIND` → `QueueInteract()` under header `AWESOME_WOTLK_KEYBINDS` ("Awesome Wotlk Keybinds").
AwesomeCVar addon: `/awesome` (its own), reads/writes the CVars above with `GetCVar/SetCVar`, uses
`AwesomeWotlk` global for the About/version page, optional ElvUI skin. No other DLL contract.

## 4. Rendering
- No world passes. Changes: UI font shaders (MSDF), UI VS/PS replacement (snap / box-tent sampling), UI
  ortho projection (half-pixel), projected-texture depth constant, portrait RT size, frame capture (offscreen
  `D3DFMT_A8R8G8B8` render targets with up to 8× MSAA, rendered twice on black and white backgrounds to recover
  alpha, `StretchRect` resolve, `GetRenderTargetData` readback in strips, PNG via stb_image_write), interaction
  highlight ring through the client's `ProjectTex::Render2d`.
- Shader constant slots used: c220/c221 (MSDF), c222 (UI snap/sampling), samplers s12-s15. Custom shaders are
  installed into **client `CGxShader` objects** through the `IShaderLoad` hook, so they run inside the client's own
  draw calls — the cleanest model for the port (ModAPIv2 R2b "shader override by name").
- Depth/stencil: none except portrait depth surface and projected-texture constant.

## 5. Config/data files
- `Cache_AwesomeWotLK\Fonts\<locale>\...` (MSDF cache, format as in Lexara.md §5 with `CACHE_DIR` changed),
  `Fonts_AwesomeWotLK\` blacklist, `Screenshots\FrameCapture_<Name>_<YYYYMMDD>_<HHMMSS>_<ms>.png`,
  `Logs\<YYYY-MM-DD-HH.MM.SS> WoWChatLog.txt` / `WoWCombatLog.txt`. No ini.

## 6. Third-party (licences in `licenses/`)
MinHook (BSD-2), asmjit (Zlib), stb (MIT/PD), bcdec (MIT/Unlicense, BC texture decode for capture), FreeType
2.14.3 (FTL), msdfgen (MIT), skia (BSD-3), unordered_dense (MIT). Windows-only: SAPI (TTS), Win32 console
(pregen), D3DCompiler_47. Mod licence: repo `LICENSE` (check; upstream FrostAtom is MIT **[I]**).

## 7. macOS porting plan
Split into independent modules (the original is already modular: `extensions`, `msdf`, `bug_fixes`, `camera`,
`name_plates`, `misc`, `unit_api`, `item`, `inventory`, `spell`, `voice_chat`, `command_line`).
1. **Infrastructure (host, ModAPIv2 R3/R6)**: CVar registration with callbacks, Lua library registration per Lua
   state, event-name registration + `SignalEvent`, unit-token extension, slash commands, key-binding registration,
   per-frame tick, enter/leave world, glue-loaded and char-list callbacks. Everything else depends on it.
2. **Pure Lua/C API (easy, port as is)**: UnitAPI, Item, Inventory, Spell, Misc window functions (map
   FlashWindow → `NSApp requestUserAttention`, IsWindowFocused/FocusWindow → NSWindow key status,
   CopyToClipboard → NSPasteboard), `AwesomeWotlk` global, command-line auto-login (port has
   `WRATHMODERN_AUTOLOGIN`; keep `-login/-password/-realmlist/-realmname` parsing).
3. **Nameplates**: logic portable (NamePlateManager is self-contained); needs host hooks replacing
   `UnitNameplatesReposition`, `GetNamePlateScreenPos`, nameplate ctor, clear-all, OnLayerUpdate level/alpha,
   target/clear-target, `SetNamePlateFocus`, distance constant. Since the port is source, implement these as
   host-side callbacks/overrides (ModAPIv2 R6) rather than byte patches.
4. **Camera**: FOV via CSimpleCamera ctor callback (or direct CVar in the port), indirect visibility needs callbacks
   in `CollideCameraWithWorld`, `VectorIntersectDoodadDefs/Entitys`, `EndHitTest` and model ctor/dtor + per-model
   alpha (ModAPIv2 R5).
5. **UI fixes**: half-pixel fix is a no-op on Metal *if* the port's translator already removes the D3D9 half-pixel
   rule — verify in `ShaderTranslator.cpp` ("Direct3D 9 semantics: rasterization rules"); pixel snap and box/tent
   sampling via shader override (fxc-compiled SM3 bytecode through the D3D9→Metal translator), texture fetch
   callback in `CSimpleRender::DrawBatch`. Projected texture depth constant: one-line host change
   (`CM2SceneRender.cpp:496` area) behind a flag.
6. **MSDF**: as Lexara.md §7; implement once, AwesomeWotlk flavour (CVar-gated, outline pass, blacklist, face
   sweep). Replace `processBatch` with a host hook point "font batch render override".
7. **Frame capture**: needs offscreen UI rendering of a frame subtree — host must expose "render CSimpleFrame
   subtree into target at scale" (or the hooks in §1.6) and D3D9Metal render targets + readback; PNG via stb.
   Low priority.
8. **TTS**: replace SAPI with `AVSpeechSynthesizer` (voices = `AVSpeechSynthesisVoice.speechVoices`), keep FIFO
   queue + events. **Portrait resolution**, **chat log names**, **object highlight**: small host hooks.
9. Auto-accept TOS/EULA: optional.
Risks: (a) volume of client hooks — ~60 sites; each must become a deliberate host hook (no runtime patching on
arm64 of compiled C++ — impossible to do generically); (b) `CVar::init` address/name mismatch to resolve;
(c) nameplate behaviour depends on exact frame-update order (`OnWorldRender` → `UnitNameplatesReposition`);
(d) Warden/anti-cheat: none of this must touch memory Warden hashes (the port's Warden emulator runs on a
pristine image — unaffected); (e) installed DLL is v38-pre-fix: follow HEAD (`2b633f5` binding key fix, `9db5eeb`).
