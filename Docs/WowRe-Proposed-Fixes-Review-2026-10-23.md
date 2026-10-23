# WowRe önerilen düzeltmeler raporu — güncel kaynak karşılaştırması

Kaynak: kullanıcının paylaştığı **WrathModern — Proposed fixes for the WowRe branch.pdf**
(8 sayfa, Eric, 22 Ekim 2026). Yerel dosya: `~/Downloads/WrathModern — Proposed fixes for the WowRe branch.pdf`.
İncelenen dal: `WowRe`, HEAD `15b9562`; çalışma ağacındaki Metal/etkileşim/test değişiklikleri ayrıca dikkate alındı.
`git fetch --all` ile `origin`, `github` ve `upstream` güncellendi. `origin/WowRe` ve `github/WowRe`
`704628b8` noktasında; yerel HEAD bunların 21 commit ilerisinde, gerisinde değil.
`upstream/main` (`b1c71b8b`) ve `github/main` (`cd29da38`) üzerinde de shader `ops[2]` ve
NPC `InternalNewNode(displayId,0,0)` hataları mevcut; upstream shader döngüsü hâlâ sınırsız
`m_textureCount` kullanıyor. Skin-profile koruması yerel `3e3eb0fb` commit'inde, uzak WowRe'de henüz yok.

PDF, eski `620da2d1…` uygulama hash'ini ve başka makinedeki ölçümleri anlatır.
Bu makinede rapordaki çökme/FPS/bellek sayıları yeniden ölçülmüş sayılmaz.
İlk inceleme eski bulguları güncel kodla ayırdı. Kullanıcının uygulama talebi üzerine kalan
**dört düzeltme de uygulandı**; mevcut skin-profile koruması tutuldu.

## Uygulanan değişiklikler ve doğrulama

- **Fix 1:** `EvtSched.cpp` içinde Apple'a özel runtime push/pop kullanan RAII pool eklendi.
  `SchedulerThreadProcProcess()` iterasyonunun tamamını ve tüm erken dönüşlerini kapsar;
  initialize/update/timer/input/poll/queued/idle/paint ve destroy dispatch'leri bu pool içinde kalır.
  C++ dosyası ObjC++'a çevrilmedi, diğer platformların kod yolu değiştirilmedi.
- **Fix 2:** NPC portresi `.New(displayId, HASHKEY_DWORD{displayId}, 0, 0)` ile yaratılıyor;
  hash/key Storm tarafından atanıyor. Mevcut purge/recycle davranışı korunuyor.
- **Fix 3:** Combiner döngüsü `opCount = min(textureCount, 2)` ile sınırlanıyor;
  son-stage `0x4000` UV testi de `opCount` kullanıyor. Dosyanın `textureCount` değeri değişmiyor.
- **Fix 4:** Yerel koruma zaten mevcut, yeniden yazılmadı. Testte 1–8 profil / beş bone bütçesi
  için PDF'nin seçim kuralıyla eşdeğer çıktı verdi; 0 profil ayrıca reddediliyor.
- **Fix 5:** `CurrentCursor()` görünmez **veya detached** durumda şeffaf imleci seçiyor.
  Ortak `DetachCursor()` içindeki hide/unhide çifti kaldırıldı; durum/association güncellendikten
  sonra her iki yönde cursor refresh yapılıyor. Bu ortak yol mouse mode, focus ve destroy'u kapsar.

Doğrulama:

- `ninja -C build/mac WrathModern AwesomeWotlk`: başarılı; uygulama imzalanıp oyun klasörüne kopyalandı.
- `python3 Tools/MacDev/test-proposed-fixes.py`: gerçek güncel fonksiyon gövdeleriyle izole
  fixture'lar, gerçek Storm hash-table template'i ve ASan/UBSan. **480 shader vakası** + explicit
  shader korunması; **40 skin seçimi** + sıfır profil; **4000 cache tekrar erişimi**; cursor truth
  table/idempotence/reattach/fallback; pool sınırında **10000 nesnenin serbest kalması** geçti.
  Fixture fiziksel cursor association çağrısını stub'lar; test kullanıcının imlecini ayırmaz.
