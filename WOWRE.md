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

### Faz 1 tasarımı (2026-10-21)

**Model: `CGxDeviceOpenGl`, `CGxDeviceD3d` değil.** Orijinal istemcide D3D'siz bir backend zaten var:
`CGxDeviceOpenGl` (binary'den çözülmüş, 113 `VERIFY`). `CGxDevice` durumunu doğrudan kendi API'sinin
durum adlarına (`m_deviceStates[90]`: `Ds_DepthMask`, `Ds_Blend`, `Ds_CullFace` ...) eşliyor;
`IRsSendToHw`/`IStateSync*` iskeleti Metal için aynen kullanılabilir. Native `CGxDeviceMetal` bu
iskeleti izler; satır satır D3D kopyası olmaktan çıkar. Karşılaştırılabilirlik için her metodun
başına hangi orijinal fonksiyonu (OpenGL ya da D3D sürümü) örnek aldığı yazılır.

**Shader hedefi D3D SM kalır.** İstemcinin `.bls` setleri API'ye göre ayrılır (D3D: `vs_1_1`…`vs_3_0`,
`ps_1_1`…`ps_3_0`; OpenGL: `arbvp1`, `arbfp1`, NV register combiners). Metal cihazı D3D setini
yüklemeye devam eder: hem en zengin set odur hem de St0ny'nin 8 ışık shader'ları (`patch-5.MPQ`)
D3D bytecode'udur. Çevirici (`ShaderBytecode` + `ShaderTranslator`) bu yüzden kalır.

**Hedef dosya düzeni** (`Engine/Source/Gx/CGxDeviceMetal/`):

```
CGxDeviceMetal.h / .mm     CGxDevice sanal metodları, IRsSendToHw, IStateSync* (OpenGL iskeleti)
MetalContext.h / .mm       MTLDevice, kuyruk, seri numaraları, komut tamponu, CAMetalLayer, sunum
MetalResources.h / .mm     UploadHeap, buffer (CGxPool/CGxBuf), doku (CGxTex), sorgular
MetalFormats.h / .cpp      EGxTexFormat / CGxFormat → MTLPixelFormat (D3DFORMAT ara adımı yok)
MetalPipeline.h / .mm      pipeline / depth-stencil / sampler önbellekleri (+ ileride binary archive)
MetalFixedFunction.h/.cpp  sabit fonksiyon MSL üretici; anahtarı CGxDevice durumundan kurulur
Shader/ShaderBytecode.*    SM1–3 bytecode okuyucu (D3D9Metal isim alanından çıkarılır)
Shader/ShaderTranslator.*  bytecode → MSL
```

**Strangler sırası (her adım derlenir ve görsel olarak eskisiyle aynı olmalı):**

- 2a. Görsel taban: mevcut shim derlemesiyle giriş ekranı + karakter seçimi + dünyada sabit bir
  konumdan pencere görüntüleri (`Tools/` altına betik). Sonraki her adım bununla karşılaştırılır.
- 2b. `ShaderBytecode`/`ShaderTranslator`/`FixedFunction`'ı D3D9 arayüzünden bağımsız `GxMetal`
  isim alanına taşı (yalnızca isim/yer değişikliği; davranış aynı).
- 2c. `UploadHeap`/`BufferCore`/`TextureCore`/pipeline önbellekleri/seri numaralarını
  `MetalContext` + `MetalResources`'a çıkar; `DeviceImpl` geçici olarak bunların üstünde kalsın.
- 3a. Kaynaklar: `CGxPool`/`CGxBuf`/`CGxTex`/`CGxQuery` doğrudan `MetalResources` nesneleri tutsun
  (`D3D9Metal::VertexBuffer/Texture/Query` kalkar).
- 3b. Hedefler + sunum: `DeviceSetRenderTarget`, `SceneClear`, `ScenePresent`, `TexCopy/TexStretch`,
  okunabilir derinlik (`Depth32Float_Stencil8`, `ShaderRead`).
