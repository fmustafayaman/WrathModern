# Native render kod tabanı incelemesi — 2026-10-23

## Kısa sonuç

**Evet, kalite düşürmeden denenebilecek somut işler var.** Bu incelemede en düşük kapsamlı aday
uygulandı: kapalı kullanıcı kesme düzlemlerinde vertex shader'ın gereksiz clip-distance çıktısı
kaldırıldı. Isınmış aynı-süreç A/B'sinde kare süresi **22.65 → 21.60 ms (≈%4.6)** azaldı; bu
minimum 60 FPS kanıtı değildir. FFX'in kullanılmayan depth hedefi de denendi; **ölçülebilir kazanç
vermediği için geri alındı**. Sis yol ayrımı ve hedef paylaşımı adayları henüz ölçülmedi. Kaynakta
iş bulunması, o işin GPU darboğazı olduğu anlamına gelmez.

İnceleme, native Metal context/kaynak/shader/pipeline kodu; M2/ground effects batching; gölge
sorgusu/çizimi; arazi/horizon sırası; FFX ve CoAVolFog sis/su geçişlerini kapsadı. Tüm kod tabanının
satır satır incelendiği iddiası yok. Oyun grafik ayarları, çözünürlük, efektler ve mesafeler azaltılmadı.
Bu incelemede yalnızca no-clip vertex varyantı kalıcı olarak uygulandı; çözünürlük, ayar ve efekt
azaltılmadı. Değişiklikler commit/push edildi.

Önceki ayrı deneydeki ~23.35 ms kare ortalamasını 16.67 ms'ye indirmek yaklaşık 6.68 ms daha
az iş/bekleme gerektirir; üstelik ortalama hedefi geçmek minimum 60 FPS'i kanıtlamaz. Aşağıdaki
adaylara hayalî ms kazançları verilip bu bütçe tamamlanmış sayılmamalı.

## Gerçek sahne yapısal ölçümü

Artefakt: `build/autotest/native-render-audit/`.

Oyun kapalıyken başlatıldı. Azure Watch, map 530, `(-4183.4995, -12414.743, 43.44328)`,
yön 1.389416; world +8 sn'de `SetView(2); FlipCameraYaw(79.6)`.
MetalFX=0, 3042×1912, Lua addonları kapalı, native modlar açık. Mevcut `CoAVolFog.ini` kaydedildi
ve koşu sonunda değişmediği doğrulandı. Dünya +35/+55 sn ekran görüntüleri alındı.

Geçici `WRATHMODERN_RENDER_AUDIT=1` sayacı mevcut pass sınırlarını ve draw hazırlıklarını saydı.
**GPU profilindeki gibi yeni pass açmadı, render durumu değiştirmedi.** Sayaçların CPU maliyeti
olduğu için bu koşu bir performans A/B'si değil; yalnızca kullanılan yapıların kanıtıdır.

| Command-buffer serial | Draw hazırlığı | Clip mask = 0 | Derinlik testi/yazımı gerekmeyen draw | VS/PS sabit dosyası upload byte | Kullanılmayan int/bool byte |
|---:|---:|---:|---:|---:|---:|
| 1200 | 2639 | 2639 | 102 | 4,677,504 | 460,224 |
| 1800 | 2621 | 2621 | 102 | 4,615,712 | 455,872 |
| 2400 | 2930 | 2930 | 102 | 5,937,264 | 541,008 |
| 3000 | 2934 | 2934 | 102 | 5,926,432 | 541,280 |

- Bunlar dört örnek karedir, bütün karelerin dağılımı değildir. İlk 600. serial yükleme geçişi
  içerebilir; yukarıdaki tablodan ayrı tutuldu (`audit-summary.json` tüm örnekleri içerir).
- Draw sayısı geometri/üçgen sayısı değildir; modların doğrudan Metal çizimleri bu sayaca dahil değil.
- Sabit byte sayısı `setVertexBytes/setFragmentBytes` ile VS/PS float/int/bool dosyalarına verilen
  uzunlukların toplamıdır. `PixelStateBlock/VertexStateBlock`, mod upload'ları ve fiziksel DRAM
  trafiği bu sayı değildir; GPU'nun kaç byte okuduğunu göstermez.
- 2400. serial'da üç ayrı 1024×512 color + depth geçişi, her ikisinde `Load/Store` ile görüldü.
  Bunlar FFX Box4 ve iki Gauss4 geçişinin kaynak kodundaki sırasıyla örtüşür. FFX bu geçişlerde
  depth test ve depth write'ı kapatır.
