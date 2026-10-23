# Azure Watch native performans hedefi: ikinci inceleme

## Sonuç

**Kalite kaybı olmadan 60 FPS henüz kanıtlanmadı; fakat bunun imkânsız olduğu da kanıtlanmış değil.**
Önceki başarısız deneyler yalnızca o uygulamaları eler. Sis çözünürlüğünü düşürmek veya MetalFX kullanmak
zorunluymuş gibi sonuç çıkarmak doğru değil. Bu inceleme sırasında MetalFX, sis ayarları, çizim mesafesi
ve oyun çözünürlüğü değiştirilmedi. Değişiklikler commit/push edildi.

## Önceki ölçümlerin sınırı

- `perf-azure-native-noprofile/` içindeki yaklaşık 22 ms kare süresi gerçek bir başlangıç ölçümüdür;
  ancak sürekli minimum 60 FPS veya p99 ölçümü değildir.
- `MetalContext::SetProfileSection()` GPU profili açıkken render encoder'ı kapatır. Dolayısıyla profil,
  yalnızca pasif bir sayaç değildir: render-pass sınırlarını ve Apple GPU'nun tile üzerinde tutabildiği
  işi değiştirir. Profil açık geçiş süreleri, profil kapalı kareye doğrudan toplanamaz.
- Ayrı süreçlerdeki sisli/sissiz koşular izole bir sis A/B'si değildir. Örneğin kayıtların son GPU
  profillerinde `perf-azure-native/` M2 opaque 8.15 ms, terrain 4.61 ms iken
  `perf-azure-fog-flat-profile/` M2 opaque 4.31 ms, terrain 2.29 ms gösterir. Yalnızca sis geçişleri değil,
  dünya çiziminin maliyeti de ciddi değişmiştir. Bu farkın nedenini ayırmadan “sis kesin 7.7 ms;
  mutlaka 2 ms'ye düşmeli” denemez. Shader hatalı/sis çizilmeyen bir koşu üretim karşılaştırması değildir.
- Bundan sonra performans kabulü, profil kapalı aynı-binary/same-process A/B ile; teşhis ise ayrıca
  GPU profiliyle yapılmalıdır. Isınma, kamera, dünya ayarları ve MetalFX=0 doğrulanmalıdır.

## Atlanmış somut hata: kapalı alfa testi hâlâ shader'da

`CGxDeviceMetalNative::IRsSendToHw(GxRs_AlphaRef)` zaten `m_hw.m_alphaTest` bilgisini tutuyordu.
Ancak çizim sırasında `PixelStateBlock.m_flags[1]` daima `SHADERCOMPARE_GREATEREQUAL` oluyordu;
`m_alphaTest` kullanılmıyordu. Hem bytecode çeviricisi hem fixed-function üreticisi her piksel
shader'ına koşullu `discard_fragment()` ekliyordu.

Orijinal `CGxDeviceD3d.cpp` ve `CGxDeviceOpenGl.cpp`, AlphaRef <= 0 iken alfa testini kapatır.
Native yol bu davranışı uygulamıyordu. Sıfır eşiği, normal 0..1 alfada görünüşte zararsız olsa da
potansiyel discard'ı shader'dan kaldırmaz; negatif alfada kapalı testin semantiği de farklıdır.
Apple'ın tile tabanlı GPU'sunda potansiyel discard, gizli yüzey elemesini kısıtlayabilecek bir adaydır.
**Gerçek oyun kazancı ölçülmeden bunun FPS artışı olduğu söylenemez.**

### Uygulanan değişiklik

- `PixelShaderVariant` ve `FixedPixelKey` içine `m_noAlphaTest` eklendi; shader cache anahtarları bu
  varyantı ayırıyor.
- Alfa testi kapalıyken, yalnızca backend'in eklediği alfa-test discard'ı kaynakta hiç üretilmiyor.
- Alfa testi açık yaprak/çimen/ağaç/diğer malzemeler eski test ve eşiklerini koruyor.
- Bytecode'daki açık `TEXKILL` talimatları **her iki varyantta da korunuyor**; shader'ın kendi clipping'i
  kaldırılmıyor. Renk, fog, blend, derinlik, örnek sayısı veya çözünürlük değiştirilmiyor.
- Referans/dinamik yolda kapalı test için `SHADERCOMPARE_ALWAYS`, açık test için önceki
  `SHADERCOMPARE_GREATEREQUAL` kullanılıyor. Böylece iki yol aynı motor semantiğine sahip.
- `WRATHMODERN_METAL_OPAQUE_SHADERS=0`: dinamik referans; `1`/varsayılan: özel varyant;
  `alternate`: her 600 command buffer'da değişim, `metalopaqueshaders: serial ..., on/off` kaydı.
  Bu anahtar bir kalite ayarı değildir.

## Doğrulananlar

- Önce yalnızca `ninja -C build/mac Gx` derlendi. Kullanıcının oyun süreci kapandıktan sonra
  `ninja -C build/mac WrathModern AwesomeWotlk` de geçti; uygulama imzalandı ve oyun dizinine dağıtıldı.
