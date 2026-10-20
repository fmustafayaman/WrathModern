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
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/mac WrathModern
# örnek mod
cmake -S Mods/Samples/DepthFog -B build/mods/DepthFog -G Ninja && ninja -C build/mods/DepthFog
```

`WRATHMODERN_GAME_DIR` **verilmedi**: verilirse derleme `WrathModern.app`'i oyun dizinine kopyalar ve
oradaki `warmane-login` derlemesinin üzerine yazar. Canlı test için ayrı bir uygulama adı/kopya
yöntemi kurulana kadar oyun dizinine kopyalama yok (bkz. "Açık sorular").

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

## İlerleme günlüğü

### 2026-10-21 — başlangıç

- `WowRe` dalı + `../WrathModern-WowRe` worktree oluşturuldu (`macos` @ `a3d749f`).
- Mod deneyi taşındı; temiz yapılandırma ile derlendi (sonuç commit mesajında).
- Başlangıç commit'i: aşağıdaki "Başlangıç noktası" satırına bakın.

Başlangıç noktası: `WowRe` dalının ilk commit'i (`git log --reverse a3d749f..WowRe | head`).

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

## Açık sorular

- Canlı test için hangi sunucu kullanılacak (AzerothCore self-host mu, Warmane mi)? `WowRe` istemcisi
  `OSX` raporlar; Warmane bunu reddeder (logon result 9).
- Oyun dizinine `WowRe` derlemesi hangi adla kopyalanacak (`WrathModern-WowRe.app` önerisi).