- Başlangıçtaki altı `ITextureDepth` ayırmasından sonra yeni ayırma yok; sonuncusu serial 596.
  **“Her kare depth allocation yapılıyor” şüphesi bu koşu için elendi.** Load/store ayrı sorundur.

Ham kayıt `stdout.log`, derlenmiş sayaç farkları `native-probe.patch` / `context-probe.patch`,
özet `audit-summary.json`. Sayaçlar sonra kaldırıldı; iki motor dosyası inceleme öncesi kopyalarıyla
`cmp` üzerinden birebir doğrulandı. `ninja -C build/mac WrathModern` başarılı; sayaçsız uygulama
imzalanıp dağıtıldı. WTF'nin **411 dosyası SHA-256 ile birebir geri yüklendi**.

## Uygulanan: no-clip vertex varyantı

`PixelShaderVariant.m_noAlphaTest` düzeltmesine ek olarak, `VertexInputSignature` ve
`FixedVertexKey` anahtarlarına `m_noClip` eklendi. `m_hw.m_clipPlaneMask == 0` iken üretilen
vertex fonksiyonu `D3DVaryingsNoClip` döndürüyor; altı `[[clip_distance]]` alanı ve yazımları
kaynakta yok. Aktif maske yolunda `D3DVaryings` ve mevcut yazımlar aynen kalıyor. Donanımın
normal frustum clipping'i etkilenmiyor.

`WRATHMODERN_METAL_CLIP_SHADERS=0` referans, `1`/varsayılan yeni yol, `alternate` her 600
command buffer'da değişim ve `metalclipshaders: serial ..., on/off` kaydı. Bu bir kalite ayarı
değildir.

### Doğrulama

`Tools/MacDev/test-clip-shaders.mm` gerçek çevirici ve fixed-function üreticisini kullanır;
Metal API+GPU validation açıkken:

- iki vertex yolu da Metal ile derlendi,
- no-clip kaynağında `out.clipDistance` yok; aktif kaynakta var,
- shader cache anahtarları varyantları ayırıyor,
- fixed pipeline ile kapalı maske çıktısı **birebir aynı** (clip-on örnek 1.00).

`build/autotest/clip-shaders-validation/`: Azure Watch'ta Metal API+GPU validation açık,
MetalFX=0, native 3042×1912; giriş/dünya geçti, validation failure yok. Açılışta clip maskesi
sıfır olmayan draw'lar da bu koşuda mevcut.

`build/autotest/perf-azure-clip-ab/`: oyun kapalıyken, aynı kamera ve mevcut `CoAVolFog.ini`,
Lua addonları kapalı, GPU profile kapalı. İlk iki blok shader/pipeline ısınması olduğu için
atıldı; phase 4–12'de geçiş içermeyen 5 sn pencereleri kullanıldı.

| Isınmış 5 sn ortalamaları | Referans (off) | No-clip (on) |
|---|---:|---:|
| Pencere / kare | 7 / 1551 | 7 / 1622 |
| Kare süresi | 22.65 ms | **21.60 ms** |
| CPU | 9.60 ms | 9.50 ms |
| GPU command-buffer | 27.86 ms | 26.79 ms |

Kare süresi **≈1.05 ms (%4.62)** azaldı; CPU neredeyse aynı, fark GPU tarafında. Bu tek ısınmış
koşunun 5 sn ortalamalarıdır; p99/minimum FPS veya bütün sahnelerde kalıcı oran değildir. Ekran
görüntüsünde 44 FPS gösteriliyor; **60 FPS'e ulaşmadı**. Statik üst dünya bölgesinde ortalama
RGB farkı 0.207/255 (medyan 0, p99 5/255); animasyon/zaman farkı vardır, bit-eşitlik iddiası
değildir. WTF 411 dosya SHA-256 ile geri yüklendi; `test-proposed-fixes.py` yeniden geçti.

Bu sonuç, aşağıdaki diğer adayların zaten kazanç sağlayacağı anlamına gelmez.

## Adaylar — önerilen sıra

### 1. Kapalı user-clip için gerçek vertex shader varyantı — **uygulandı**

**Kanıt:**
- `Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderTranslator.cpp:35`: ortak vertex çıkış yapısı
  altı `[[clip_distance]]` içeriyor.
- `ShaderTranslator.cpp:1482` ve `MetalFixedFunction.cpp:239`: her varyant bu altı çıkışı
  dinamik `state.flags.x` koşuluyla yazıyor. Mask 0 olduğunda değer 1; fakat kaynakta clipping
  çıkışı ve koşulu duruyor.