- 3c. Durum: `IRsSendToHw` → Metal durum anahtarları; `EDeviceState`/`DsSet`/D3DRS tabloları kalkar.
- 3d. Çizim: `Draw` → doğrudan render encoder.
- 4. `D3D9/Device.h`, `D3D9Types.h`, `Resources.mm`'nin D3D arayüzü silinir; `Mac/CGxDevice.mm`
  adaptör sorguları `MTLDevice`/`CGDisplay*`'e taşınır. `grep -r D3D9Metal` boş dönmeli.

## İlerleme günlüğü

### 2026-10-21 — başlangıç

- `WowRe` dalı + `../WrathModern-WowRe` worktree oluşturuldu (`macos` @ `a3d749f`).
- Mod deneyi taşındı. Temiz yapılandırma ile `WrathModern` (1443 hedef, 0 hata) ve `DepthFog.dylib` derlendi; `ModsRunHook` ve `GxMetalAcquireTargets` ikilide.
- Başlangıç commit'i: aşağıdaki "Başlangıç noktası" satırına bakın.

Başlangıç noktası: commit `f917b1c`, etiket `wowre-start` (`git diff wowre-start` o andan beri yapılan her şeyi gösterir).

### 2026-10-21 gece — test altyapısı (kullanıcı uyurken, tüm kararlar bana bırakıldı)

- `WoW/Source/Autotest.{h,cpp}` + `Tools/autotest.sh`: istemci gözetimsiz çalışır, kendi sunucuya
  girer, oyunun **kendi** ekran görüntüsü yoluyla (`ScrnScreenshot`, 88 baytlık istemci damgası
  zorunlu; 0 verilirse ImageWriter başlamaz) kare alır, çıkar. WTF yedeklenip geri yüklenir; bir
  seferde tek test (`/tmp/wrathmodern-autotest.lock`).
  - `Tools/autotest.sh <çıktı> "glue@15"` → giriş ekranı;
    `Tools/autotest.sh <çıktı> "world@15,world@30"` → dünyada iki kare.
  - Ortam: `WOWRE_APP`, `WOWRE_LOGIN` (kimlik dosyası), `WOWRE_CHARACTER`, `WOWRE_REALM(LIST)`.