- `build/autotest/pdf-fixes-validation/`: Metal validation + pool high-water açık, giriş ve Dalaran
  testleri başarılı. Yakındaki NPC hedeflenip portresi **786 kez** güncellendi, **0 test Lua hatası**;
  0.5 saniye aralıklarla mouselook aç/kapat yapıldı. Validation assertion/çökme yok.
- `build/autotest/pdf-fixes-addons-validation/`: normal addon ayarlarıyla ikinci dünya testi de geçti.
- Autorelease high-water: ilk koşuda **8641**, ikinci koşuda **9714** pending release.
  Bunlar validation açık kısa koşuların yerel sonuçlarıdır; aynı makinede düzeltme öncesi pool
  high-water alınmadığı için rapordaki 1.247 milyon → 6900 kazanımı burada ölçülmüş sayılmaz.
- WTF'nin **126 dosyası** iki koşudan sonra SHA-256 ile birebir geri yüklenmiş doğrulandı.
- `git diff --check`: başarılı. Fullscreen Direct/Composited geçişi/VRR kazancı bu makinede ölçülmedi.

## İlk inceleme sonucu (yamalar uygulanmadan önce)

| PDF maddesi | Güncel kaynakta durum | Kanıt / sınır |
|---|---|---|
| 4 — >4 skin profili, negatif tablo indeksi | **Zaten düzeltilmiş** | `3e3eb0fb` (`Model2: clamp the skin profile table index, fixing the Dalaran crash`). `CM2Shared::Initialize()` artık `GetSkinProfileIndex()` çağırır; bu metot profil sayısını 4'e sınırlar ve 0 profili de reddeder. Böylece ilk dört profil PDF'nin `min(k,3)` yaklaşımıyla aynı seçilir; tekrarlanan sonraki 21-bone limitleri ilk eşleşmeyi değiştirmez. Yeniden yama gerekmiyor. |
| 3 — shader combiner yerel dizisinde taşma | **Kaynakta hâlâ mevcut** | `Engine/Source/Model2/M2Shared.cpp`, `SubstituteSimpleShaders()`: `uint32_t ops[2]` dizisine `j < batch->m_textureCount` döngüsüyle `ops[j]` yazılıyor. `textureCount > 2` durumunda sınır aşılır. Bu oturumda güncel istemcide bir crash tekrar üretilmedi. İleride düzeltme uygulanırsa yalnızca etkin combiner döngüsü ve son-stage UV kontrolü 2 ile sınırlanmalı; dosyadaki `m_textureCount` değiştirilmemeli. |
| 2 — NPC portre cache anahtarı | **Kaynakta hâlâ mevcut** | `WoW/Source/UIUtil/PortraitButton.cpp`, `RenderPortraitTexture()`: oyuncuda `.New(hash,key,0,0)`, NPC'de `.InternalNewNode(displayId,0,0)`. `Engine/Source/Storm/H/stpl.h` incelendi: `InternalNewNode` anahtarı/hash'i atamaz, `New` ikisini de atar. Aynı portreye sonraki erişim cache miss üretebilir. Texture purge bulunması, hash node hatasını düzeltmez. Raporun 20 GB ölçümü bu makine için doğrulanmış değildir. |
| 1 — scheduler autorelease pool | **İlgili döngüde yok** | `Engine/Source/Event/EvtSched.cpp`, `SchedulerThreadProcProcess()`/`SchedulerThreadProc()`: dispatch döngüsünde pool yok. `Os/Mac` altındaki yerel `@autoreleasepool` blokları tüm dispatch/render süresini kapsamaz. Pool sayısı/bellek yüksek su seviyesi ayrıca ölçülmeli; rapor bunu da asıl NPC portre sızıntısından ayrı bir hijyen düzeltmesi sayıyor. ObjC kaynaklarının yaşam süresi incelenmeden körlemesine pool eklenmemeli. |
| 5 — fullscreen cursor hide/unhide takılması | **Mekanizma mevcut, kod şekli farklı** | `Engine/Source/Os/Mac/Input.mm` ortak `DetachCursor(bool)` üzerinden `[NSCursor hide/unhide]` kullanıyor. `GameWindow.mm::CurrentCursor()` yalnızca `s_gameCursorVisible` kontrol ediyor; detached durumu burada yok. PDF'nin fonksiyon başına binary adresleri doğrudan uygulanamaz. Bu M4'te Direct/Composited geçişi ve hitch henüz ölçülmedi; cursor focus/relative/destroy dengesi birlikte test edilmeli. |