- Yukarıdaki dört kararlı karede draw hazırlıklarının %100'ünde clip mask 0. Yükleme örneğinde
  28 draw mask sıfır değil; global olarak clipping kapatmak bu yüzden yanlış.

**Değişiklik:** Vertex cache anahtarına `clip-disabled` özelliği; yalnızca mask 0 yolunda hem
çıktı alanını hem ilgili yazımları çıkarmak. Aktif maske yolunu aynı bırakmak. Bytecode ve
fixed-function yollarının ikisini kapsamak. Kullanılmayan sıradan varyings'i derleyici zaten
atabilir; bu öneri özellikle rasterizer'a bağlı user-clip çıkışı içindir.

**Kalite koşulu:** Aktif kesme düzlemleri, su altı/üstü bölünmüş modeller ve mevcut homogeneous
near/far clipping korunmalı. Bu donanımın normal frustum clipping'ini kapatmak değildir.

**Sonuç:** Uygulandı ve yukarıdaki A/B'de ≈%4.6 kare süresi kazancı verdi. Aktif maske yolu ve
kalite korunuyor; diğer sahnelerde oran farklı olabilir.

### 2. Sis composite'ini hızlı ve tam hesap yollarına ayırmak

**Kanıt:** `Mods/CoAVolFog/Shaders/vf_fog.metal`:
- `DepthAwareUpsample()` (1032): 2×2 derinlik kontrollü ağırlıklı örnekleme.
- `FogWithoutMatchingTap()` (988): eşleşme yoksa ek düzlem/eğrilik/ışık testleri ve nearest tap.
- Son fallback (1007), aynı tam çözünürlüklü composite shader'ından `IntegrateFogAtDepth()` çağırıyor.
- Integrator (642), `stepWeight[STEPS]` (660), layer döngüleri ve yerel ışık integrasyonunu taşıyor.

Hafif upsample ile 24-adım march aynı shader'da. Divergence, register/spill veya occupancy
maliyeti olası; **kaç piksel fallback kullanıyor ve derleyici kaç register ayırıyor henüz ölçülmedi**.
Ayrıca üç yardımcı yol aynı 2×2 derinlik örneklerini kaynakta tekrar okuyor; bunları bir kez
hesaplamak başka bir küçük adaydır, fakat compiler'ın ortak hesapları atıp atmadığı kontrol edilmeli.

**Değişiklik:** Önce fallback piksel oranını/ısı haritasını, compiler register/spill bilgilerini ve
aynı girdinin offscreen replay maliyetini ölç. Ardından hızlı composite + yalnızca fallback
piksellerini işleyen maskeli/compact ikinci yol deneyini yap. Orijinal karar eşikleri, jitter,
24 adım, ışıklar, temporal history ve tüm formüller aynen korunmalı.

**Kalite koşulu:** Yeni texture'da ara sonuç gerekiyorsa önceki float hesapları fp16'ya sessizce
kuantize etme; sample/depth MSAA semantiğini değiştirme. Maskede bir pikseli kaçırmak görüntü
hatasıdır. Fallback'i kapatmak veya düz bilinear'a çevirmek bu öneri değildir.

**Risk/kabul:** Orta-yüksek; ekstra dispatch/pass ve bellek maliyeti kazancı silebilir.
Kayıtlı *aynı frame girdileriyle* eşitlik, sonra statik/hareketli kamera A/B şart.
Daha önce reddedilmiş düşük çözünürlüklü composite/nearest-only/flat-bilinear deneyi tekrarı değil.

### 3. FFX'te kullanılmayan depth attachment'larını kaldırmak — **denendi, kazanç yok**

**Kanıt:**
- `Engine/Source/FFX/FFX.cpp:605–630`: `SetupGx()` depth test ve depth write'ı kapatıyor.
- `CGxDeviceMetalNative.mm:505–537`: offscreen color için yine `ITextureDepth()` atanıyor.
- `MetalContext.mm:692–719`: bağlanan depth/stencil yüklenip saklanıyor.
- Yapısal ölçüm: üç 1024×512 glow geçişinde gerçekten depth `Load/Store` var.

**Değişiklik:** Açıkça depth kullanmayan bütün bir post-process pass'ine attachment'sız yol;
uyumlu pipeline key/depth state; pass sonunda önceki dünya depth'i eksiksiz geri yükleme.
Tek tek draw'larda attachment aç/kapatıp fazladan pass üretmekten kaçın.

**Kalite koşulu:** World depth'i sis, su, isim etiketleri veya sonraki draw için gerekiyorsa
sakla. Tüm dünya depth'ine `DontCare`/memoryless yapmak doğru değil. Bu öneri kullanılmayan
FFX yardımcı depth'iyle sınırlı.

