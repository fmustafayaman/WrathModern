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
    (insan rogue 1, Northshire). `Loradona` (Azuremyst) da test için kullanılabilir — kullanıcı 2026-10-21'de istedi (`WOWRE_CHARACTER=Loradona`); test karakteri yürütmez, yalnızca girer/bakar.
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

### 2026-10-21 öğlen — Faz 3 birleşti, Lux bitti, AwesomeWotlk başladı

- `WowRe-patches` → `WowRe` birleşti (çakışmasız). Artık `-DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=OFF` ile
  çalışıyor (yama #19). Faz 3 ayrıntıları "Faz 3 — St0ny Reforged yamaları" bölümünde.
- **Motor SDK'sı kararı:** yürütülebilir dosya ~23 bin C++ sembolünü dışa açıyor. Ağır modlar
  (`Mods/CMakeLists.txt`, ana ağaçta derlenir) decompile başlıklarını kullanıp istemci fonksiyonlarını
  isimle çağırır (`-undefined dynamic_lookup`); Windows DLL'lerinin adresle yaptığı her okuma/çağrı böyle
  karşılanır. Host hook'u yalnızca kontrol akışına müdahale gereken yerlerde. **Tuzak:** üst CMake
  `link_libraries(MacPlatform)` modlara sızmasın diye `wrathmodern_add_mod` LINK_LIBRARIES'i boşaltır.
  Mod istemciyle aynı derlemeden gelmeli (yapı düzenleri eşleşmeli).
- **LuxShoulderCam (Faz 4, tamam):** host'ta `CGCamera::CalcTargetCamera` içinde `OnCameraTarget`
  (orijinalin 0x006070CB yamasının yeri). Mod DLL'in disassembly'den çıkan mantığını birebir uygular,
  `LSC()` protokolü + `LuxShoulderCam.cfg`. Doğrulama: değiştirilmemiş addon kayıtlı ofsetleri uyguladı,
  karakter merkezin sağında, sohbette "Active -- /lsc to open panel". Kullanıcının cfg'si geri yüklendi (0/0).
- **AwesomeWotlk A aşaması (tamam):** `AwesomeWotlk=38` global'i, 48 CVar (varsayılan/aralık/kırpma),
  Unit API'leri, `GetInventoryItemTransmog`, `GetSpellBaseCooldown`, `IsWindowFocused`, `FocusWindow`,
  `CopyToClipboard`. AwesomeCVar artık "not loaded" demiyor. **Henüz etkisiz CVar'lar** (B/C aşamasında
  bağlanacak): nameplate*, camera*, showPlayer, interaction*, ui*, MSDF*, objectHighlightMode,
  portraitResolution, chat/combatLogSessionKey, tts*.
- Modlar artık derlemeyle `WoW_Wrath/Mods` (= `mods`) içine kurulur: `LuxShoulderCam.dylib`,
  `AwesomeWotlk.dylib`. Eski v1 host'lu derlemeler (ör. `WrathModern.app`) v2 modları reddeder.

### 2026-10-21 öğleden sonra — Faz 1 park edildi, AwesomeWotlk B başladı

- **Faz 1 (native Metal) park edildi.** `WowRe-metal` dalındaki yarım iş incelendi: yeni başlık
  (`CGxDeviceMetal.h`, `MetalContext/MetalResources/MetalPipeline/MetalFormats/MetalFixedFunction`,
  ~6 600 satır) yazılmış ama `CGxDeviceMetal.cpp` hâlâ eski D3D9 shim uygulaması (176 `m_d3dDevice`
  çağrısı, 226 D3D sabiti). Dosya `.mm` yapılınca bile 122 hata veriyor. WIP commit'lendi
  (`81ed11e`), ürün dalına alınmadı; Faz 1 ayrı bir iş olarak duruyor.
- **`nameplateDistance` / `nameplatePlacement` bağlandı.** İstemcide iki sabit değişkenle
  değiştirildi: `WoW/Source/Object/ObjectClient/NamePlateTuning.h` (`g_namePlateMaxDistanceSq`,
  `g_tabTargetMaxDistanceSq`, `g_namePlateHeightOffset`). Orijinal bunları Wow.exe'de yama ile
  yazıyordu (0x00715739 operantı ve tab-target tavanı); bizde mod değişkeni yazıyor. Nameplate
  yerleşim yöneticisi (stacking/clamp/hitbox/occlusion) hâlâ istemcinin kendi kodu.