- Hesaplar (şifreler yalnızca 600 izinli dosyalarda, repoda/log'da yok):
  - `~/.wrathmodern-wowre-autologin` → `loradona` (kullanıcının hesabı); test karakteri **Rhon**
    (insan rogue 1, Northshire). Ana karakter `Loradona`'ya dokunulmaz.
  - `~/.wrathmodern-selfwow-autologin` → `SELFWOW`; karakter **Selfwow**. Paralel ajan bunu kullanır.
  - Realm `Azeroth`, realmlist `100.71.234.118`.
- **Öğrenilen tuzak:** otomatik "Enter World" bir girdi olayı üretmediği için `CSimpleTop::s_eventTime`
  0 kalıyor; `CGGameUI` 30 dk boşta kontrolü anında `CMSG_LOGOUT_REQUEST` gönderiyordu. Sürücü artık
  tıklama gibi `s_eventTime`'ı tazeliyor. Teşhis `WRATHMODERN_NETLOG=1` (opcode günlüğü) ile yapıldı.
- Sunucu Mac Warden modülünü istiyor (`Warden.log`); `ClientResponseDelay=600`, `FailAction=0`
  → yalnızca log. Kendi sunucumuzda sorun değil.
- **Referans görüntüler (D3D9 shim'li hal):** `build/autotest/ref-shim/{glue,world,world-2}.png`
  (2560×1440). Faz 1'in her adımı bunlarla karşılaştırılır. Dünya karesi gece/NPC hareketi
  nedeniyle piksel piksel aynı olamaz; göz + ortalama fark ile karşılaştırılır.
- Derleme seçeneği: `-DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON` şart (Reforged GlueXML'i
  değiştirmiş; aksi halde "GlueXML is modified or corrupt" ve istemci girişte kalır). Faz 3 #19 bunu
  kalıcı yapacak.

### Paralel iş düzeni

- `../WrathModern-WowRe-patches`, dal `WowRe-patches` (WowRe'den): **Faz 3** (St0ny Reforged
  yamaları) bir alt ajanda. `Engine/Source/Gx/CGxDeviceMetal/**`'a dokunmaz (Faz 1 orada). Uygulama
  adı `WrathModern-WowRe-patches.app`, hesap `SELFWOW`. Bitince `WowRe`'ye merge edilir.
- `../WrathModern-WowRe-metal`, dal `WowRe-metal`: **Faz 1** (native Metal) bir alt ajanda. Uygulama
  `WrathModern-WowRe-metal.app`, hesap `loradona`/Rhon. Modların cihazla tek sözleşmesi `Gx.h`
  (`GxMetalDevice`, `GxMetalAcquireTargets`) — Faz 1 bunu korur, böylece Faz 2/4/5 bağımsız ilerler.
- Analiz ajanı: dört modun (Lux, Lexara, AwesomeWotlk, CoAVolFog) hook/adres → decompile fonksiyon
  eşlemesi ve Mod API v2 gereksinimleri → `/tmp/wowre-specs/*.md` (bitince repoya `Docs/Mods/` olarak alınır).
- `WowRe` (bu worktree): koordinasyon, Faz 2 (Mod API v2), dalların birleştirilmesi ve doğrulama.

### 2026-10-21 sabah — Faz 2 çekirdeği (Mod API v2) tamamlandı

- `WMModAPI.h` v2: Lua fonksiyonu/olay/CVar kaydı, `SignalEvent`, `ExecuteLua`, `GetLuaState`,
  `GetScriptContext`, `OnScriptContext(context, loaded)`. `WMModHost.size` + `WM_HOST_HAS` ile ileri
  uyumluluk; v1 modlar yüklenmeye devam eder.
- Lua C API'si yürütülebilir dosyadan dışa açık (128 sembol); modlar `-undefined dynamic_lookup` ile
  bağlanır, ikinci bir Lua kopyası **asla** bağlanmaz.
- Glue (`CGlueMgr`) ve oyun (`GameUI`) Lua durumu her kurulduğunda mod fonksiyonları yeniden kaydedilir;
  mod olayları istemcinin olaylarından sonra eklenir (`FrameScript_AppendEvents`), istemci indeksleri değişmez.
- **Tuzak:** istemcinin `OsValidateFunctionPointer`'ı Lua C fonksiyonlarının ve CVar callback'lerinin
  ana ikilinin `__TEXT`'inde olmasını şart koşuyor ("Invalid function pointer" fatal). Kontrol
  kapatılmadı; ModHost yüklediği her modun `__TEXT`'ini `OsAllowFunctionPointerImage` ile ekliyor.
- Doğrulama: `Mods/Samples/ApiSmoke` (oyun dizinine geçici kopyalandı, sonra silindi) — glue ve oyun
  bağlamında ping/olay/CVar zinciri `Logs/Mods.log`'da eksiksiz.
- Oyun dizinindeki mod klasörü `WoW_Wrath/mods` (exFAT büyük/küçük harf duyarsız, `Mods` ile aynı);
  içinde Windows Reforged DLL'leri de duruyor, ModHost yalnızca `*.dylib` yükler.

### Token bütçesi kararı (kullanıcı, 2026-10-21 sabah)

Kullanıcı: "Token sınırlı, ajanları çok sık kullanma." Bundan sonra **en fazla bir ajan** aynı anda
çalışır; ajan talimatları tutumlu olmayı söyler. Sıra: Faz 3 ajanı (bitiyor) → Faz 1 ajanı (adım 1
commit'lendi, devam edecek) → spesifikasyon ajanı yalnızca gerekirse. Küçük işler (Lux vb.) ana oturumda.

## Sıradaki adımlar (yarıda kalırsa buradan devam)

1. [x] Faz 1 tasarımı (yukarıda "Faz 1 tasarımı").
2. [ ] 2a görsel taban — GUI testi; kullanıcı oyunda değilken (bkz. hafıza `gui-testing-etiquette`).
3. [ ] 2b → 2c → 3a → 3b → 3c → 3d → 4 (tasarımdaki sırayla, her biri ayrı commit).

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

## Faz 3 — St0ny Reforged yamaları (2026-10-21)

Kaynak: `St0nys-AIO-WoW-EXE-Patcher` (`st0ny-main`, `apply_patches.ps1` + `docs/PATCHES.en.md`), seçim:
`/Volumes/Yaman SSD/ProjectReforged/presets/patcher_selection.ini`. VA'lar temiz `Wow.exe.ORI` (12340) üzerinde
capstone ile doğrulandı. Hepsi `WRATHMODERN_CLIENT_PATCHES` arkasında; yamasız derleme orijinal davranışa döner.

| # | Yama | Reforged | Durum | Yer | Kanıt / not |
|---|---|---|---|---|---|
| 1 | laa | 1 | no-op (macOS) | — | 64-bit süreç; 2 GB sınırı yok |
| 3 | itemcache | 1 | önceden var (değiştirilmiş) | `DBCache.h` | 0x6695FD; 30 sn pencere yerine saniyede 1/30 dolum, AntiDOS'a takılmaz |
| 5 | timer | 1 | yapıldı (Win) / no-op (macOS) | `Os/Win32,Win64/TimeManager.cpp` Calibrate | 0x86AC8E; Mac `Calibrate` zaten her zaman monotonic saati seçer |
| 7 | mirrorfix | 1 | önceden var | `Unit_C.cpp` | |
| 8 | wmocube | 1 | önceden var | `MapObjRead.cpp` | |
| 9 | glyphfix | 1 | önceden var | `GxuFont*` | |
| 12 | scandll | 1 | önceden var / no-op | `ScanDLLGlue.cpp` | Scan.dll macOS'ta çalışmaz; temiz tarama sayılır |
| 13 | noserverpatch | 1 | yapıldı | `CGlueMgr::Idle` case 6 | 0x4DAEA8; St0ny indirmeyi atlar (giriş asılı kalır) — burada aktarım reddedilir, oturum kapanır, LOGIN_BADVERSION gösterilir |
| 14 | nosurvey | 1 | önceden var | `CGlueMgr::Idle` | 0x4DAEBD |
| 15 | skipbnet | 1 | yapıldı | `ClientServices::Logon` | 0x6B2B48; '@' içeren hesapta da Grunt girişi |
| 16 | skiprdp | 1 | yapıldı (macOS'ta zaten 0) | `ConsoleDetectDetectHardware` | 0x76BA40 |
| 17 | nohttp | 1 | önceden var | `OsURLDownload.mm` | |
| 19 | glue | 1 | yapıldı | `FrameXML_CheckSignature`, `DeleteInterfaceFiles` | 0x8165E0/0x5F4DBF. GlueXML/FrameXML/`Blizzard_` imzası her zaman geçerli (MD5 gerçek dosyadan); **3. parti eklentiler orijinal kontrolde kalır** — St0ny sürümü imzasız eklentileri "secure" yapıyordu (= Lua unlock, dışlandı). `-DWOWDECOMP_DISABLE_SIGNATURE_CHECKS` olmadan giriş + dünya test edildi |
| 21/22 | mpqnames/localdata | 0 | dışlandı | — | Reforged kapalı |
| 31 | lux | 1 | Faz 4 | — | DLL yükleyici; `.dylib` portu Faz 4 |
| 32 | areatrigger | 1 | önceden var | `Player_C.cpp` | |
| 33 | swing | 1 | var, artık kapılı | `CGPlayer_C::MeleeModeEnter` | 0x6E2867; WoWFix335 girdisinde koşulsuzdu |
| 34 | npcanim | 1 | var, `#else` orijinale düzeltildi | `CGUnit_C::PreAnimate` | 0x73E3C9 |
| 35 | spellanim | 1 | var, artık kapılı | `CGUnit_C::OnChannelSpellChanged` | 0x73ECD6 |
| 36–39, 42 | ghostattack, naked, forcereaction, mail, level101 | 1 | önceden var | | |
| 54–57, 59 | farclip, horizon, envdetail, grounddist, goscale | 1 | önceden var | | (`sliders`, `cat0`, `deadchat`, `lootopen`, `flash` Reforged'da 0 ama macos dalında eklenmişti; korundu) |
| 61 | occluder | 1 | yapıldı | `CWorldOcclusion.cpp` `s_occlusionVolumes[0]` | 0xAF0040 harita anahtarı 99999 |
| 62–66 | bluemoon, notransparency, hdportraits, iconsnap | 1 | önceden var | | |
| 67 | lights | 1 | yapıldı (motor tarafı) | `CM2Lighting` (+`m_extraLights`), `CShaderEffect::SetExtraLocalLights`, `CWorldScene::SetupLighting`, `CMapRenderChunk::SetVertexShader`, `GxMaxLights` | M2/WMO ışık 5–8 → c236–c246 (≥247 kayıt, yalnız 4 ışıklı permütasyonda), zemin ışık 4–8 → c236–c250 (≥251); sabit fonksiyon 8 slot, en yakın önce. `Data/` içinde `patch-5.MPQ` **yok** → stok shader'lar bu sabitleri okumaz, görüntü değişmez |
| 68 | lightstay | **0** | yapıldı, CVar `lightStay` (vars. 0) | `CM2Scene::SelectLights` | 0x81E548 |
| 69–71 | tracker, worldmap, castbars | 1 | önceden var | | |
| 74 | charrandom | 1 | yapıldı | `CCharacterCreation::InitCharacterComponents` | 0x4E147B |
| 79 | bubblerange | 1 | önceden var | | |
| 80/81 | window/maximize | 1 | yapıldı | `RegisterGxCVars` | 0x76A67D/0x76A6B2; macOS'ta görünür alanı kaplayan pencere |
| 82 | windowfix | 1 | var, artık kapılı | `DesktopGammaCallback` | 0x401A94 |
| 83 | mouse | 1 | no-op (macOS) | — | Windows SetCursorPos yeniden merkezleme yarışı; Mac mouselook imleci ayırıp NSEvent delta kullanır |
| 84 | sound | 1 | yapıldı | `SI2::RegisterSoundEngineCVars`, `SI2::Init` | kalite 2, 64 kanal, reverb 1; 126 kanal yalnız donanım seslerinde. macOS'ta `Sound_EnableHardware` 0 kalır (Core Audio yazılım karıştırır) |
| 11, 23, 24, 47–53, 6, 2 | wardenoff, luaunlock*, hareket hileleri, nothrottle, cache | 0 | dışlandı | — | ban riski / AntiDOS |

**Metal ajanına gereksinimler (#67):** vertex sabit dosyası 256 kayıt kalmalı ve `m_caps.m_shaderConstants[GxSh_Vertex]` ≥ 251
raporlanmalı; yüklenen aralık shader'ın okuduğu en yüksek kaydı (c250) kapsamalı; sabit fonksiyon yolu `IStateSyncLights`
ve üretici 8 ışık (`GxMaxLights`) işlemeli — şu an 4 slot döngüsü sun + 3 en yakın ışığı gösterir.

Test: `build/autotest/phase3` (imza kontrolü AÇIK derleme) — giriş ekranı, giriş, dünya (Northshire) normal.
Doğrulanamayanlar: 8 ışık shader'ları (patch-5.MPQ kurulu değil), sunucu yamasını reddetme yolu (AzerothCore
12340 için yama göndermez), Windows tarafı (timer) derlenmedi.