## İlk incelemede belirlenen öncelik (sonrasında yukarıdaki değişiklikler uygulandı)

1. Profil düzeltmesini tekrar uygulama.
2. Shader taşması ve NPC cache anahtarı somut kaynak hatalarıdır; ayrı, küçük değişiklikler ve regresyon testleriyle ele alınabilir.
3. Scheduler pool için önce/sonra pending-object/bellek ölçümü ve ObjC kaynak yaşam süresi kontrolü gerekir.
4. Cursor değişikliğini fullscreen, odak kaybı/geri gelişi, kamera tuşları ve çıkış senaryolarında doğrulamadan tamamlanmış sayma.

## Bu incelemeyle karıştırılmaması gereken devam eden optimizasyon

`CGxDeviceMetalNative` için render geçişi bazlı durum/buffer bağlantı önbelleği ayrı bir değişikliktir.
PDF'deki beş düzeltmeden biri değildir. Native modlar açık, Lua addonları kapalı geçerli A/B koşusu:
`build/autotest/metal-state-ab-valid/`.

- Aynı binary/süreçte `WRATHMODERN_METAL_STATE_CACHE=alternate` ile her 600 command buffer'da aç/kapat.
- `WRATHMODERN_AUTOTEST_NO_ADDONS=1`; ekran görüntüsünde yüklenen Lua addon sayısı **0**, test Lua hatası **0**.
- Yükleme/glue içeren ilk bloklar hariç, aynı sabit bakışta üç tam kapalı blokta `IDrawPrepare` ortalaması
  **1.356 ms/buffer**, üç tam açık blokta **1.245 ms/buffer**: yaklaşık **0.11 ms / %8.1** hazırlık maliyeti azalması.
  İş miktarı küçük ölçüde farklıdır (yakındaki animasyon/NPC'ler); bu rakam bir bütün-oyun FPS artışı değildir.
- İzlenen pipeline/depth/cull/fill/bias/blend/viewport/scissor/vertex-buffer bağlamalarının yaklaşık **%91.9**'u
  açık bloklarda gereksiz tekrar olarak atlandı. Shader constant, doku ve sampler çağrıları bu orana dahil değildir.
- Aynı süreçte ardışık iki sabit bakış görüntüsünde tam-kare ortalama mutlak RGB farkı **0.237/255**, medyan **0**.
  Bu tek başına piksel eşitliği kanıtı değildir; karakter/NPC animasyonu ve saat değişir. Görsel kontrolde geometri/ışık kaybı görülmedi.
- Ayrı `metal-state-on` ve `metal-state-off` koşuları Metal API validation ile giriş + oyun içi ekranlarını geçti.
- Validation'ın her iki modda da ortaya çıkardığı eski framebuffer-only drawable'a blit hatası `MetalContext::Present` içinde
  render fallback'i seçilerek giderildi. Mipmap blit'lerinden sonra encoder edinilmesi de düzeltildi.
- Test aracı WTF klasörünü geri yükledi; **126 gerçek dosyanın içeriği SHA-256 ile birebir karşılaştırıldı**.

### Geçersiz / sonuç çıkarılmayacak koşular

- `metal-state-ab-noaddons`: kullanıcı yanlışlıkla istemciyi kapattı; yalnızca bir kare var.
- `metal-state-ab` ve `metal-state-ab-clean`: test betiği Lua'dan gizli `cameraDistanceE` CVar'ını yazmayı deniyordu;
  tekrarlayan hatalar oluştu. Bu koşulardaki kare süreleri FPS kazancı veya ısıl yavaşlama kanıtı **değil**.
- Geçerli koşul `SetView(2)` kullanır; gizli kamera CVar'larına Lua'dan erişmez.
