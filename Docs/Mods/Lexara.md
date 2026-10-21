# Lexara (HD/MSDF font renderer) — porting specification

Sources examined
- Repo: https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5 @ `ad30c8d`
  (`src/dllmain.cpp`, `src/Proxy.cpp`, `src/MSDF.cpp`, `src/font_exact/*`; `source.7z` holds the same
  `src/` tree dated 2026-04-28 plus third_party headers — not extracted beyond listing).
  Three `dllmain*.cpp` variants exist; the built one is `dllmain.cpp` (it `LoadLibraryA("LuxShoulderCam.dll")`).
- Installed: `/Volumes/Yaman SSD/WoW_Wrath/mods/Lexara.dll` (784 384 B, PE32, compiled **2026-10-15**,
  SHA-256 `b524623f…6d72`). Its strings match the repo source: proxy exports
  (DirectInput8Create/GetdfDI*/GetFileVersionInfo*/VerQueryValue*/DirectSound*), the exact
  `MSDFShaders.h` HLSL text, `[MSDF] Initialization committed — HD fonts active`, `LuxShoulderCam.dll`.
  Error string `[AwesomeWotlk] %s Shader Error: %s` shows the code is copied from AwesomeWotlk/Transmorpher.
  Imports `skia.dll` (game root, identical to `awesome/deps/skia/skia.dll`, SHA-256 `ef23add…57d4`) and
  `D3DCOMPILER_47.dll` (runtime HLSL compile). No version number is embedded. **[V]** the binary matches
  the repo source (compile date is later than the last commit; no behavioural strings differ).
