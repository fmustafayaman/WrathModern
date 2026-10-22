# Oturum 2026-10-22 (öğleden sonra): FPS, ufuk, gölge/aydınlatma

Ortam: MacBook Air M4 (fansız), 2940×1912 (çizim 3042×1912), WowRe dalı, native Metal.
Senin ayarların: `groundEffectDensity 248`, `groundEffectDist 90`, `extShadowQuality 5`, `farclip 2477`,
CoAVolFog açık (sis, su, god rays, renk derecelendirme). Testler kendi AzerothCore sunucunda,
`Loradona` (GM) ile, çoğunlukla Nagrand'da.

---

## 1. Yapılanlar (commit'li, `WowRe` dalı)

| Commit | Ne | Sonuç / kanıt |
|---|---|---|
| `357178b` | **GPU profili**: `WRATHMODERN_GPU_PROFILE=1`. Kare bölümleri (gök, gölgeler, arazi, WMO, ufuk, M2, çimen, sıvılar, glow, arayüz…) ve mod geçişleri **tek zaman çizgisinde** ölçülüyor; 5 sn'de bir stderr'e ms yazıyor. | Ayrı zaman çizgileri çakışan geçişleri iki kez sayıyordu (7.45 ms görünen su kopyası aslında 0.5 ms; 3.7 ms görünen glow aslında ~1 ms). |
| `2d5b16d` | Metal: renk yazımı kapalıyken pixel shader yalnızca alpha testine derleniyor. | Çimen derinlik ön geçişinin ucuz olması için. |
| `853a8f6` | **Çimen iki geçişte**: önce derinlik, sonra EQUAL ile tek gölgeleme; chunk'lar önden arkaya. | Nagrand'da çimen GPU **7.2 → 2.4 ms**; aynı süreçte dönüşümlü ölçümde kare başına **−3.4 ms**. Görüntü farkı ort. 1.1/255 (yalnızca yaprak kenarları). |
| `5a5f1a7` | **Kamera dönerken çimen yeniden kurulmuyor**: görüş dışına çıkan yakın chunk'ların çimeni korunuyor, tampon havuzu mesafeye göre (90 yd'de 168 çift). | Senin oyunundaki ana iş parçacığının ~%31'i buydu; dönüşte CPU'da artık görünmüyor. |
| `d3c606e` | **Ufuk dağlarının görüş açısı**: ufuk sabit 0.6 katsayısıyla (yalnızca 4:3'te doğru) çiziliyordu; 1.59:1'de dünyadan ~%13 geniş. Artık dünya kamerasıyla aynı (`_SG`). | Dönerken kaybolan/gelen mavi dağlar. Aynı açıdan (FlipCameraYaw 207) stok/düzeltilmiş görüntüde dağ artık doğru boyut ve konumda. Orijinal Wow.exe'de de aynı sabit var (0x791170). |
| `2f419cf`, `1ee7d72` | `WOWRE.md` notları. | |

**Ölçülen sonuç (Nagrand, vsync, dönen kamera):** önce 50 fps → sonra ilk ~70 sn **60 fps**, Air ısınınca 56–57.

---

## 2. Bulunan ama çözülmeyen sorunlar

### 2.1 FPS hâlâ düşük (senin oyununda 47 fps) — AÇIK
- Oyununa dokunmadan alınan örnek: **darboğaz CPU**, ana iş parçacığı hiç boşta değil (~21 ms/kare).
  Zaman dağınık: arazi çizimi %19, culling (`CFrustum::Cull` %9), gölge sorguları, M2/parçacık, Metal komut kodlama.
- Olası sebep: **`farclip 2477`**. Client patch'imiz mesafe sınırını tamamen kaldırıyor (`World::ValidateFarClip`
  → 10000); CoAVolFog'un `FarClipMax=1583` ayarı Mac'te etkisiz. Bellek 3.7 GB.
