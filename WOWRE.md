# WOWRE.md — `WowRe` dalının çalışma defteri

Bu dosya `WowRe` dalının tek doğruluk kaynağıdır: ne yapıldı, ne yapılacak, ne yapılamadı, neden,
ve iş yarıda kalırsa nereden nasıl devam edilir. Her anlamlı adımdan sonra güncellenir.

## Hedef (kullanıcının tanımı)

> Projenin tamamen bitmiş, oynanabilir formda olması; hiçbir sorun yaşamadan başından sonuna kadar.

Sıralı fazlar:

1. **Native Metal backend** — `CGxDeviceMetal`, D3D9 taklit katmanı (`D3D9Metal::`) olmadan doğrudan
   `CGxDevice` → Metal.
2. **WMModAPI v2** — `.dylib` modların ihtiyaç duyduğu motor köprüleri (render, Metal, font, kamera,
   Lua/CVar, sahne, UI).
3. **St0ny AIO "Reforged" EXE yamaları** — eksik yamalar + 8 nokta ışık (#67) + LightStay (#68).
4. **St0ny DLL'lerinin `.dylib` portları** — `LuxShoulderCam`, `Lexara`, `AwesomeWotlk`.
5. **CoAVolFog** — Volumetric Fog & Light Shafts (+ Modern Water) native Metal modu.

Kaynaklar (hepsi GitHub'da):

- St0ny patcher: <https://github.com/Raz0r1337/St0nys-AIO-WoW-EXE-Patcher> (`st0ny-main`)
- 8 ışık shader'ları: <https://github.com/Raz0r1337/WoW-lights-patch-shader>
- Volumetric fog: <https://github.com/Raz0r1337/coa-vfog> (`st0ny-main`)
- Lux Shoulder Cam: <https://github.com/Stormhand-dev/Lux-Shoulder-Cam>
- Lexara: <https://github.com/Stormhand-dev/Lexara---HD-Font-Renderer-for-WoW-3.3.5>
- awesome_wotlk: <https://github.com/noname08662/awesome_wotlk>
- İndirme sayfası: <https://projectreforged.github.io/wotlk/downloads/>

## Dal ve worktree düzeni

| Worktree | Dal | Ne için |
|---|---|---|
| `../WrathModern` | `warmane-login` | Eski Warmane işi + commit'lenmemiş mod deneyi. **WowRe işi burada yapılmaz.** |
| `../WrathModern-warden` | `grimfall-recovered` | Warden / buck-data. WowRe'ye taşınmaz. |
| `../WrathModern-WowRe` | `WowRe` | **Bu proje.** |

- `WowRe`, `macos` dalının `a3d749f` commit'inden açıldı. `macos`'ta Warmane/Warden kodu yok;
  istemci kendini Intel Mac istemcisi (`x86`/`OSX`) olarak raporlar. Bu değiştirilmez.
- `warmane-login` çalışma ağacındaki mod deneyi (ModHost, WMModAPI v1, dünya render hook'ları,
  `GxMetalAcquireTargets`, `Mods/Samples/DepthFog`) `WowRe`'ye taşındı.
- **Taşınmayanlar ve nedenleri:** `NetClient.cpp`/`RealmConnection.cpp`'deki `/tmp/wm-net.log`
  dökümleri (session key'i diske yazıyordu — güvenlik), `System.mm` platform kimliği farkı ve
  `README.md`/`WARMANE.md` Warmane notları.
- `main` dalı kullanılmaz.

## Derleme

```sh
cd ../WrathModern-WowRe
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWRATHMODERN_GAME_DIR="/Volumes/Yaman SSD/WoW_Wrath" \
  -DWRATHMODERN_APP_NAME=WrathModern-WowRe
ninja -C build/mac WrathModern
# örnek mod
cmake -S Mods/Samples/DepthFog -B build/mods/DepthFog -G Ninja && ninja -C build/mods/DepthFog
```

- **Oyun dizini artık `/Volumes/Yaman SSD/WoW_Wrath`** (AGENTS.md'deki `wow3.3.5a` diskte yok).
  Derleme kendini oraya `WrathModern-WowRe.app` adıyla kopyalar; aynı dizindeki `WrathModern.app`
  ve `WrathModern.bak.app` başka derlemelerdir, **dokunulmaz**.
- `Tools/sign-app.sh` (warden dalından alındı) hem `bin/` içindeki uygulamayı hem kopyayı
  `WrathModern Dev` kimliğiyle imzalar; disk erişim izni yeniden derlemelerde korunur.

### Yerel referanslar (Windows Reforged kurulumu)

`WoW_Wrath` içinde orijinal Reforged kurulumu var; port doğrulamasında **kesin referans** budur:

- `WoW_Wrath/mods/`: `AwesomeWotlkLib.dll`, `CoAVolFog.dll` + `CoAVolFog.ini` + `fogdata.bin` +
  `waterdata.bin` + `CoAVolFog.log`, `Lexara.dll`, `LuxShoulderCam.dll`, `libSiliconPatch.dll`.
- `WoW_Wrath/LuxShoulderCam.cfg`, `Cache_AwesomeWotLK/`, `Fonts_AwesomeWotLK/`, `mtld3d*` (CrossOver
  D3D9→Metal katmanı: görsel/performans karşılaştırma tabanı), `St0nys-AIO-WoW-EXE-Patcher/`.
- `/Volumes/Yaman SSD/ProjectReforged/presets/`: `patcher_selection.ini` (Reforged'ın seçtiği
  yamalar, örn. `timer=1`, `lux=1`, `farclip=1`) ve `CoAVolFog.ini`.

## Mimari notlar (kalıcı bilgi)

### Mevcut Metal yolu (başlangıç noktası)

```
CGxDevice (EGxRenderState, CGxPool, CGxTex, CGxShader)
  └─ CGxDeviceMetal  (CGxDeviceD3d'nin satır satır kopyası; EDeviceState[182], DsSet)
       └─ D3D9Metal::Device / DeviceImpl  (D3DRS_*, D3DTSS_*, D3DFORMAT, Surface, Query ...)
            ├─ UploadHeap, BufferCore, TextureCore, SurfaceImpl      (Metal kaynakları)
            ├─ PipelineKey / SamplerKey önbellekleri                  (Metal durum nesneleri)
            ├─ ShaderBytecode + ShaderTranslator (SM1-3 bytecode → MSL)
            └─ FixedFunction (D3D sabit fonksiyon durumundan MSL üretimi)
```

Dosyalar: `Engine/Source/Gx/CGxDeviceMetal/CGxDeviceMetal.{h,cpp}` (~3.8k satır) ve
`D3D9/` (~7.6k satır). `Engine/Source/Gx/CGxDevice/Mac/CGxDevice.mm` adaptör sorguları için de
`D3D9Metal::Direct3D` kullanır.

### Native port'ta neyin kalıp neyin gittiği

- **Kalır (veri formatı, API değil):** `.bls` shader'ları Direct3D 9 SM1.1–3.0 bytecode'u içerir.
  Native Metal'de de bu bytecode MSL'e çevrilmek zorunda. `ShaderBytecode` + `ShaderTranslator`
  korunur, `D3D9Metal` isim alanından çıkarılır.
- **Kalır (zaten Metal):** upload heap, buffer yeniden adlandırma (rename) + seri numarası,
  pipeline/sampler önbellekleri, oklüzyon sorguları, cursor.
- **Gider:** `Direct3D`/`Device`/`Surface`/`VertexDeclaration` arayüzleri, `D3DRS_*`/`D3DTSS_*`
  durum dizileri, `EDeviceState` + `DsSet`, `D3DFORMAT` ara eşlemeleri. `CGxDevice`'ın
  `m_appRenderStates`/`IRsSendToHw` çıktısı doğrudan Metal durum anahtarlarına yazılır.
- **Değişir:** sabit fonksiyon üreticisi D3D TSS dizisi yerine doğrudan `CGxDevice` durumundan
  (texture op/arg, texgen, ışıklar, materyal, fog) beslenir.

### `CGxDeviceMetal`'in kullandığı D3D9 yüzeyi (2026-10-21 ölçümü)

`CGxDeviceMetal.cpp` içinde shim'e yapılan çağrılar (sayı = çağrı noktası). Native port bu listenin
tamamını doğrudan Metal karşılığına bağlayınca `D3D9/Device.h` silinebilir.

| Grup | Çağrılar | Native karşılık |
|---|---|---|
| Durum | `SetRenderState` ×40, `SetTextureStageState` ×8, `SetSamplerState` ×7, `SetTransform` ×7, `LightEnable` ×3, `SetLight`, `SetMaterial`, `SetClipPlane`, `SetScissorRect`, `SetViewport` | `MetalStateBlock`: pipeline/depth-stencil/sampler anahtarları + sabit fonksiyon uniform'ları |
| Kaynak | `CreateTexture` ×4, `CreateCubeTexture`, `CreateRenderTarget`, `CreateDepthStencilSurface`, `CreateVertexBuffer`, `CreateIndexBuffer`, `CreateVertexDeclaration` ×2, `CreateQuery` ×5 | `CGxTex`/`CGxPool`/`CGxQuery` → doğrudan `id<MTLTexture>`/`id<MTLBuffer>`/görünürlük tamponu |
| Shader | `CreateVertexShader`, `CreatePixelShader`, `SetVertexShader` ×2, `SetPixelShader` ×2, `Set*ShaderConstantF` | `CGxShader` → çevrilmiş MSL fonksiyonu; sabitler uniform tamponuna |
| Çizim | `SetStreamSource`, `SetIndices` ×2, `SetVertexDeclaration` ×2, `DrawPrimitive`, `DrawIndexedPrimitive`, `Clear`, `BeginScene`/`EndScene` | render encoder |
| Hedefler | `SetRenderTarget`, `GetRenderTarget`, `SetDepthStencilSurface` ×3, `GetDepthStencilSurface` ×2, `GetBackBuffer` ×2, `StretchRect` ×4 | `CAMetalLayer` drawable + ofscreen dokular, blit/resolve |
| Swap chain | `CreateDevice`, `Reset` ×2, `TestCooperativeLevel`, `Present`, `SetGammaRamp` ×2, `EvictManagedResources` | `CAMetalLayer`, gamma → son geçiş LUT'u |
| Adaptör | `CheckDeviceFormat` ×7, `CheckDeviceMultiSampleType` ×2, `CheckDepthStencilMatch`, `GetAdapterIdentifier` ×2, `GetAdapterDisplayMode` ×2, `GetAdapterCount`, `GetDeviceCaps` | `MTLDevice` özellik sorguları, `CGDisplay*` (`Mac/CGxDevice.mm` dahil) |
| İmleç | `SetCursorProperties`, `ShowCursor` ×3, `SetCursorScale` ×2, `GetCursorImage` | `NSCursor` (zaten native) |
| Mod | `GetMetalDevice`, `AcquireExternalTargets` | WMModAPI köprüsü |

## İlerleme günlüğü

### 2026-10-21 — başlangıç

- `WowRe` dalı + `../WrathModern-WowRe` worktree oluşturuldu (`macos` @ `a3d749f`).
- Mod deneyi taşındı. Temiz yapılandırma ile `WrathModern` (1443 hedef, 0 hata) ve `DepthFog.dylib` derlendi; `ModsRunHook` ve `GxMetalAcquireTargets` ikilide.
- Başlangıç commit'i: aşağıdaki "Başlangıç noktası" satırına bakın.

Başlangıç noktası: commit `f917b1c`, etiket `wowre-start` (`git diff wowre-start` o andan beri yapılan her şeyi gösterir).

## Sıradaki adımlar (yarıda kalırsa buradan devam)

1. [ ] Faz 1 ayrıntılı tasarımı: `CGxDeviceMetal`'in her sanal metodunu ve `IStateSync*` yolunu
       `DeviceImpl` içindeki karşılığına eşleyen tablo (bu dosyaya eklenecek).
2. [ ] `D3D9/` içindeki Metal çekirdeğini (`UploadHeap`, `BufferCore`, `TextureCore`, pipeline,
       translator, fixed function) `MetalCore` olarak D3D tiplerinden bağımsızlaştır; davranış
       değişmeden derlenmeli ve oyun aynı görünmeli.
3. [ ] `CGxDeviceMetal`'i adım adım `MetalCore`'a doğrudan bağla; her adımda derle + görsel test.
4. [ ] `D3D9/` arayüz katmanını sil, `Mac/CGxDevice.mm` adaptör sorgularını doğrudan Metal/CG'ye
       taşı.

## Yapılamayanlar / riskler

- (henüz yok)

## Kararlar

- **2026-10-21 — dal amacı:** `WowRe` ve `warmane-login` iki ayrı proje olarak paralel yürür.
  `warmane-login` bu proje bitene kadar eski kod tabanıyla (D3D9 shim) devam edebilir; `WowRe`
  bittiğinde native Metal + mod altyapısı oraya portlanır. Bu yüzden `WowRe` değişiklikleri
  **commit'ler küçük ve konu bazlı** tutulur (sonradan `cherry-pick`/rebase edilebilsin) ve Warmane'e
  özgü hiçbir şey `WowRe`'ye girmez.
- **2026-10-21 — canlı test sunucusu:** kullanıcının kendi AzerothCore sunucusu
  (Dokploy, `yaman@100.71.234.118`, "WoW Bot Brain" compose; ayrıntı hafızada `selfwow-server`).
  `OSX` platform kimliği burada sorun değil.
- **2026-10-21 — oyun dizini:** `WowRe` derlemesi oyun dizinine `WrathModern-WowRe.app` adıyla
  kopyalanır. (Kullanıcı `warmane-login` derlemesini oyun dizininden sildi.)

## Faz 3'te yeniden değerlendirilecek eski kararlar

`macos` dalında şu St0ny yamaları bilerek dışarıda bırakılmıştı (o zaman canlı ChromieCraft'ta
ban riski ve Windows'a özgülük gerekçesiyle):

- `timer` (#5) — Mac saati zaten hassas; Reforged preset'inde var. Gerçekten gerekli mi ölç.
- `mouse flicker` (#83), `windowfix` — Windows'a özgü; macOS karşılığı gerekip gerekmediği test edilecek.
- `occluder` (#61) — yalnızca özel ortam paketleri için anlamlı; kendi sunucumuzda Reforged
  ortam paketleri kullanılacaksa gerekli.
- `nothrottle`, `cache` — AzerothCore AntiDOS nedeniyle **dışarıda kalır** (kendi sunucumuzda da
  paket seli istemiyoruz).

## Açık sorular

- Kendi sunucuda otomatik giriş: `macos` tabanında `WRATHMODERN_AUTOLOGIN` var mı, yoksa taşınmalı mı?
  (Kullanıcı adına giriş yapılmaz; test hesabı/kimlik dosyası kullanıcıdan gelir.)