- `Tools/MacDev/test-opaque-shaders.mm`: gerçek native çeviriciler ve sistem Metal derleyicisiyle
  offscreen GPU regresyonu. Oyun başlatmaz, WTF/oyun dosyalarını değiştirmez.
- `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/test-opaque-shaders`:
  **1200 bit-eşit kapalı-alfa/TEXKILL/fog karşılaştırması + 300 açık-alfa eşik vakası PASS.**
  Normal ve alpha-only bytecode, fixed-function/specular, negatif/sıfır/taşan alfa ve eşik çevreleri
  kapsanır. Her iki shader cache anahtarının farklı olduğu da kontrol edilir.

```sh
clang++ -std=c++17 -fobjc-arc -I. Tools/MacDev/test-opaque-shaders.mm \
  Engine/Source/Gx/CGxDeviceMetal/MetalFixedFunction.cpp \
  Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderTranslator.cpp \
  Engine/Source/Gx/CGxDeviceMetal/Shader/ShaderBytecode.cpp \
  -framework Metal -framework Foundation -o build/tests/test-opaque-shaders
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 build/tests/test-opaque-shaders
```

## Oyun içi aynı-süreç A/B

Kullanıcının oyunu kapandıktan sonra `build/autotest/perf-azure-opaque-ab/` kaydı alındı:
MetalFX=0, GPU profile/Metal validation kapalı, Lua addonları kapalı, native modlar açık,
3042×1912, aynı sabit kamera ve mevcut CoAVolFog ayarları. `CoAVolFog.ini` koşu dizinine
kaydedildi; sis 761×478/24 adım, 8 yerel ışık, far clip 2478.4 ve max fog distance 4013 korundu.
Varyant her 600 buffer'da değişti; toplam koşu yaklaşık 131 saniye sürdü.

Isınmış bölümde 8. (kapalı) blok, simetrik olarak 7. ve 9. (açık) bloklarla çevrelendi.
Mod geçişi içeren 5 saniyelik pencereler elendi. Ham kaynak `stdout.log`, sınıflandırma
`window-analysis.json`, özet `summary.json` içindedir.

| 5 sn pencere ortalamaları | Dinamik referans | Alfa testi kapalı shader varyantı |
|---|---:|---:|
| Kullanılan pencere / kare | 2 / 413 | 4 / 858 |
| Kare süresi | 24.25 ms | 23.35 ms |
| CPU (bloklu bekleme hariç) | 10.54 ms | 10.24 ms |
| GPU command-buffer süresi | 29.40 ms | 28.39 ms |

Kare süresi bu alt karşılaştırmada **~0.90 ms (%3.72)** azaldı. Bu tek koşunun 5 saniyelik
ortalamalarıdır, minimum FPS/p99 veya tüm sahnelerde sabit kazanç iddiası değildir. GPU buffer
süreleri örtüşebilir; kare süresine toplanmamalıdır. İlk sıcaklık yükselişi sırasında mutlak süreler
ayrıca değişti; eski gece koşularıyla doğrudan oran hesaplanmadı.

İki dünya görüntüsünde statik üst dünya bölgesinin (x850:2650, y150:800) ortalama mutlak RGB farkı
0.166/255, medyanı 0, p99'u 3/255; `image-comparison.json`. Ağaç/çimen/sis kaybı gözlenmedi.
Ekran görüntüleri 80 saniye aralıklı olduğundan animasyon/ışık/zaman değişir; tüm dünya için
bit-eşitlik veya aynı-zaman kalite kanıtı değildir. Bit-eşitlik yukarıdaki offscreen regresyona aittir.

`build/autotest/opaque-shaders-validation/`: yeni yol açık, Metal API + GPU validation açık;
giriş ve Azure Watch dünya testi geçti. Oyun içi panel native **3042×1912 → 3042×1912**, MetalFX off
gösterdi; addon/native durum assert'leri olan testte hata/validation failure kaydı yok.
PDF regresyon paketi de yeniden geçti (`build/tests/proposed-fixes-final.log`).
Her iki koşudan sonra mevcut WTF'nin **411 dosyası SHA-256 ile birebir geri yüklendi**.

## Henüz doğrulanmayanlar / sonraki adaylar

**Native sürdürülebilir minimum 60 FPS hâlâ sağlanmadı.** Ham kare süreleri/p99, uzun yürüyüş/dönen
kamera ve aynı-zaman dünya görsel karşılaştırması bu dar değişiklik için henüz yapılmadı.
Bu, ölçülmüş bir kalite kısmayan kazançtır; hedefin tamamlandığı anlamına gelmez.

Sis composite shader'ının derinlik eşleşmesi bulunmayan piksellerde tam march'a geri dönmesi,
aynı büyük shader içindeki hafif/ağır yolların register/occupancy etkisi ve render-pass birleştirme
hâlâ kaliteyi kısmadan araştırılabilecek ayrı adaylardır. Bunlar bu değişikliğe dahil edilmedi.