**Deney sonucu:** FFX boyunca no-depth hedef bayrağı uygulandı; `WRATHMODERN_METAL_FFX_DEPTH=alternate`
ile aynı-süreç A/B yapıldı (`build/autotest/perf-azure-ffx-depth-ab/`). Isınmış, geçişsiz
pencerelerde off **21.03 ms**, on **20.99 ms** (≈%0.22, gürültü); CPU 9.29→9.12 ms, GPU 26.42→26.41 ms.
Metal API+GPU validation temizdi ve görüntü bozulmadı, fakat kazanç yok. **Değişiklik geri alındı**;
FFX.cpp tekrar HEAD ile aynı. Attachment sıkıştırması teorik byte tasarrufunu gerçek ms kazancına
çevirmiyor. Bu aday kapatıldı.

### 4. Sis → glow arasındaki tam ekran kopyayı hedef paylaşımıyla kaldırmak

**Kanıt:** `Mods/CoAVolFog/Source/renderer_metal.mm:672–688` sis composite'ini dünya color'ına
çiziyor. Hemen ardından `FFX::Finish()` (`FFX.cpp:369–379`) o color'ı `s_full[0]` içine kopyalıyor;
glow daha sonra bundan okuyor. Native backbuffer zaten örneklenebilir bir offscreen texture.

**Değişiklik:** Render hedefi yaşam sürelerini düzenlemek: sisin doğru FFX girdisine doğrudan
çizmesi veya FFX'in mevcut offscreen color'ı sahiplik/feedback tehlikesi olmadan girdi kabul etmesi.
Glow formülü, çözünürlük, gamma ve UI sırası değişmez. Bu, önceki *sis içi scene-copy framebuffer
fetch* deneyiyle aynı değişiklik değil; mod/motor sınırındaki sonraki kopyadır.

3042×1912 BGRA8 için bir tam kopyanın mantıksal read+write hacmi ~46.5 MB/kare; bu gerçek DRAM
trafik veya zaman ölçümü değildir. Mevcut POT texture boyutu/viewport-UV dönüşümleri, MSAA resolve,
underwater/ghost fallback ve başka modların girdileri korunmadan salt alias yapılamaz.

**Risk/kabul:** Orta-yüksek kapsam; önce kaynak/hedef grafiği ve aynı-frame görsel replay.
`FFX::CreateTex()` ayrıca NPOT destekli Metal'de hâlâ POT boyutları ayırıyor; kullanılmayan
padding'i kaldırmak bellek adayıdır ama sampling/clamp kenarlarını değiştirebileceği için otomatik
“sıfır görsel fark” sayılmaz.

### 5. Sabit dosyası upload'larını küçültmek — CPU adayı

**Kanıt:** `CGxDeviceMetalNative.mm:1228` dirty aralığı kopyalayıp boolean dirty tutuyor;
2933–2948'de herhangi bir değişiklikte `[0, constantCount)` yeniden `set*Bytes` ediliyor.
Integer/bool dosyaları da shader'ın `ShaderInfo.m_usesIntConstants/m_usesBoolConstants` değerlerine
bakılmadan yükleniyor. Ölçüm: yalnızca bu gereksiz int/bool dosyaları ~0.46–0.54 MB/kare.

**Değişiklik:** Önce kullanılmayan dosyaları bind etme; daha sonra değişmeyen global sabitlerle
per-draw kemik/malzeme sabitlerini versioned shared-buffer ring/snapshot yaklaşımıyla ayır.
Shader değişiminde önceki binding'in yanlış kullanılmaması ve GPU hâlâ okurken aynı slotun
üzerine yazılmaması zorunlu. Eski içeriği değiştirip yalnızca dirty byte'ları yazmak tek başına güvenli değil.

**Risk/kabul:** İlk adım düşük kapsam; kapsamlı ring orta. CPU encode ve kopyalanan byte sayısı
ölçülmeli. GPU-bound Azure Watch'ta bunun tek başına 60 FPS getirmesi beklenmemeli; ısınma/CPU
bütçesine katkı ayrıca ölçülür.

### 6. M2 doodad için gerçek Metal instancing — daha büyük mimari iş

**Kanıt:**
- `Engine/Source/Model2/M2Shared.cpp:338–452`: aynı modelin vertex/index verileri instance sayısı
  kadar çoğaltılıyor; vertex içindeki bone index'i ofsetleniyor.