- **Henüz ölçülmedi**: 2477 ile 1583 arasındaki fark (testim senin oyunun açıkken koştu ve geçersiz oldu —
  iki oyun GPU'yu paylaştı). `/console farclip 1583` denemesini senden istedim; sonuç gelmedi.
- GPU tarafında sıradaki büyük kalemler (dönen kamera, Nagrand): arazi ~3.0, **sis composite ~2.4–3.0**,
  çimen ~2.2, gölgeler ~1.5, M2 ~1.1, sis march ~0.7, ufuk ~0.7 ms. Sis composite tam çözünürlükte piksel
  başına ~10 okuma yapıyor (4 tap tam çözünürlüklü derinliği dağınık okuyor) — görüntüyü değiştirmeden
  ucuzlatılabilir.
- Air fansız: ısınınca %20'ye kadar yavaşlıyor. Kesintisiz 60 için ısınmış GPU'da ~12 ms hedef gerekir.

### 2.2 Su yansımaları ekran kenarında bozuluyor — TEŞHİS EDİLDİ, DÜZELTİLMEDİ
- `vw_water.metal` `TraceScreenReflection`: ışın yalnızca **dikeyde** ekranla sınırlanıyor (`OnScreenExtent`),
  sahne kopyası `MIRROR` adreslemeyle örnekleniyor → yandan çıkan ışın aynalanmış görüntüyü yansıtıyor;
  kenar sönümü de yalnızca dikey.
- Orijinal CoAVolFog'da da aynı (`shaders/vw_reflection.hlsli`, `water_renderer.cpp:1369`).
- Düzeltme planı: yatay sınır + yatay kenar sönümü, ışın ekrandan çıkınca gökyüzü yansımasına geçiş.

### 2.3 Yürürken karakterin çevresinde aydınlık çember, dışı "gölge çökmüş" — AÇIK
Senin tarifin: *karakterin çevresinde bir çember mesafesi aydınlanmış, gerisinin aydınlatması yok*.
Konum: Nagrand `X -1149.58 Y 8158.52 Z 4.27`, yön 4.705 (karşıdaki kaya duvarı).

Elenenler (aynı yerde yürüyüş testleriyle):
- Bizim çok yüksek mod bant atlama patch'i (tüm bantlar her kare çizilse de var).
- Bantlar arası gölge düşüren elemesi (kapatılsa da var).
- `shadowCull 0` (var).
- Yakın bant yarıçapı ×2 (var).
- `extShadowQuality 4` (sen denedin: değişmedi) → "çok yüksek" moda özgü değil.
- Bugünkü çimen değişiklikleri (tek geçişli çimende de var).

Gözlemler:
- Gölgeler kapalıyken (`extShadowQuality 0`) kaya tek tip aydınlık.
- Arazi shader'ının gölge bandı seçimi `max(|x|,|y|)` ile — **kare** sınır üretir; senin gördüğün **çember**.
  Bu yüzden sebep büyük olasılıkla gölge haritası değil, **mesafeye bağlı bir aydınlatma/sönüm terimi**.
- Bu oturumdaki gölge bandı testlerinin hiçbiri doğru şeyi ölçmüyordu.

Sıradaki plan: sabit konumda (yürüyüş yok) iki görüntü — normal ve bir aday kapalı — piksel oranıyla çemberin
yarıçapı ve kaynağı. Adaylar: CoAVolFog yerel ışıkları ve `LightStay`, istemcinin nokta ışık patch'leri
(#67 8 ışık), sisin aydınlatma/sönüm kısmı, harita ışığının konuma göre değişen güneş yönü
(`CMap::s_mapLight`).

---

## 3. Yanlış yollar / hatalarım (tekrarlamamak için)

- Kaya gölgesi videosunu ilk seferde dikkatli izlemedim; kendi testimdeki çimen görüntüsüne kaydım.
- `screencapture -v` ile ekran kaydı: oyun önde değilken masaüstünü (Discord, terminal) kaydetti. Kayıtlar
  silindi; bundan sonra yalnızca oyunun kendi ekran görüntüleri.
- Senin oyunun açıkken test koştum: GPU paylaşıldı, senin oyunun da yavaşladı, ölçüm geçersizdi.
- Ufuk için önce "sütun oklüzyonu" (`QueryBuffer`) sonra "bir kare gecikmeli culling" diye tahmin ettim;
  ikisi de orijinal davranıştı, asıl sebep senin "daha dar alanı renderleyen kamera" tespitindi (FOV 0.6).
- Otomatik pop/titreme dedektörlerim gürültülü çıktı; kanıt için aynı açıdan sabit ekran görüntüsü daha güvenilir.

---

## 4. Çalışma ağacındaki commit'lenmemiş kod

- `WoW/Source/Autotest.cpp`: **`WRATHMODERN_AUTOTEST_WALK="başlangıç,süre"`** — karakteri istemcinin kendi
  kontrol bitiyle yürütür (`MoveForwardStart` korumalı olduğu için Lua'dan yürütülemiyor). Testler için
  kalıcı olmalı; commit'lenecek.
- `Engine/Source/ShaderEffect/ShadowCache.cpp`: geçici `PROBE_*` anahtarları (`PROBE_SHADOW_ALL_BANDS`,
  `PROBE_NO_BAND_CULL`, `PROBE_BAND0_X2`). Hepsi varsayılan kapalı; kaldırılacak.
- Oyun klasöründeki `WrathModern-WowRe.app` bu probe'ları içeren bir test derlemesi (anahtarlar kapalıyken
  davranış commit'li hâlle aynı).

## 5. Test araçları (bu oturumda kullanılan)

- Aynı noktaya ışınlanma: `WRATHMODERN_AUTOTEST_LUA='SendChatMessage(".go xyz X Y Z 530 O","SAY")'`
  (önce `.revive` ve `.gm on`; Loradona 8. seviye, Nagrand'da ölüyor).
- Deterministik bakış: `FlipCameraYaw(derece)` (dünyaya girdikten ~8 sn sonra).
- Dönen kamera: `MoveViewLeftStart(0.4)` (korumasız).
- Ölçüm: `WRATHMODERN_FRAMESTATS=1`, `WRATHMODERN_NO_VSYNC=1`, `WRATHMODERN_GPU_PROFILE=1`,
  `COAVOLFOG_GPU_PROFILE=1`; CPU için `sample <pid> 4`.
- Termal etki: ardışık koşular karşılaştırılamaz; A/B'yi aynı süreçte dönüşümlü pencerelerle yap.