- Lineage: "font subsystem of Transmorpher 2.0.0" = an **older copy of AwesomeWotlk's MSDF code**
  (`awesome/src/AwesomeWotlkLib/MSDF*.{h,cpp}`, `D3D.{h,cpp}`). Differences vs AwesomeWotlk (current):
  Lexara is always on (no `MSDFMode` CVar), no `MSDFOutlinePass` (outline drawn in the same pass), no font
  blacklist folder, cache in `%TEMP%\LEXARA\…` instead of `Cache_AwesomeWotLK\Fonts\<locale>\`, no
  `/msdfpregen` command (MSDFPregen.cpp is compiled but nothing calls it — `PREGEN_START_KEY = VK_F11` is
  defined and unused), no face-sweep, CJK locales fall back to the stock renderer.
- **Conflict:** the Reforged install loads both `Lexara.dll` and `AwesomeWotlkLib.dll`; both Detour the
  same FreeType/CGxString functions (chained detours, double MSDF processing guarded only by the
  `0x40000000` string flag). The port should ship **one** MSDF implementation (AwesomeWotlk's, see
  AwesomeWotlk.md §MSDF) and treat Lexara as "MSDF always on, outline in one pass" — recommended decision.

Legend: **[V]** verified in source/binary, **[I]** inferred.

---

## 1. Hooks / patches

All hooks are Microsoft Detours. Two phases: at DLL attach (`FontExact_OnAttach`, `src/font_exact/Entry.cpp`)
it detours the device lifecycle (`D3D::initialize`) and **FreeType `FT_Init_FreeType`**; the real work is
armed inside the FT_Init detour (first call = client font system start, `MSDF.cpp FreeType_InitHk`).

### 1.1 Device lifecycle (D3D.cpp `D3D::initialize`) [V]
| Wow.exe | Original | Decompiled (WrathModern-WowRe) | Mod action |
|---|---|---|---|
| 0x00682CB0 | `CGxDevice::DeviceCreate` | `Engine/Source/Gx/CGxDevice/CGxDevice.cpp:138` | after: grab `IDirect3DDevice9*` at `*(0x00C5DF88)+0x397C`, detour 13 IDirect3DDevice9 vtable methods (Present, BeginScene, EndScene, DrawPrimitive, DrawIndexedPrimitive, SetTexture, SetRenderState, SetVertexShader, SetPixelShader, CreateTexture, SetRenderTarget, Clear, Reset) — Lexara registers **no** callbacks on them (generic infrastructure) — then run onCreate callbacks |
| 0x006843B0 | `CGxDevice::NotifyOnDeviceRestored` | `CGxDevice.cpp:282` | before: recreate managed resources, onRestore callbacks |
| 0x006904D0 | `CGxDeviceD3d::DeviceSetFormat` | `Gx/CGxDeviceD3d/CGxDeviceD3d.cpp:2808` | after: restore resources |
| 0x006903B0 | `CGxDeviceD3d::IDestroyD3d` | `CGxDeviceD3d.cpp:2756` | before: onDestroy (release cached PS/VS, clear all atlases) |
| 0x00690150 | `CGxDeviceD3d::IReleaseD3dResources` | `CGxDeviceD3d.cpp:2657` | before: release D3DPOOL_DEFAULT resources |
| 0x006AA0D0 | `IShaderCreateVertex` | `Gx/CGxDeviceD3d9Ex/CGxDeviceD3d9Ex.cpp:820` | after: if the shader is `g_fontVertexShader` → replace its D3D shader with the MSDF VS |
| 0x006AA070 | `IShaderCreatePixel` | `CGxDeviceD3d9Ex.cpp:801` | after: if `g_fontPixelShader` → replace with MSDF PS |
Globals: `0x00C5DF88 g_theGxDevicePtr` (`CGxDevice.cpp:76`); `0x00C7D2CC g_fontPixelShader`,
`0x00C7D2D0 g_fontVertexShader[2]` (`Engine/Source/Gxu/GxuFontMain.cpp:99-103`, created at
`GxuFontMain.cpp:951-952` from `Shaders\Vertex|Pixel\UI`). The mod writes `ShaderData->pixel_shader /
vertex_shader` (the `IDirect3D*Shader9*` inside `CGxShader`) and `compilation_flags = 1` (= valid).

### 1.2 FreeType replacement (client links FreeType 2.0-era code at 0x0099xxxx) [V]
| Wow.exe | FT function | Detour |
|---|---|---|
| 0x00991320 | `FT_Init_FreeType` | first call arms everything below; returns the mod's **FreeType 2.14.1** library instead of the client's; creates `msdfgen::initializeFreetype()`. CJK locale (zhCN/zhTW/koKR, detected from `Data/<loc>/{locale,patch,base}-<loc>.MPQ`) → call original and do nothing. |
| 0x00993370 | `FT_New_Memory_Face` | open the face with FT 2.14 and `MSDFFont::Register(face, bytes, size)` |
| 0x009931A0 | `FT_New_Face` | returns 1 (error) — file faces disabled |
| 0x00992610 | `FT_Done_Face` | `MSDFFont::Unregister` then FT 2.14 `FT_Done_Face` |
| 0x00992780 | `FT_Set_Pixel_Sizes` | FT 2.14 |
| 0x009911A0 | `FT_Get_Char_Index` | FT 2.14 |
| 0x00992DA0 | `FT_Load_Glyph` | FT 2.14 with flags forced to `FT_LOAD_RENDER | FT_LOAD_NO_HINTING` |
| 0x00991050 | `FT_Get_Kerning` | FT 2.14 |
| 0x00992CB0 | `FT_Done_FreeType` | shut down MSDF + both FT libraries |
Effect: every client `FT_Face` is a modern FreeType object (msdfgen needs ≥2.x outline API).
Port: WrathModern-WowRe links `ThirdParty/freetype` (headers say 2.0) statically, **no OFFSETs**; client
calls are at `Engine/Source/Gxu/GxuFontFace.cpp:94` (`FT_New_Memory_Face`), `:62` (`FT_Done_Face`),
`GxuFontFont.cpp:96,107` (`FT_Set_Pixel_Sizes`), `:255-262` (char index/kerning), `IGxuFontGlyph.cpp:11-13`
(`FT_Load_Glyph`), `:60`. Two options: (a) host upgrades the client to a modern FreeType (recommended; one
library, the mod receives the client's FT_Face directly); (b) host exposes a "face loaded (bytes,size,FT_Face)"
callback at `GxuFontFace.cpp:94` and the mod opens its own modern face from the same bytes keyed by the
client face pointer. Either way the forced `FT_LOAD_RENDER|NO_HINTING` and the bearing tweak (1.3) must be
reproduced only for MSDF faces.

### 1.3 Font glyph/geometry function detours [V]
| Wow.exe | Original | Decompiled | Mod action |
|---|---|---|---|
| 0x006C53A0 | `CGxuFont::RenderBatch` | `CGxStringBatch::RenderBatch`, `Gxu/GxuFontMiscClasses.cpp:551` | after: zero PS/VS constant c23 (disables MSDF path for later UI draws) |
| 0x006C8CC0 | `CGxuFont::RenderGlyph` | `IGxuFontGlyphRenderGlyph`, `Gxu/IGxuFontGlyph.cpp:54` | after, MSDF faces: `GLYPHBITMAPDATA.m_bearingY -= m_verAdv` |
| 0x006C3FC0 | `CGxFont::GetOrCreateGlyphEntry` | `CGxFont::NewCodeDesc`, `GxuFontFont.cpp:336` | after, MSDF faces: store `u0 = 1 + codepoint` (marker), cell index min/max = 0, `m_textureNumber = 0` (all glyphs "on page 0") |
| 0x006C7480 | `CGxString::CheckGeometry` | `GxuFontString.cpp:578` | before: if string high byte has 0x80 and its 7-bit atlas-eviction stamp ≠ font's eviction count → `ClearInstanceData()` (0x006C6B90, `GxuFontString.cpp:191`); accumulate `g_runtimeVBSize += CalculateVertsNeeded(0)` (0x006C63E0, `:142`) |
| 0x006C5E90 | `CGxString::WriteGeometry` | `GxuFontString.cpp:28` | after: bind atlas pages to samplers **12-15** (clamp, linear, no mip), set PS+VS constant **c23** = `{fontPx, outlineMode, SDF_SPREAD=12, ATLAS_SIZE=2048}`; non-MSDF faces get c23 = 0 |
| 0x006C6CD0 | `CGxString::InitializeTextLine` | `GxuFontString.cpp:251` | after: if not yet flagged, push every byte of `m_text` into the prefetch list, set `m_flags |= 0x40000000` |
Helpers called: 0x006C24F0 `CGxFont::GetGlyphBearing` (`GxuFontFont.cpp:122`), 0x006C8080 `FontFaceGetFace`
(`GxuFontFace.cpp:12`), 0x006C0B20 `ScreenToPixelHeight` (= `GetFontEffectiveHeight`, `GxuFontUtil.cpp:207`),
0x006C0B60 `ScreenToPixelWidth` (`:218`), 0x006C48D0 `UnlockVertexPtrAndRender` (= `FlushBuffer`,
`GxuFontMiscClasses.cpp:263`), 0x006C47B0 `BATCHEDRENDERFONTDESC::Initialize` (`:220`), 0x006876D0
`CGxDevice::PoolCreate` (`CGxDevice.cpp:1642`).

### 1.4 Inline mid-function patches (naked asm) [V]
All in `BATCHEDRENDERFONTDESC::RenderBatch` (OFFSET 0x006C4AD0, `GxuFontMiscClasses.cpp:288`) unless noted.
| Site → resume | Purpose | Port equivalent |
|---|---|---|
| 0x006C4AF3 → 0x006C4B00 | replaces the first `CheckGeometry` loop with a pre-pass calling the CheckGeometry hook for every string, then `PrefetchCodepoints(firstString)` (generate all queued glyphs of the batch at once) | host callback "batch begin (BATCHEDRENDERFONTDESC*)" before the `CheckGeometry` loop (`:293-296`) |
| 0x006C4B09 → 0x006C4B10 | after each `CheckGeometry` call, `ProcessGeometry(string)`: rewrites each MSDF glyph quad (detected by `u > 1`) to the MSDF cell: position from `bitmapLeft/Top*scale ± pad`, UVs from atlas, page index encoded in the **signs** of u/v (bit0 → −u, bit1 → −v); clears 0x40000000; stores eviction stamp `(count&0x7F)|0x80` in `m_flags` bits 24-31 | host callback "string geometry built (CGxString*)" right after `CheckGeometry()` returns |
| 0x006C4B40 → 0x006C4B45 | `GxBufStream(vertex, 24, N)`: N = clamp(g_runtimeVBSize, 0x800, 0xFFFC) instead of 2048 | `GxuFontMiscClasses.cpp` `GxBufStream(..., 2048)` call |
| 0x006C4B64 → 0x006C4B70 | `space = g_runtimeVBSize` | `int32_t space = 2048;` |
| 0x006C4C36 → 0x006C4C4B, 0x006C4C67 → 0x006C4C8A | flush with `g_runtimeVBSize - space` / reset space to g_runtimeVBSize | the two `UnlockVertexPtrAndRender(buf, 2048…)` calls |
| 0x006C4CC4 → 0x006C4CC9 | epilogue: `g_runtimeVBSize = 0` | end of RenderBatch |
| 0x006C47BD → 0x006C47D8 (in `Initialize`, 0x006C47B0) | index pool 0x30000 bytes, buffer `0x1801A` | `GxuFontMiscClasses.cpp:222-223` (6144 / 3072) |
| 0x006C480C → 0x006C4811 (in `InitializeIndexBuff`, 0x006C47F0) | `ebx = 0x3FFF` quads instead of 512 | `:233` loop `i < 512` |
| 0x006C8C71 → 0x006C8C77 (in `CalculateYOffset`, 0x006C8C60, `IGxuFontGlyph.cpp:21`) | for MSDF faces read `bitmap_top` as 0 (`face->glyph(+0x54)->bitmap_top(+0x68)`) | host: skip the baseline offset for MSDF faces |
Why: with every glyph forced onto "texture page 0" a string's whole geometry is drawn in one page loop, so
the vertex stream and the static quad index buffer must be large enough (16 383 quads) — **[I]** reasoning,
code **[V]**. Struct fields used, mod name → port name: `m_fontSizeMult`(0x1C) → `m_currentFontHeight`;
`m_fontObj`(0x44) → `m_currentFace`; `m_flags`(0x5C) (bit 0x80 = 3-D string, 0x40000000 = MSDF pending,
24-31 = eviction stamp) → `m_flags`; `m_geomBuffers[8]`(0xB4) → `m_textLines[8]` (`TEXTLINETEXTURE*`);
`CGxFont::m_atlasPages[0].m_flags` → `m_textureCache[0]`/`m_fontFlags` (outline 1 = OUTLINE, 8 = THICKOUTLINE);
`m_rasterTargetSize` → `m_pixelSize`(0x24C); `m_ftWrapper` → `m_fontFaceHandle`(0x70).
Caveat: the mod's `CGxString` layout (`GameClient.h:1144`) differs from the port after 0x7C (it has no
`m_scale`) — port by member name, never by offset.

## 2. Other client globals/functions used
See tables above. Also `0x00C5DF88`+`0x397C` (IDirect3DDevice9 inside CGxDeviceD3d). `GameClient.h`/`Lua.h`
declare many unrelated addresses (nameplates, Lua) inherited from Transmorpher; **none are called** by the
font code.

## 3. Lua / CVars / slash / events
None. No addon. (No `MSDFMode` — always on.)

## 4. Rendering
- No passes of its own; it changes how the UI font batch draws.
- Shaders (runtime-compiled with `D3DCompile`, `src/font_exact/MSDFShaders.h`):
  - VS `vs_3_0`: `mul(pos, WorldViewProj)`; c23.x<1 → passthrough; else page index = (u<0) + 2·(v<0), uv = |uv|.
    Inputs POSITION0, COLOR0, TEXCOORD0 (the client's `CGxVertexPCT`, 24 bytes). `WorldViewProj` is the
    client's UI constant (bound by the client's UI vertex shader convention — c0..c3 **[I]**).
  - PS `ps_3_0`: s0 = game glyph texture (non-MSDF path), s12-s15 = 4 atlas pages; c23 =
    (fontPx, outline hint 0/1/2, spread 12, atlas 2048). `sd = median(r,g,b)`;
    `screenPxRange = spread / max(fwidth(u),fwidth(v))/atlas · (1 − min(0.3, 0.0035·fontPx))`;
    `alpha = saturate((sd−0.5)·range+0.5)`; outline: `outlinePx = max(2.75, fontPx^0.15·1.6)` (thick) or
    `max(1.5, fontPx^0.075·1.5)`; colour = lerp(black, col, alpha), alpha = max(alpha,
    saturate((a−0.5)·range·5 + outlinePx))·col.a.
- Atlases: up to 4 pages per font, `D3DFMT_A8R8G8B8` 2048², managed pool, LRU page eviction
  (`m_evictionCount` bumps invalidate strings), gutter 14 px. Glyph cell 96 px render size, spread 12.
  RGB = MSDF (msdfgen `generateMSDF`, overlapSupport, `edgeColoringInkTrap(3.0)`, `resolveShapeGeometry`,
  sign-corrected non-zero), A = true SDF with 5× range (outline). Generation is synchronous on the render
  thread on first use, batched per string batch.
- No depth/stencil use. Pass position: whatever the client's UI pass is (`CSimpleTop` render → font batches).

## 5. Config/data
- No config. Disk cache: `%TEMP%\LEXARA\<family>_<style>_s96_sp12\` with `manifest.dat` (+`.lock`, `.jrn`),
  `block_<N>.dat` (+`.lock`), `.tmp/.old` swap files. Formats (packed):
  - `ManifestHeader` 24 B: `magic 'MSMF' 0x4D534D46, version 1, sdfRenderSize u32, sdfSpread u32, entryCount u32, pad`;
    then `entryCount × {codepoint u32, blockId u32}`.
  - Block = codepoints `[N·512, N·512+511]`: `BlockFileHeader` 64 B (`magic 'MSDB' 0x4D534442, version, blockId,
    entryCount`), then `GlyphEntry` 64 B each (`codepoint u32, width u16, height u16, bitmapTop i32,
    bitmapLeft i32, dataOffset u32, dataSize u32`, padded), then RGBA8 pixel data.
  - Fonts are identified by a hash of the font bytes (`MSDFManager::RegisterFont(HashFont(...))`).
- Font validity: `MSDFValidator::IsFontMSDFCompatible` (self-intersection check) unless cache already has glyphs.

## 6. Third-party
Microsoft Detours (MIT), FreeType 2.14.1 (FTL/GPLv2 dual), msdfgen + msdfgen-ext (MIT), Skia (BSD-3, via
msdfgen-ext `resolveShapeGeometry`), ankerl::unordered_dense 4.8.1 (MIT), D3DCompiler_47 (runtime). Licence of
Lexara: `LICENSE` file in repo (MIT per repo). FreeType: the port already vendors an old FreeType; the
modern one would replace or sit beside it (symbol clash → build the mod's FreeType with prefixed symbols or
hidden visibility).

## 7. macOS porting plan
Pure logic (port as is): MSDFFont/MSDFCache/MSDFManager/MSDFValidator/MSDFUtils (replace Win32 file locking,
`GetTempPathW`, `LockFileEx` with `flock`/`NSTemporaryDirectory` or better the game dir), msdfgen + skia path
ops (skia is only used for `resolveShapeGeometry`; msdfgen can be built `MSDFGEN_USE_SKIA=OFF` at a quality
cost — keep skia, it builds on macOS arm64), ProcessGeometry/PrefetchCodepoints/version-stamp logic.
Needs host API (ModAPIv2 §R7 "font pipeline"): face-loaded/face-destroyed callbacks; glyph-entry-created
callback (`NewCodeDesc`); glyph-rendered callback (`IGxuFontGlyphRenderGlyph`); string text-line
initialised; string geometry built (after `CheckGeometry`); batch begin/end; dynamic batch vertex budget
and 16 383-quad index buffer (host-side change in `GxuFontMiscClasses.cpp`, gated); per-string draw callback
after `WriteGeometry` to bind atlas textures + constant; batch end to reset; MSDF-face baseline override in
`CalculateYOffset`.
Metal: two options —
(A) **Recommended**: keep the client's UI font draw (D3D9→Metal translator) and let the mod supply the MSDF
VS/PS as D3D9 SM3 bytecode compiled offline by fxc from the unchanged HLSL (the translator supports ps_3_0,
`dsx/dsy`→`dfdx/dfdy`, 16 samplers — `CGxDeviceMetal/D3D9/ShaderTranslator.cpp:953`), installed into
`g_fontPixelShader/g_fontVertexShader` via a host "replace shader" call; atlases as `D3D9Metal::Texture`
(`D3DFMT_A8R8G8B8`, `Device.h:201`) bound to samplers 12-15; constants via `SetPixelShaderConstantF(23)`.
This needs the host to expose the `D3D9Metal::Device` (ModAPIv2 §R2).
(B) Native MSL port of the two shaders and a host callback that substitutes the pipeline state for font
batches. More work, no benefit.
Risks: double-implementation with AwesomeWotlk (ship one); FreeType version swap affects every client font
metric (test glue + FrameXML text layout); glyph generation stutter (pregen + cache); the sign-encoded page
index requires the translator to pass negative texcoords unmodified (it does — no clamping of varyings
**[I]**); larger index buffer must be ≤ 65 535 vertices (16 383 quads × 4 = 65 532 ✓).