- `InitializeSkinProfile()` (1135): instance sınırı 16-bit indis ve shader bone kapasitesine bağlı.
- `CM2SceneRender.cpp:604–646`: kemik matrislerini sabit register'lara paketleyip çoğaltılmış
  geometry'yi normal draw ile çiziyor.
- Native backend `drawIndexedPrimitives` yolunda `instanceCount/instance_id` kullanılmıyor.

**Değişiklik:** Başlangıçta yalnızca aynı mesh/material ve uygun rijit doodad gruplarında tek
geometry + per-instance dönüşüm buffer'ı + Metal instanced draw; mevcut batching gruplarının
ışık/fog/shadow koşullarını koru. Sonra gölge caster yolu değerlendirilebilir.

**Sınır:** Modelin her instance'ı için vertex shading ve görünür piksel işi yine yapılır. Mevcut
kod zaten batch ediyor; “draw sayısı instance sayısı katı azalır” ya da “N kat hızlanır” denemez.
Asıl aday kopyalanmış geometry belleği, batch kapasitesi ve CPU submission maliyetidir.

**Risk:** Yüksek kapsam; animasyon, farklı bone combo'ları, alpha sıralaması ve light/fog
farklılığı olan instance'lar körlemesine birleştirilemez. İlk müdahale olarak seçmezdim.

## İkincil işler ve elenen kestirmeler

- **Gölge sorgusu:** `WoW/Source/WorldClient/ShadowQuery.cpp:467` bütün `CMap::entityList`'i
  aktif band'larla karşılaştırıyor. Uzamsal broad-phase ve tam invalidation'lı aday-listesi önbelleği
  CPU adayı; caster'ları azaltmadan yapılmalı. Mevcut `ShadowCache.cpp:584` dış band güncellemelerini
  zaten 2/4 kareye yaymış; bunu daha da seyrekleştirmek “kaliteyi koruyan yeni optimizasyon” değildir.
- **İlk kullanım takılmaları:** `MetalPipeline.mm:132–163` shader *fonksiyonlarını* prewarm ediyor;
  `PipelineCache::Pipeline()` (300–359) gerçek pipeline'ı ilk draw'da senkron derliyor. PSO/binary
  archive prewarm kötü kareler için aday; ısınmış GPU-bound ortalamayı çözmez.
- **Query beklemesi:** Native `QueryGetData()` bloklayabilir; fakat bulunan dünya glare çağırıcısı
  `DayNight.cpp:3082` önce `available` kontrol ediyor. Sırf fonksiyonda wait var diye bunu ana
  darboğaz ilan etmek doğru değil.
- **Dünya depth'i memoryless yapmak / genel `early_fragment_tests` zorlamak:** Su/sis depth okur;
  alfa-testin depth/stencil etkisi değişebilir. Körlemesine uygulanmamalı.
- **Terrain front-to-back, genel M2 depth prepass, sis içi framebuffer-fetch:** Önceki deneylerde
  kazanç sağlamadı. Yeni kanıt olmadan aynı denemeleri döngü halinde tekrarlamak öncelik değil.

## Ölçümün düzeltilmesi

`MetalContext::SetProfileSection()` render pass'ini bölüyor. Sadece bu profilin parça süreleriyle
öncelik/savings toplamı çıkarılmamalı. `FrameStats` ise 5 saniye ortalamaları ve >25 ms logları
veriyor; **16.67–25 ms aralığındaki kötü karelerin dağılımını kaybediyor**. `cpu = wall - tracked wait`
aktif CPU cycle ölçümü değil, ölçülmüş beklemeler çıkarılmış duvar zamanıdır.

Bir sonraki implementasyondan önce/asılı olarak, pass bölmeyen opt-in ham kare kaydı:
serial/frame id, CPU begin/submit, drawable/semaphore wait, compile süresi, GPU begin/end,
presentedTime ve warmup/test fazı. Buffer/frame sayıları ile GPU tamamlanmasının gecikmesi
eşleştirilmeli; profil ve validation FPS kabul koşusunda ayrı tutulmalı. p50/p95/p99 ve en kötü
kare, ısınmış tekrarlı A/B ve hareketli kamera olmadan “minimum 60” sonucu çıkarılmamalı.

**Uygulanan ilk küçük adım:** clip-disabled vertex varyantı; ≈%4.6 kare süresi kazancı, 60 FPS değil.
**Sıradaki ölçüm:** pass bölmeyen ham kare kaydı ve p99/en kötü kare.
**Sonraki GPU odaklı deney:** fallback oranı kanıtlandıktan sonra sis shader yol ayrımı.
Hiçbir adım MetalFX'e veya çözünürlük/efekt azaltmaya dayanmıyor.