- **`GetItemInfoInstant` eklendi** (`Mods/AwesomeWotlk/Item.cpp`): öğe önbelleği + DBC sınıf/alt sınıf
  adları + `s_inventoryTypeTokens` + `CGItem_C::GetInventoryArt`. Önbellekte kayıt yoksa **değer
  döndürmez** (istemcinin `GetItemInfo`'su gibi); ilk çağrı yalnızca sorguyu gönderir.
- **Test aracı:** `WRATHMODERN_AUTOTEST_LUA=<lua>` dünyaya girince bir kez çalışır; sohbet
  penceresinde görünür. Doğrulama: karakterin hançeri için
  `2092|Weapon|Daggers|INVTYPE_WEAPON|INVTYPE_Weapon_ShortBlade_05de|2|15`.

## Sıradaki adımlar (yarıda kalırsa buradan devam)

1. [x] Faz 1 — native Metal cihazı (aşağıda "Faz 1 — native Metal cihazı", gözden geçirme dahil).
2. [x] Faz 2 çekirdeği (Mod API v2), Faz 3 (St0ny yamaları), Lux (Faz 4), AwesomeWotlk A.
3. [ ] AwesomeWotlk B (aşağıdaki liste).
4. [ ] **CoAVolFog (Faz 5) — ÖNCELİK.** Plan: aşağıda "Faz 5 — CoAVolFog (Metal) — PLAN". Kaynak kod GitHub'da
   (tersine mühendislik gerekmiyor). Sıradaki: dilim 1 (altyapı).
5. [ ] Lexara + AwesomeWotlk MSDF fontları (ortak), çerçeve yakalama, TTS (AVSpeechSynthesizer).

### AwesomeWotlk B — kalan iş

Yapıldı (commit'lerde): `cameraFov`, `showPlayer` (3ea5ed5), `nameplateDistance`/`nameplatePlacement`,
`GetItemInfoInstant` (e78c505), kare tick'i + etkileşim/`/interact`/vurgu (f9fad71), sohbet log dosya adları
(b2191f7). **Kalan:** nameplate olayları/`C_NamePlate`/`nameplateN` token'ları, `objectHighlightMode`,
`cameraIndirectVisibility`, shader gerektiren UI CVar'ları ve MSDF. Aşağıdaki keşif notları bunlar için.


- **Interaction** (`Misc.cpp` ~200 satır): istemcide gerekli her şey var —
  `ClntObjMgrEnumVisibleObjects(func, param)` (ObjectMgrClient.h:65), `CGGameUI::OnSpriteRightClick(guid)`
  (GameUI.cpp:2308; hedefler + `object->OnRightClick()`), `CGObject_C::ShowHighlightType(type)` /
  `HideHighlightType(type)` (tip 0/1 istemcide kullanılıyor, 2 boş; `m_modelFlags` bit 24-26).
  Oyuncu: `ClntObjMgrGetActivePlayer()` + `TYPEMASK_PLAYER`; mesafe `GetPosition()` farkından;
  koni `GetFacing()`. Obje süzgeci orijinalde: gameobject alt tipleri (door, button, questgiver,
  chest, binder, chair, spellfocus, goober, fishingnode, mailbox, meetingstone, guildbank) +
  `CanUse()/CanUseNow()`; unit için lootable/skinnable/`CanAssist(unit, true)` + NPC bayrakları.
  Bizde `GetInteractDistanceSq` yok → sabit 5 yarda kullan. **Gereken host eklentisi:** kare başına
  bir tick (`WMModInfo::OnFrame`) — şu an yalnızca render hook'ları var (WM_HOOK_*, 4 kez/kare).
- **Nameplate olayları / `C_NamePlate`**: `CGNamePlateFrame::Initialize(CGUnit_C*)` (NamePlateFrame.cpp)
  ve `CGUnit_C::NamePlateClearAll` (Unit_C.cpp:20269) çengellenmeli; Lua tarafında frame nesnesi
  `C_NamePlate.GetNamePlateForUnit` için script nesnesi kaydı gerekir. `nameplateN` token'ları
  `Script_GetGUIDFromToken` / `Script_GetTokenIDsFromGUID` tarafında host token kaydı ister
  (WMModAPI'de yok). Tahmini 300-500 satır + ABI büyümesi.
- **Küçük ve hazır olanlar:** sohbet/combat log dosya adı (`s_chatLogNames`, ChatFrame_Scripts.cpp:1473
  — `static`, mod için erişimci gerekir; yalnızca oyuncu sohbet kaydını açtıysa görünür, bu yüzden
  ertelendi), `objectHighlightMode` (loot parıltısı), `cameraIndirectVisibility` (doodad'ların kamerayı
  kesmesini kaldırma + M2 solma).
- **Portre çözünürlüğü zaten karşılanıyor:** `kPortraitSize` `WRATHMODERN_CLIENT_PATCHES` ile 256
  (PortraitButton.cpp:74); `portraitResolution` CVar'ı bu yüzden işlevsiz, belgelenmeli.
- **Shader gerektirenler (ertelendi):** `uiHalfPixelFix`, `uiPixelSnap`, `uiTextureSampling`, MSDF —
  hepsi `CGxShaderExt` benzeri bir shader değiştirme katmanı istiyor; D3D9 shim varken HLSL derlemek
  ayrı bir iş.

## Faz 5 — CoAVolFog (Metal) — PLAN (2026-10-21, kullanıcının önceliği)

**Hedef (kullanıcı):** "Oyun WoW Forever'dan daha güzel görünsün; tüm uğraşım ona." Önce Windows CoAVolFog ile
**görsel eşlik** (CrossOver'daki Reforged kurulumu ve kullanıcının `CoAVolFog.ini`'si referans), sonra onu aşan
iyileştirmeler.

**Kaynak:** `~/Projects/refs/coa-vfog` (`Raz0r1337/coa-vfog`, `st0ny-main` @ `d08a6a3`): ~20 bin satır C++,
3,2 bin satır HLSL (ps_3_0), `data/fogdata.bin` (Classic sis katmanları) + `data/waterdata.bin` (Forever suyu),
README'de her motor girdisinin 12340 adresi. Lisans: `LICENSE` (port ederken korunur).

**Mimari kararı:** `Mods/CoAVolFog` — ayrı `.dylib` mod (kapatılabilir, cherry-pick edilebilir), motor SDK'sı ile
istemci yapılarını doğrudan okur. Orijinalin D3D9 sarmalayıcısı, `INTZ` derinlik hilesi, adres kancaları ve durum
blokları **gitmez → yerine**: Mod API'ye adlandırılmış render olayları + native Metal cihazından hedef/derinlik
erişimi. HLSL elle MSL'e çevrilir (sayısal davranış birebir), `fog_data.cpp`/`water_data.cpp` ayrıştırıcıları
taşınabilir C++ olarak aynen alınır. Ayar dosyası `Mods/CoAVolFog.ini` (aynı anahtarlar).

**Kanca → kaynak eşlemesi** (README "How it attaches"/"Engine inputs"):

| Orijinal | Bizde | Kullanım |
|---|---|---|
| `0x4FB03D` → `0x4F8EA0` | `CGWorldFrame::OnWorldRender` (Ui/WorldFrame.cpp) | kare başı/sonu, stok sisi itme/geri alma |
| `0x4F911D` → `0x823CB0` | `CM2Scene::Draw(M2PASS)` (pass 0 sonrası) | kamera/projeksiyon/ışık girdileri |
| `0x4F9170` → `0x77F020` | `CWorldScene::RenderLiquidPass` (World.cpp:1932) | su: derinlik yazımı, sınıf etiketleme, su geçişi |
| `0x4F9213` → `0x7F0870` | `DayNight::RenderGlares` | şeffaf sis yolu (TransparentFog) |
| `0x7E5818` → `0x6BCE40` | `GxuFontRenderBatch` (dünya yazıları) | sisin dünya yazılarından önce çizimi |
| `0x4F9281` → `0x8C1010` | `FFX::Finish` | sis kompoziti (parlamadan önce) |
| `0x780770` | `World::ValidateFarClip` | `FarClipMax` |
| `0x81FD15` → `0x873210` | `CShaderEffect::SetFogParams` (M2 batch sisi) | TransparentFog |
| `0x79D5E0` | `CMap::RenderWaterRipples` | dümen suyu/dalga halkaları |
| ışık tablosu, DayNight renkleri, sıvı ayarları… | `DayNight::g_dnInfo`, `CM2Scene` ışıkları, `LiquidType` DBC | doğrudan okuma |

**Dilimler** (her biri derlenir, A/B kareleri + Metal doğrulaması + GPU süresiyle kapanır):
1. **Altyapı:** Mod API render olayları (kare başı, pass-0 sonrası, sıvı geçişi başı/sonu + su çizimi başına
   sıvı türü, FFX öncesi); native cihazdan renk/derinlik (MSAA'da `texture2d_ms`, çözümlemesiz) ve komut tamponu;
   mod iskeleti, ini, veri yükleme, `Logs/CoAVolFog.log`; `DebugView=1` doğrusal derinlik görüntüsü.
2. **Sis çekirdeği:** çeyrek/yarım çözünürlük march + derinlik doğrulamalı upsample kompozit; uzak pus + zemin
   sisi + uzak sis (3.3.5 gün/gece verisinden, `DataMode`); stok sisin itilmesi; parlama ön-telafisi.
3. **Classic sis verisi:** `fogdata.bin` katmanları ışıklara göre, authored noise, yoğunluk varyasyonu, iç mekân geçişi.
4. **Zamansal filtre** (`Temporal`), siluet bölme, MSAA örnek başına.
5. **Yerel ışıklar** (8 slot, stabilizatör, faz) — LightStay zaten var (Faz 3 #68).
6. **Güneş saçılması, god rays, glow telafisi, renk derecelendirme** (`ColorGrading`, `ForeverGlow`).
7. **Su:** sıvı geçişinde stencil sınıf etiketleri, GPU FFT (Forever spektrumları), Beer–Lambert/HG/EnvBRDF
   gölgelendirme, köpük, ekran-uzayı yansıma, bölge renkleri.
8. **Dalga halkaları/dümen suyu**, su temasları, istemci sıçramalarının kapatılması.
9. **Şeffaf sis**, uzak mesafe (`FarClipMax`), ayar paneli (Alt+F7 yerine oyun içi Lua paneli).
10. **"Forever'ı aşmak":** Apple GPU'da tam çözünürlük march; gölge haritasıyla **gerçek hacimsel ışık huzmeleri**
    (radyal bulanıklık yerine — ağaç arası huzmeler); daha yüksek FFT çözünürlüğü; MSAA'nın doğrudan okunması.

**İlerleme:**
- **Dilim 1 — TAMAM (2026-10-21):** `Mods/CoAVolFog` (GPL-3.0, `LICENSE`); `fog_data`/`water_data`/`water_types`/`log.h`
  orijinalden değiştirilmeden; `config.cpp` taşınabilir ini okuyucu (orijinal anahtar/aralık tabloları);
  `Logs/CoAVolFog.log`. Veri: Classic 625 ışık/6222 katman/8 renk eğrisi, Forever 4 preset/7 FFT karosu/6 köpük
  maskesi. `WMWorldState`'e `viewportMinZ/MaxZ` eklendi (opak geçiş sonrası yakalanıyor; dünya `[0, 0.94]`,
  ötesi gök/WDL — README ile aynı). `DebugView=3` doğrusal derinlik MSAA'da `depth2d_ms`'ten doğrudan okunuyor
  (Windows'taki kopya hilesi gereksiz). Kanıt: `build/autotest/vf1`. Test sırasında kullanıcının `CoAVolFog.ini`'si
  yedeklenip birebir geri yüklenir (md5 kontrolü).
- **Port stratejisi (karar, 2026-10-21):** HLSL ps_3_0 sabitleri `register(cN)`'de; MSL'de tek bir
  `constant float4* C` tamponu (yazmaç numaraları aynı), her ad makroyla eşlenir (`#define cWorldViewportPx C[0]`,
  `cLayers[i]` → `C[12 + i]`, `row_major cViewToWorld` c4–c7 → `v.x*C[4].xyz + v.y*C[5].xyz + v.z*C[6].xyz`, kamera
  `C[7].xyz`). Böylece `renderer.cpp`'nin sabit kurulumu yazmaç numaralarıyla birebir taşınır. `#define` varyantları
  (`STEPS`, `LOCAL_LIGHTS`, `SAMPLES_AUTHORED_NOISE`) MSL function constant/ön işlemci makrosu olur. Yazmaç
  haritası: c0–c8 ortak, c9 ışık, c11 march, c12–c35 katmanlar (4×6), c36–c47 authored noise (3×4), c78 yoğunluk
  varyasyonu; yerel ışıklar aradaki aralıkta (`vf_local_lights.hlsli`). Örnekleyiciler: s0 derinlik, s9 yoğunluk
  3B gürültüsü, s10 authored 3B gürültü.
- **Dilim 2 — TAMAM (2026-10-21):** `Shaders/vf_fog.metal` (HLSL'den birebir, `struct Fog`; çalışma anında
  varyant başına derleniyor, kaynak CMake ile `vf_fog_source.h`'ye gömülü), `renderer_metal.mm` (RenderPasses'ın
  march + kompozit kısmı; c0–c98 aynen; fp16 march hedefi; ONE/INV_SRC_ALPHA, RGB; MSAA'da `DEPTH_MS`),
  `engine_mac.mm` (`FrameInputs` motordan), `fog_model`/`noise_volume` orijinal; Mod API `WM_HOOK_WORLD_BEGIN/END`
  ile stok sis 50000'e itilip geri konuyor (yalnız önceki karede sis çizildiyse ve kamera sıvıda değilse).
  Kanıt: `build/autotest/f-off` ↔ `f-on` (Azuremyst, harita 530 → gün/gece'den türetilen 4 katman, 1/4
  çözünürlük 735×403, 16 adım), Metal doğrulaması 0 hata. Bilerek eksik: jitter kapalı (zamansal filtre 4. dilim).
- **Dilim 3 — doğrulandı (2026-10-21):** Classic veri yolu harita 0'da çalışıyor (Northshire: 3 katman; Duskwood
  `.tele duskwood` ile — kanıt `build/autotest/dw-off` ↔ `dw-on`, Darkshire gece sisi). Metal doğrulaması 0 hata.
  **Performans:** 2940×1612, MSAA 2×, kalite 1: sis GPU'ya 4–8 ms ekliyor (Azuremyst 10–11 → 14–19 ms); 60 FPS
  korunuyor ama sınırda — ağır kısım tam çözünürlük kompozit (MSAA'da derinlik kontrolleri + eşleşmeyen
  kenarlarda tam çözünürlük march). Optimizasyon dilim 10'dan öne alınabilir. Test hesabı GM: `.tele`.
- **Dilim 4 — TAMAM (2026-10-21):** zamansal filtre (`vf_temporal`, `vf_history_depth` MSL'de; FilterWithHistory:
  c9–c12 yeniden izdüşüm, c13 ayarlar; iki fp16 geçmiş + RGBA8 paketli geçmiş derinlik; geçerlilik koşulları
  orijinalle aynı: ölçek, ayarlar, harita, ekran efekti yuvası, ileri yön ≥ 45°, projeksiyon, boyut, < 0,25 s,
  < 30 yd hareket), march jitter açık. Duskwood'da dönen kamerada hayaletlenme yok (`build/autotest/t-val`),
  doğrulama 0 hata, 60 FPS, GPU 13,8–16,1 ms.
- **Ayar paneli — TAMAM (2026-10-21, kullanıcı isteği; dilim 9'dan öne alındı):** `Source/panel.lua` (İngilizce;
  WoW şablonları: OptionsSlider, UICheckButton, UIPanelScrollFrame, diyalog çerçevesi), `ALT-F7`
  (`SetOverrideBindingClick`, kalıcı tuş atamalarına dokunmaz) ve `/coavolfog`; Esc kapatır. Lua köprüsü:
  `CoAVolFog_Get/Set/Save/Revert` (oyun bağlamı). Ayarlar anında uygulanıyor (geçmiş `SameFogSettings` ile
  sıfırlanıyor). `SaveConfig` ini'yi yorum ve sırayı koruyarak yazıyor; değeri değişmeyen satırın yazımı
  korunuyor (kayıt sonrası md5 birebir aynı — test edildi). Panelde yalnızca şu ana kadar etkili ayarlar var;
  her yeni dilim kendi ayarlarını ekler. Oyun içinde Enable kapatılırsa stok sis kare sonunda yine geri konuyor.
- **Dilim 5 — TAMAM (2026-10-21):** yerel ışıklar. `engine_lights.cpp` Windows bellek okumasından arındırıldı (seçim,
  sıralama, görüş testi aynen), `local_light_stabiliser` aynen; yakalama `engine_mac.mm`'de `CM2Scene::m_lightGrid`
  (4096 kova, `CM2Light` = orijinal `NativePointLight` alan alan) üzerinden. Renderer: 8 ışık görüş uzayında
  (konum+kesme, renk, zayıflama), `c53` kontrol, lit varyant, kompozitte `WithMeanNoise`. Darkshire: 24 aday →
  8 yüklendi, doğrulama 0 hata (`build/autotest/l-off` ↔ `l-on`, fenerlerin çevresinde sıcak haleler). Panel'e
  LocalLights/Intensity/Phase eklendi.
- **Dilim 6a — sahne kopyası, god rays, glow telafisi (2026-10-21):** sahne kopyası mod içinde, host değişmeden:
  MSAA renk hedefi boş bir geçişte `StoreAndMultisampleResolve` ile modun dokusuna çözümleniyor (MSAA yoksa blit).
  Kompozit artık orijinaldeki gibi "linear light over a scene copy" (`BlendOverSceneCopy`, `FogSceneColour`,
  `BlendGodRays`). God rays: 1/4 küçültme, `vf_ray_mask`, iki `vf_ray_blur` (sabitler orijinal). Glow telafisi:
  `GlowScreenEffectAmount` (ffx açık + etkin efekt `CGWorldFrame::ffxEffectGlow` + kendi CVar'ı; m_glow 0–1; sıvıda
  0). Test için istemciye `WRATHMODERN_TIME_OF_DAY=SS:DD` (yalnız ortam değişkeniyle, `CGWorldFrame`'de gün/gece
  saati). Doğrulama 0 hata. **Açık:** huzmeler güneş kadraj kenarındayken devreye girdi (güç 0,02, sönümlü);
  güneş tam kadrajdayken görsel olarak henüz görülmedi — autotest'te kamerayı güneşe çevirmek zor (Lua'da yaw yok).
- **Dilim 6b — renk derecelendirme TAMAM (2026-10-21):** `vf_grade` (vp_grade.hlsl; eğri tampondan), sisten sonraki
  ikinci sahne kopyası üzerine; güç = `ColorGrading × ForeverLookWeight(kapsama)`, Classic ışığın eğrisi varken,
  hayalet efektinde değil (`CGWorldFrame::ffxEffectDeath`), sıvıda değil. Goldshire 14:00: 0,5 güçle çiziliyor,
  doğrulama 0 hata (`build/autotest/cg-off` ↔ `cg-on`). `ForeverGlow` (glow baytını değiştirme) henüz yok —
  kullanıcı ayarında 0.
- **Sıradaki — dilim 7:** Forever suyu (`water_renderer.cpp` 1517 + `water_fft.cpp` 759 + spektrum/dalga/temas,
  `vw_*.hlsl` ~800 satır): sıvı geçişinde sınıf etiketleme (stencil), GPU FFT, gölgelendirme, köpük, SSR.

**Kabul ölçütü:** her dilimde aynı nokta/saat için Windows CoAVolFog (CrossOver) ↔ WowRe karşılaştırması
(kullanıcı referans kareyi verir), sıfır Metal doğrulama hatası, kare süresi ölçümü (`WRATHMODERN_FRAMESTATS`).

## Faz 1 — native Metal cihazı: TAMAMLANDI (2026-10-21)

`CGxDeviceMetalNative` (MetalContext/Resources/Pipeline/FixedFunction/Formats + `Shader/` çevirici) tek
Metal cihazıdır; `CREATE_CGxDeviceMetal` onu döndürür. `Engine/Source/Gx/CGxDeviceMetal/D3D9/` ve shim
cihazı `CGxDeviceMetal.{h,cpp}` silindi; `grep -r D3D9Metal` boş. `CGxDevice/Mac/CGxDevice.mm` adaptör
sorguları MTLDevice'ta (biçimler `supportsTextureSampleCount:`, kimlik `registryID`, donanım sınıfı en üst
kademe: hwTnL + 8 doku birimi + SM3). Modlar cihazı `GxMetalDevice`/`GxMetalAcquireTargets` ile görür.
`WRATHMODERN_METAL_SHADERS=0` sabit fonksiyon yolunu zorlar (shader karşılaştırması için).

**Bitiren hatalar (gece, DeepSeek):**
1. Vertex sabitleri: `ShaderInfo` boştu, GPU'ya yalnızca c0 gidiyordu → geometri çöküyordu.
2. `ISetTexture` dokuyu `ITexMarkAsUpdated`'den önce önbelleğe alıyordu → siyah yer tutucu kalıcıydı.
3. Pipeline örnek sayısı gerçek renk hedefinden; doku hedeflerine kendi 1× derinlik tamponu.
4. İndeksli çizim, sabitlerin çizimden önce kopyalanması, `m_context`/`DeviceWM`/`ScenePresent`/`m_texTarget`.
5. Mipmap üretimi, bağlı durum aynası.

**Gözden geçirme (2026-10-21 akşam, Claude) — "tamamlandı" denen halde bulunan ve düzeltilen hatalar:**

| # | Hata | Etki | Düzeltme |
|---|---|---|---|
| 1 | `ITexTarget` henüz oluşturulmamış hedef dokuyu yok sayıyordu | Gölge haritası (PCF, 2048² renk + D24) arka tampon (2940 geniş, MSAA) ile 2048² 1× derinliği **aynı geçişte** eşleştiriyordu → geçersiz Metal geçişi; 2×'te sürücü tolere ediyordu, **4× MSAA'da GPU hang + kalıcı siyah ekran** | `CGxDeviceD3d::DeviceSetRenderTarget` gibi gerekirse `ITexCreate` |
| 2 | `DeviceSetFormat` yalnızca hedefleri yeniden yaratıyordu | Video ayarları → Uygula (`RestartGx`): pencere yeniden yapılandırılmıyor, `CGxDevice::DeviceSetFormat` (UI en-boy oranı) atlanıyor, `m_savedDepthTarget` serbest bırakılmış tampona sarkıyordu | Shim gibi: GPU işini boşalt, sarkanları temizle, `ICreateWindow` + hedefler + temel sınıf + caps + vsync + imleç |
| 3 | Scissor dikdörtgeni hedefe kırpılmıyordu; scissor kapalıyken encoder'da öncekisi kalıyordu | Doku hedefinde Metal doğrulama hatası (kare başına yüzlerce); kısmi temizlemeden sonraki çizimler kırpılabiliyordu | Her çizimde hedefe kırpılmış scissor; scissor kapalıysa tüm hedef; boş dikdörtgen çizmez (D3D gibi) |
| 4 | Oklüzyon sorguları sahteydi (`QueryBegin/End/GetParam/GetData` boş) | Güneş parlaması (`DNGlare::GetQueryOcclusion`) hiç görünmez sayılıyordu | `GxMetal::Query`'ye bağlandı; tembel oluşturma, cihaz yıkılırken bağlamdan önce serbest |
| 5 | Donanım imleci yoktu (`m_hwCursor = 0`, `ICursorCreate` hiç çağrılmıyordu) | Oyunda hep sistem oku; WoW'un eldiven/kılıç/el imleçleri yok | NSCursor (Retina 2×, `GxCursorSetScale`), `IUpdateCursor` |
| 6 | `IPollWindow` boştu ve çağrılmıyordu | Pencere elle boyutlandırılınca hedefler/görüş alanı güncellenmiyordu | Shim'deki boyut yoklaması, `ScenePresent` sonunda |
| 7 | `ScenePresent` `WaitForFPSCap` çağırmıyordu | `maxFPS`/`maxFPSBk` etkisiz (pil, ısı) | Eklendi |
| 8 | `gxVSync` yok sayılıyordu (shim'de de) | Dikey senkron kapatılamıyordu | `CAMetalLayer.displaySyncEnabled` |
| 9 | `DeviceReadDepths` boş dizi döndürüyordu | D3D dikdörtgen boyutunda sıfır dizisi döndürür; çağıran indekslerse taşma | D3D ile aynı |
| 11 | İndekssiz çizimin başlangıç köşesine **son bağlanan indeks tamponunun** havuz ofseti ekleniyordu | Yağmur damlaları/sıçramaları (ve indekssiz her çizim) bazı karelerde tampon dışı köşeler okuyordu → gökyüzünde ve yağmurda ekranı kesen çizgi patlamaları (kullanıcı ekran kaydıyla buldu; shim ve CrossOver'da yok) | D3D `DrawPrimitive` gibi `baseVertexIndex`; indekssiz yelpazede taban köşe iki kez eklenmiyor |
| 12 | Viewport ve scissor her zaman **pencere** boyutundan hesaplanıyordu (D3D `DeviceCurWindow`: doku hedefindeyken dokunun boyutu); hedef değişince ikisi kirli işaretlenmiyordu | #1 ile doku hedefleri ilk kez çalışınca tam ekran parlama (`ffxGlow`) yarım/çeyrek dokulara ~2,9× büyük çiziliyor, ekrana "ağaç şekilli sis" olarak biniyordu (kullanıcı `ffxGlow 0` ile doğruladı); gölge haritası viewport'u da aynı hatayla | `DeviceCurWindow`; `DeviceSetRenderTarget` viewport + scissor'ı kirli işaretliyor; scissor renk *veya* derinlik hedefine bakıyor |
| 10 | Çizim yolunda unutulmuş hata ayıklama günlüğü (`stage N texture kind`) | Log gürültüsü | Silindi |

**Doğrulama (autotest, kendi sunucu, Rhon, Apple M4):**
- `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`: giriş → karakter seçimi → dünya → 4× MSAA `RestartGx` →
  dünya: **sıfır doğrulama hatası, sıfır GPU hatası** (düzeltmeden önce ~290 bin hata + GPU hang).
- `RestartGx` (değişikliksiz), 4× MSAA, pencereli 1280×720: hepsi doğru görüntü.
- Oklüzyon sorgusu pozitif kontrol: dünya çizimini saran sorgu kare başına ~37 M örnek saydı; bloklayan
  `GetData` takılmadan döndü (geçici enstrümantasyon, commit'lenmedi).
- İmleç: üretilen NSCursor görüntüsü dökülüp incelendi (WoW eldiveni, renkler doğru, 64 px Retina).
- Kararlılık: 5 dk dünyada (`world@30,world@300`), sıfır GPU/komut tamponu hatası; WTF geri yüklendi.
- Faz 3 #67 gereksinimleri karşılanıyor: vertex sabit dosyası 256 kayıt, sabit fonksiyon 8 ışık.

**Teşhis notu (#11):** önce tampon yarışından şüphelenildi; geçici sayaç (`NoOverwrite` kilidi GPU kullanırken)
`WeatherPacket_vtx`'te sıfır yarış gösterdi ve hipotezi eledi. Ekran kaydının kare kare incelenmesi patlamaların
bir karedeki tüm damlaları birden bozduğunu gösterdi → çizim başına durum hatası. Kullanıcı yağmurda doğruladı.

**Bilinen sınırlar:** stereo yok (macOS); `DeviceReadDepths` D3D gibi sıfır döndürür; pencereli modda
`gxResolution` nokta değil piksel olarak uygulanıyor (shim'deki davranış, ayrı bakılacak).

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
