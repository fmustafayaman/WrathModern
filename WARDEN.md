# Warden — durum ve devam notları

Amaç: native macOS/arm64 WoWReMac istemcisinin Warden etkin sunucularda (Warmane dahil) tam değerli istemci gibi davranması.
Yol: istemci tarafı Warden'ı emülasyonla çalıştırmak. Hile/sahte cevap YOK — sunucunun modülü gerçek kod olarak çalışıyor; "honest-compat".

## 288 baytlık challenge — çevrimdışı düzeltme, 2026-10-24

20. dakikadaki `0x006B3856` durması **yeni bir oyun fonksiyonu değil**, henüz
çözümlenmemiş `GetCurrentThreadId` import'uydu. Temiz Wow.exe SHA-256:
`aa63a5750d60ef16746c686b3d5e26876d98953eab08b1c026cd0faf78e88cb8`. PE
OriginalFirstThunk, `KERNEL32.dll!GetCurrentThreadId` için IAT `0x009DF218`
gösteriyor; dosyadaki `0x006B3856` bir **import-name RVA**. Onu VA gibi çalıştırınca
alakasız bir fonksiyonun epiloguna düşülüyordu.

- Tam paket, `failure-20m/realm-packets.log` içindeki `14697987` satırından alındı:
  `captures/challenge-288.bin`, SHA-256
  `4cb66fa666c0063f8fa6886db6a24f62e898b3ed164dcf8272c8a443eb05ad1a`.
  Önceki buck nonce'u `B5383598`; çözülünce nonce `670850AB`, kod uzunluğu **278**.
- Kod, `GetCurrentThreadId` → `OpenThread(0x4A)` → `CreateThread` →
  `WaitForSingleObject(INFINITE)` çağırıyor. Yardımcı x86 kodu ana thread'in
  `CONTEXT_DEBUG_REGISTERS` durumunu okuyor, bekleyen kodun devamını dört NOP ile
  değiştiriyor ve cache'i temizliyor. Gönderilen 28 bayt: API'nin gerçek BOOL sonucu
  ve DR0/DR1/DR2/DR3/DR6/DR7; cevabı yine teslim edilen kod oluşturuyor.
- `BuckCodeEmuThreads.inl`: tek Unicorn makinesi üzerinde ayrı register context/stack
  taşıyan kooperatif guest thread'leri. Hook içinden iç içe `uc_emu_start` yok;
  bekleme dış yürütücüye bırakılıyor. Context, canlı Unicorn CPU kaydından okunuyor;
  sabit sıfır/başarı cevabı veya host ARM register'ıymış gibi gösterim yok.
- Altı argümanlı Win32 çağrıları için host frame dizisi de genişletildi; önceki
  altı-dword dizi, dönüş adresinden sonraki altıncı argümanda sınır dışına çıkıyordu.
- Yakalanan challenge artık gerçek worker'ı çalıştırıp **36 bayt** cevap üretiyor.
  Taze guest CPU'nun DR6/DR7 değerleri `FFFF0FF0`/`00000400`; bunlar sıfırlanmıyor.
  Testte guest kodunun yazdığı **sıfır olmayan DR0/DR3 de aynen geri geliyor** ve
  ikinci worker/challenge sonrasında korunuyor. Henüz canlı sunucu kabulü iddiası yok.
- Regresyonlar: **12 framework + 6 Lua + 5 PE-runtime + 8 session-verifier** testi geçti.
  Bilinmeyen CONTEXT grubu, desteklenmeyen sonlu bekleme ve worker bellek hatası
  açıkça başarısız; yarım/bypass cevabı gönderilmiyor. Yürütme beş saniye ile sınırlı.
- Kullanıcının pragmatik kapsam tercihi `AGENTS.md`'ye kaydedildi: isteğe bağlı,
  uygulanamayan özellikler açık sabit “desteklenmiyor” mesajıyla atlanabilir;
  sunucu/Warden kontrolünü sahte başarıyla atlama veya başarısız testi başarılı
  sayma değildir.

**Sıradaki kanıt:** yeni uygulamayla tek PID/GUID üzerinden en az 1800 saniye canlı
oyun, hareket/UI, kesintisiz gerçek render ve bütün challenge cevapları. Önceki
20 dakikalık başarısız koşu korunur ve aşağıda ayrıntılanır.

## 30 dakikalık kararlılık çalışması — 2026-10-24, hedef henüz sağlanmadı

Kullanıcının hedefi: aynı native oturumda **en az 30 dakika kesintisiz dünya bağlantısı ve
sorunsuz oyun**. Süre süreç açılışından değil aktif oyuncu + gerçek render doğrulamasından
sayılıyor. Tek PID/GUID, ilerleyen kareler, canlı sunucu trafiği, normal hareket/UI ve
uyarı/kopma olmaması birlikte doğrulanmadan hedef tamamlanmış sayılmaz.

### Gerçek framework ve içerik yürütmesi

- 264 baytlık 0x2E9 yalnız guest'te GUID/XY yazımlarına ayrılıyor; ortaya çıkan bootstrap ve
  0x4B8 dispatcher **gerçek x86 olarak** çalışıyor. Dört install segmenti kendi VirtualAlloc,
  SetMessageHandler ve Send2 çağrılarını yapıyor. Opcode taraması/uydurulan token kaldırıldı.
- Tek emülatör, realm→world boyunca aynı handler/anahtar/global durumunu taşıyor. 0x41B
  cevabı handler'ın kendisinden geliyor; tekrar kurulum içerik sürümlerini gerçek globalden
  okuyup token'ı yeniden kullanıyor. Unicorn yazılan kodun cache'ini gerçekten temizliyor.
- 0x41D hook'u native `ClientInitializeGame` noktasında çalışıyor: native DBC satırları asıl
  32-bit layout'a çevrilip guest'e aktarılıyor, sunucunun parser'ı düzenliyor, değişmiş gerçek
  satırlar native DB'ye geri çevriliyor. Map/MapDifficulty/LfgDungeons/Item/ItemExtendedCost/
  CurrencyTypes/Spell/Achievement, toplam 8 tablo. Bilinmeyen schema/opcode başarısız olur.
- FrameXML patch hook'u orijinal ExecuteFile→ExecuteBuffer çağrı yerinde çalışıyor;
  WorldStateFrame.lua gerçek yüklü buffer üzerinde değişiyor. Geçici guest bellekleri serbest
  bırakılıp tekrar kullanılıyor. SMemReAlloc eski tahsis sınırını aşan kopya yapmıyor.
- Toolhelp yalnız guest'te gerçekten eşlenen Wow.exe'yi raporluyor; tarihsel sabit Wine
  DLL listesi kaldırıldı. Import thunk'larının stdcall yığın temizliği de düzeltildi.
- PE IAT `0x009DF388`, **GetDiskFreeSpaceExA** çıktı; FILETIME değil. Gerçek dosya sistemi
  toplam/boş kapasitesi okunuyor. Önceki köprü ~134 PB ve sürekli değişen bir tarih değeri
  gönderiyordu. Bu değişiklik donanım kimliği değiştirme/ban aşma denemesi değildir.

### Unauthorized software uyarısının somut tutarsızlığı

İlk yeni kurulum koşusu (`stability-30m-20261024-sY63GV`, PID 16670) dünyaya girdi ama
**154.295 saniyede uyarı** aldı. Kullanıcının 19:15:50 ekranı ve `SMSG 0096` içindeki gerçek
uyarı metni arşivlendi; bu oturum başarılı sayılmadı.

- Warden plaintext `packet-043` (85 B) içinde `39 00 b8 15 d4 00 08`: istemcinin
  `0x00D415B8+8` belleğini okuyor. Eski outbound plaintext:
  `020b00124269e6000000000000000000e9e9` — durum baytından sonra **8 sıfır**.
- BuckCodeEmu zaten bu globalleri dolduruyordu; bağımsız WardenEmu doldurmuyordu. Asıl
  `OsFindTextSectionBounds` (0x86B510..0x86B591) `.text` bölümünün ImageBase/RVA/VirtualSize
  hesabını yapıp iki globale yazar. `ClientImageRuntime.h` bu hesabı PE'den iki makinede de
  yapıyor; ne sabit cevap ne yakalanmış hash kullanılıyor.
- Düzeltmeden sonraki aynı replay cevabı:
  `020b00402814f40000104000b3e39d00e9e9` — `0x00401000..0x009DE3B3` gerçek PE sınırları.
  Bu kontrolün çıktısı değişti, diğer cevaplar aynı kaldı. Eski hatalı canlı kayda göre
  **1 mismatch beklenir**; bunu gizlemek için replay'in beklenen cevabı değiştirilmedi.
- Sonraki koşu (`stability-30m-runtime-20261024-YdmpWs`, PID 17441) **12 dakika** boyunca
  uyarı/kopma olmadan kaldı; native ileri/geri girdi testi ve konum değişimi doğrulandı.
  Akıcılık düzeltmesini denemek için bu oturum bilinçli kapatıldı, 30 dakika diye sunulmadı.

### Render takılmasının kaynağı ve doğrulaması

Warden'ın gerçek ADT hash'leri sırasında 2 saniyeyi aşan event-loop boşlukları görüldü.
`uc_emu_start(..., count=500000000)` Unicorn'da **her komuta tanı callback'i** ekliyor;
SHA1'ın kendisini hesaplamak yerine atlamak gibi bir optimizasyon yapılmadı.

- Varsayılan artık instruction counter olmadan gerçek JIT + **5 saniyelik wall-time üst
  sınırı**. Sentinel'e ulaşmayan çağrı başarısız, yarım cevap geçerli sayılmıyor.
- Tam dal izi de yalnız `WARDEN_TRACE_BLOCKS=1` ile açık; hata snapshot'ları korunuyor ve
  offline trace ile yeniden oynatılabiliyor. `WARDEN_STEP_LIMIT` tanı için hâlâ kullanılabilir.
- Aynı 14 cevap, instruction counter açık: **12.6946 s**; bounded JIT: **1.9167 s**,
  **6.62x hızlanma**. Her 14 payload birebir aynı. Eski canlı kayda karşı yalnız yukarıdaki
  başlangıç-global düzeltmesinin 1 mismatch'i var.
- Bounded-JIT native uygulamasıyla yeni 30 dakikalık test **19:40:27** civarında başlatıldı,
  PID 18078. Aktif arşiv: `$HOME/Library/Logs/WrathModern-Warden/stability-30m-jit-20261024-85aOqJ/`.
  Denetim sonucu aşağıda; bu koşu başarısızdır.

### 20. dakikadaki gerçek hata

- Audit **1210.222 saniyede** başarısız oldu: dünya buck challenge sayısı **9**, cevap sayısı
  **8**. `world buck challenges were missing or not all replied to` ve
  `emulator/bridge failure found in the live logs` kapıları tetiklendi.
- `14697990`: teslim edilen kod `0x006B3856` istemci fonksiyonunu çağırdı; bu köprü yok.
  `14697991`: `delivered function 03d10000 failed at 006b3856`; ardından
  `SMSG 041b (288 bytes): delivered handler failed`. Bu başarısızlık gizlenmedi/başarılı
  cevapla doldurulmadı. Fonksiyon henüz tanımlanmadı; sonraki iş offline çoğaltıp gerçek
  köprüsünü eklemek.
- Öncesindeki minimum aralık FPS **56**, ortalama yaklaşık **59.4**, en büyük event-loop
  boşluğu **401 ms** idi. Normal hareket ve karakter paneli görüntüyle doğrulandı; bu
  ölçümler cevaplanamayan challenge'ı veya eksik 30 dakikayı başarılı hale getirmez.
- Oyun logları, trafik ve anahtarlar yukarıdaki özel arşivin `failure-20m/` altına kopyalandı.
  Anahtarlar/kimlik bilgileri Git dışında kalır. PID **18078 artık çalışmıyor**.
- Yeni canlı oturum yerine önce aynı 288 baytlık challenge offline çalıştırılmalı.

### Doğrulama araçları

- `test-buck-lua.py`: 6 test.
- `test-buck-framework.py`: 7 test (gerçek bootstrap/install, DB/FrameXML, tekrar kurulum,
  zincirli challenge, gerçek disk, gerçek guest module listesi, bozuk paket reddi).
- `test-client-image-runtime.py`: 5 test (gerçek PE, kesik/bozuk başlık, exact bölüm adı,
  taşma/boş bölüm).
- `verify-session.py <private-run> --live`: tek PID/GUID, >=1800 s kesintisiz native ready,
  kare ilerlemesi ve >=15 FPS aralıklar, <=2 s event-loop takılması, taze sunucu trafiği,
  Warden/buck cevapları, uyarı/kopma/yeniden giriş olmaması ve gerçek konum değişimi.
  Bu denetim 30 dakikalık test penceresini kapsar, oyunun bütün içeriğine garanti değildir.

## Temiz Warmane kurulumu — 2026-10-24 canlı test (önceki kayıt)

**Giriş ve dünya render'ı çalışıyor; uzun süreli bağlantı henüz çözülmedi.** Kullanıcının
`/Volumes/Yaman SSD/Warmane` dizinindeki temiz 3.3.5a kurulumuna ayrı
`WrathModern-Warden.app` dağıtıldı. `WoW_Wrath`, diğer worktree'ler ve modlarına dokunulmadı.
Kimlik bilgileri yalnız repo dışındaki, `0600` izinli test dosyasında; buraya yazılmaz.

### Somut düzeltme: 0x4B9 tek Lua dizgesi değil

Kullanıcının paylaştığı [anti-cheat incelemesi](https://backend-development.net/post/warmane-anti-cheat),
Lua işleyicisinin birden çok parça çalıştırdığını işaret etti. Asıl doğrulama kaydedilmiş
`blob-893-seg1-350.bin` kodundan: `+0x32..+0xB7`, her NUL'dan sonra `ESI != EDI` iken tekrar
çözüp çalıştırıyor; BL anahtarı NUL'da da `decoded + 1` ile ilerliyor, sıfırlanmıyor.
REA'nın raw-binary/Ghidra sağlayıcısı yapılandırılmamıştı; bu dar disassembly mevcut Capstone
ile yapıldı. Yazıdaki açıklamalar tek başına protokol kanıtı değildir (örneğin bizde doğrulanan
cevap sabiti hâlâ `0xD0BE63D1`).

- Eski kod ilk NUL'da duruyor, kalan Lua parçalarını sessizce atıyordu. Ortak
  `BuckCodeEmu::DecodeLuaScripts` artık tüm paketi çözüyor; `BuckData` her parçayı sırayla
  `FrameScript_Execute` ile çalıştırıyor. Son parçası tamamlanmamış paket çalıştırılmıyor.
- 1808 baytlık kayıt: **799 / 404 / 126 / 19 / 454** baytlık beş parça. Canlı dünya paketi
  6772 bayt ve **dokuz parça**; dokuzu da `buck-data.log`'da işlendi.
- `python3 Tools/Wardendump/test-buck-lua.py`: **6 test geçti** (kayıtlı beş parça, tek parça,
  parça sınırında devam eden/saran anahtar, boş parçalar, boş betik ve bozuk paketler).
- `SetDataTable` ve `WRATHMODERN_BUCK_TABLE_OVERRIDE` yolu kaldırıldı. Tanı API'si
  `ReadFrameworkState` yalnız gerçek guest durumunu okur; yakalanmış tabloyu yazmaz.
- Mevcut BuckCodeEmu WIP'i korunurken eksik `HeapAllocPages` bildirimi derlemeyle uyumlandı.
  `WrathModern`, `WardenSmoke`, `BuckSmoke`, yerleşik lifecycle ve aynı emülatörde 40→660
  testi geçti. Resmî 40 baytlık fixture hâlâ birebir 12 baytlık cevabı üretir.

### Canlı kanıt ve kopma

İlk oturumda karakter girişine karşı yeniden `003B` geldi. Sonraki çalıştırmada:

- `3990822`: `CMSG 003D` (Maheta); `3991137`: **`SMSG 0236`** (map 1). Camp Narache'deki
  karakter ve oyun UI'si yalnız bu uygulamanın pencere görüntüsüyle de doğrulandı. Yeniden
  girişin kabul edilmesinin Lua düzeltmesinden kaynaklandığı kanıtlanmış değil.
- Buck-data cevapları: **40→12, 660→112, 192→1984, 84→48, 92→32, 80→16** bayt.
- `4102982`, 92 baytlık kontrol: `0x0050D2B7+7`, `0x0040100A+6`, `0x0050A288+9` —
  istenen **22/22 bayt** temiz Wow.exe ile aynı, ardından **iki sıfır padding baytı** var.
  `verify-client-reads.py` bu padding'i yanlışlıkla fark sayıyordu; istenen veri ile hizalama
  ayrıldı, uzunluk ve sıfır padding de doğrulanıyor. Bu araç mevcut untracked WIP olarak kaldı.
- `4223065`, 80 baytlık kontrol: `0x00AA33DC+8` (`.rdata`) — **8/8 bayt** aynı.
- `4225893`: **DISCONNECTED**, dünya onayından **234756 ms** sonra. Son kontrole yakınlık
  onun reddedildiğini kanıtlamaz; kopmanın nedeni henüz bulunmadı. Yeni giriş döngüsü başlatılmadı.
- Tam oturumun offline replay'i **14/14 CMSG birebir**, ADT dosyaları gerçek Storm/MPQ
  okumalarıyla hash'leniyor (45/53/77 baytlık cevaplar dahil). Bu, canlı istemciyle aynı
  hesabı yaptığımızı gösterir; sunucunun sonucu kabul ettiğini göstermez.

Tam trafik, oturum anahtarı, modül, challenge'lar, ekran kanıtı ve replay çıktısı repo dışında:
`$HOME/Library/Logs/WrathModern-Warden/warmane-20261024-VZqrz5/`.
Yeni login'den önce bu arşiv korunmalı; oyun dizinindeki tek anahtar dosyası üzerine yazılıyor.

**Sıradaki offline iş:** 0x2E9/0x4B8'in kurulumunu ve native içerik/DB hook'larını tamamlamak.
Şu an istek opcode'u taranıp handler segmentleri çalıştırılıyor; bütün kurulum çalışmıyor ve
84 baytlık kontrolün okuduğu framework durumu eksik. Mevcut sabit Wine modül listesi de native
sürecin doğrulanmış envanteri değil; bunu gerçek eşlenmiş ortamla tutarlı hale getirmek gerekir.
Başlıktaki `InstallFramework`/`Dispatch`/hook API'leri önceki WIP'ten, henüz uygulanmış değil.
Kontrolü geçirtmek için sabit cevap/tablo/modül listesi ayarlamak çözüm değildir.

Kurulumu gerçek kodla sürdürmek için ek bulgu: canlı 264 baytlık 0x2E9'da oyuncu sayısı 0,
flag sayısı 249314. `Packet_MSG_BATTLEGROUND_PLAYER_POSITIONS` (0x0054B3F0) yazım düzeniyle
ilk 16 GUID/XY çifti guest'te yeniden yerleştirilince `0x00DD1000..0x00DD107F` bootstrap'ı ve
`0x00DD15A0..0x00DD161F` kodu çıkıyor. Bootstrap **`CGBattlefieldInfo::ShutdownGame`**
(0x0054E220) çağırıyor, VirtualAlloc ile tampon alıyor, 0xDD15A0'dan 0x5E baytı oraya kopyalıyor
ve 0x4B8'i `base+0x20` handler/param ile kaydediyor. Dispatcher ilk segmenti `param+0x3E`'ye
kopyalayıp çalıştırıyor; install segmenti de sonraki handler/veri segmentlerini tüketiyor.
PE import tablosundan doğrulanan slotlar: `0x009DF1B4=IsDebuggerPresent`,
`0x009DF1C0=GetCurrentProcess`, `0x009DF294=FlushInstructionCache`. Sonuncusu Unicorn
translation cache'ini de gerçekten temizlemeli. Bu çözümleme henüz canlı yürütücüye bağlanmadı.

## Güncel dal düzeni — 2026-10-24

- GitHub projesi **[WoWReMac](https://github.com/fmustafayaman/WoWReMac)**; varsayılan
  geliştirme/taban dalı **`WowRe`**, bu dal **`WoWReMac-Warden`**.
- **`epoch-native` ayrı kalır**; Epoch relay/auth çalışması ne ana dala ne Warden
  dalına taşınır. Mevcut worktree klasörleri ve kaydedilmemiş çalışmalar korunur.
- Eski `macos` dalı ana tabanda tamamen bulunduğu doğrulandıktan sonra silindi.
- `WowRe` @ `3c7f82c0` üzerine 55 Warden/buck-data commit'i rebase edildi. Native Metal,
  mod altyapısı, istemci yamaları ve autotest korunurken Warden/autologin akışı üstüne eklendi.
- Sonraki taban güncellemeleri `git rebase WowRe` ile alınır. Sunucuya özel kod `WowRe`'ye
  geri taşınmaz; `WowRe` worktree'sindeki kaydedilmemiş değişikliklere dokunulmaz.
- Eski, ilgisiz sunucuya özel `ClientExtensions` 0x52E bildirimi ve ayrı sunucu notu kaldırıldı.
  Tanımsız paketler artık `Logs/unhandled-packets.log` dosyasına yazılır.
- Aşağıdaki 2026-10-17 kayıtları emülatörün tarihsel hata ayıklama kanıtlarıdır; güncel
  projenin başka bir sunucuya bağlı olduğu veya yeni canlı giriş gerektiği anlamına gelmez.
- **Doğrulama (commitli kaynak, önceki BuckCodeEmu WIP'i hariç):** `WrathModern`,
  `WardenSmoke`, `BuckSmoke`, `LuxShoulderCam`, `AwesomeWotlk`, `CoAVolFog` derlendi.
  Yerleşik Warden lifecycle, resmi 40 baytlık challenge'ın birebir 12 baytlık cevabı,
  aynı emülatörde 40→660 sırası (12→112 bayt) ve 0x4B9 Lua çözümü geçti.
  Native Metal/mod kaynakları `WowRe` ile birebir; canlı sunucuya giriş yapılmadı,
  dağıtım kapalı tutuldu (`WRATHMODERN_GAME_DIR=""`).

> **GÜNCEL DURUM (2026-10-17 öğle, en altta güncelleme var):** Sunucu modülü artık **yükleniyor, oluşturuluyor, initialize oluyor, sunucu paketlerini tüketiyor ve cevap üretiyor** — ama modülün `Process`'i okunamayan belleğe erişip fault veriyor ve `LCMapStringA` anlamsız argümanlarla çağrılıyor. El sıkışma tamamlanmadığı için karakter listesi gelmiyor.
> **Takıldığım yer:** Gerçek oturum olmadan (doğru oturum anahtarı) bu iki hatayı offline çoğaltamıyorum. Bu yüzden tanı dump'ları eklendi; **bir kez daha login** gerekiyor — sonrası tamamen offline. Ayrıntı: en alttaki "NEREDE TAKILDIK" bölümü.


> **GÜNCEL DURUM (2026-10-19 akşamı): Warmane'de karakter listesi VE oyuna giriş ÇALIŞIYOR.**
> Eksik parçalar kapandı: (1) `SMSG_REALM_SPLIT` onayı + karakter isteğinin el sıkışma sonrası
> tekrarı, (2) Warmane'in özel kod dağıtım paketlerine (0x2E9/0x4B8) karşılık gönderilen
> buck-data istekleri (0x41D/0x41C/0x41B; 0x41B'de istemci adresi token'ı — sıfır gönderilince
> sunucu bağlantıyı kesiyor), (3) modülün `.zdata` hesap-adı stub'ı (client RVA 0x9D1000) için
> `AccountName()` köprüsü (resmi istemcinin cevabıyla byte-byte aynı). Ayrıntı ve kanıtlar:
> aşağıdaki "WARMAME" bölümü ve `Logs/realm-packets.log`.

## WARMAME (2026-10-19) — karakter listesine giden yol

Warmane, Warden el sıkışmasından sonra realm bağlantısında **özel kod paketleri** gönderir
(`02e9` = 264 B, `04b8` = 2037/338/893/502 B; ikisi de stock tabloda kullanılmayan opcode'lar).
Resmi istemci bu bloblardaki x86 kodu çalıştırır; kod `SetMessageHandler` ile buck-data
işleyicilerini kurar ve `WDataStore` + `Send2` ile **istek paketlerini** yollar. Sunucu ancak bu
isteklerden sonra veri paketlerini ve **karakter listesini** (`003b`) gönderir.

Yakalanan resmi istemci trafiği (`realmhook-client.dll`, NetClient::ProcessMessage/Send):
```
RECV 04b8(2037) -> SEND 041d | payload 00000000
RECV 04b8(338)  -> SEND 041c | payload 00000000
RECV 04b8(893)+(502) -> SEND 041b | payload <istemci tampon adresi, ör. 00004c05>
RECV 041d(10838), 041c(361), 04b9(1808), 003b(karakter listesi), 041b(40) ...
```
Native istemci aynı istekleri gönderir (`RealmConnection::HandleCustomCode`, blob içindeki
`push <opcode>` deseninden opcode'u okur; 0x41B için makul bir istemci adresi token'ı üretir).
Token sıfır gönderilirse sunucu bağlantıyı sessizce keser — kanıtlandı.

Ek olarak: realm bağlantısındaki tüm paketler `Logs/realm-packets.log`'a yazılır
(`NetClient::ProcessMessage/Send`, yalnız realm bağlantısı için; teşhis amaçlı). Warden
modülünün `.zdata` stub çağrısı (client RVA 0x9D1000) artık `WardenEmuServices::AccountName()`
ile giriş yapılan hesabın büyük harfli adını döndürür; resmi istemcinin cevabıyla birebir aynı
(`020b00...0009LORADONAA`).

Bilinen sınır: Warmane'in buck-data kod akışının 0x41B challenge'ları artık gerçekten
çalıştırılıyor (aşağıya bakın); 0x41D/0x41C veri paketlerini işleyen sunucu işleyicileri hâlâ
kurulmuyor, bu paketler yalnız loglanıyor. Karakter listesi ve dünyaya giriş çalışıyor.

### 0x41B challenge'ları sunucunun kodu ÇALIŞTIRILARAK cevaplanıyor (2026-10-19 akşamı)

Oyun içinde ~1 dk sonra düşmenin ve karakter seçiminde kopmanın nedeni yanlış 0x41B cevabıydı.
Resmi istemcinin `realmhook-client` kaydıyla protokol birebir çözüldü:

- **İlk anahtar = CMSG 0x41B isteğinde gönderdiğimiz token.** Gövde dword başına sağa 1 bit
  döndürülerek XOR'lu; çözülünce `[nonce][codeLen][code][answer]` çıkıyor.
- **0x41B işleyicisi** (0x4B8 bloğunun son segmenti) token adresinde çalıştırılıyor. İşleyici
  `CDataStore::GetInt` ile **nonce**'u okur (0x401170; GetSize değil), kendini
  `SetMessageHandler(0x41B, ctx, nonce)` ile yeniden kaydeder, `GetInt` ile codeLen'i okur,
  kodu `handler+0x154`'e kopyalar ve çağırır. Kod son bir `GetInt` ile kuyruktaki **answer**
  dword'ünü okur; cevap `[0x41B][0xD0BE63D1][answer]` olur ve nonce anahtarıyla kodlanır.
- **Doğrulama:** Resmi kayıttaki 40 baytlık challenge (token `0x05660000`, nonce
  `0x4AC5131D`) bizim makinemizde birebir aynı cevabı üretiyor:
  `1b 04 00 00 cc 70 7b 9a ec 6d 08 e0` (`0x456AE462` answer'ı). Bu challenge
  `Tools/Wardendump/captures/challenge-official-40.bin` olarak saklandı.
- **660 baytlık challenge (sistem parmak izi):** Karakter listesinden sonra gelen 660 baytlık
  challenge'ın 652 baytlık kodu fiziksel bellek, işlemci sayısı, bilgisayar/kullanıcı adı,
  ekran adaptörü, HW profile GUID'i, registry `InstallDate` değeri, CPUID marka dizisi ve
  FILETIME toplayıp 112 baytlık cevap kuruyor. `BuckCodeEmu` bunun için `OsGetPhysicalMemory`,
  `OsGetNumberOfProcessors`, `OsGetComputerName`, `OsGetUserName`, `FindDisplayDevice`,
  LoadLibrary/GetProcAddress/FreeLibrary, RegOpenKeyEx/RegQueryValueEx/RegCloseKey ve FILETIME
  köprülerini içeriyor; CPUID markası Wine'ın Apple Silicon'daki değeriyle
  (`VirtualApple @ 2.50GHz `) dolduruluyor. Cevap 112 bayt (resmi uzunlukla aynı) ve ilk 64
  bayt resmi kayıtla birebir: bellek `0x3FFFFF000`, 10 işlemci, `MAC`, `MAC`, `Apple M4`,
  `{12340001-...}` GUID'i.
  Kaydedilmiş challenge: `Tools/Wardendump/captures/challenge-660.bin`.
- **İstemciye özgü tuzak (çözüldü):** Aynı emülatör örneğinde önce 40'lık sonra 660'lık
  challenge çalışınca ikincisi eski 26 baytlık kodu çalıştırıyordu; Unicorn, hook içinden
  yapılan guest yazımlarında translation cache'i geçersiz kılmıyordu. `StoreGetArray` kodu
  yazdıktan sonra `uc_ctl_remove_cache` ile o aralığı temizliyor; işleyici tamponu ve
  `RunChallenge` kodu da aynı şekilde temizleniyor. `BuckSmoke --seq client-40.bin
  client-660.bin 04d30000` artık sırayla 12 ve 112 bayt üretiyor.
- **Anahtar zinciri:** bir sonraki challenge, önceki challenge'ın **nonce**'u ile kodlanıyor;
  işleyicinin yeniden kaydettiği param bu. `s_challengeKey` bunu taşıyor; çözüm adayları
  arasında token ve nonce denenip başlık/kod makullüğüyle doğrulanıyor (`ChooseChallengeKey`).
- **Nerede:** `Engine/Source/Os/Mac/BuckCodeEmu.{h,cpp}` — teslim edilen x86'yı Unicorn'da,
  Wow.exe görüntüsü 0x00400000'a eşlenmiş halde çalıştıran makine; istemci fonksiyonları
  (CDataStore/WDataStore, SMem, memset/memcpy/strlen, zlib_uncompress, VirtualAlloc/Protect,
  Send2, SetMessageHandler) köprülenir. Hile yok: baytları sunucunun kodu üretir.
- **İstemci tarafı:** `WoW/Source/Net/NetClient/BuckData.{h,cpp}`; kodu iki bağlantıda da
  işler (realm'de 0x4B8'i RealmConnection yapar, 0x4B9/0x41B'yi BuckData; dünyada hepsini
  BuckData). Token `NetClient::m_buckToken` ve süreç geneli `s_lastBuckToken`'da saklanır;
  realm'in istek üreteci artık `BuckDataHandleCustomCode`'u paylaşır. Teşhis:
  `Logs/buck-data.log`.
- **Offline test:** `Tools/Wardendump/buck_smoke.cpp` + `Tools/Wardendump/captures/`.
  `BuckSmoke --handler captures/handler-41b.bin captures/challenge-official-40.bin 5660000`
  resmi kayıttaki challenge'a resmi cevabı (`cc 70 7b 9a ec 6d 08 e0`) birebir üretir;
  `challenge-660.bin` ile 112 baytlık parmak izi cevabı üretilir; `--lua` 0x4B9 betiğini
  çözer. `challenge-40-a/b` eski oturum kayıtlarıdır.
- **Hâlâ yapılmadı:** 0x41D/0x41C veri paketlerini işleyen sunucu işleyicileri kurulmuyor
  (paketler yalnız loglanıyor). 0x4B9 Lua betiği doğru tohumla çözülüp `FrameScript_Execute`
  ile çalıştırılıyor: işleyici ilk baytı `CDataStore::GetByte` ile anahtar tohumu olarak
  okuyor, kalanı onunla çözüyor (eskiden tohum 0 sanılıp ilk bayt da çözülüyordu; karakter
  seçiminde Lua sözdizimi hatası veriyordu). Uzun challenge'lar yeni istemci fonksiyonu köprüleri gerektirebilir; eksik
  köprü çağrısı `Logs/buck-data.log`'a düşer ve `BuckCodeEmu.cpp`'deki tabloya eklenir.
  Offline testler: `BuckSmoke --handler captures/handler-41b.bin
  captures/challenge-official-40.bin 5660000` (resmi cevabın birebir aynısı) ve
  `BuckSmoke --lua captures/lua-4b9.bin`.

## WARDEN — 364 baytlık dosya kontrolü ve SFile2 köprüleri (2026-10-19 gece)

Dünyaya girdikten sonra gelen 364 baytlık `SMSG 0x2E6` challenge'ı, Warmane modülünün
**harita dosyalarını okuyup hash'lemesini** istiyor. Modül dosyaları istemcinin kendi Storm
API'siyle açıyor:

```
World\Maps\IsleofConquest\IsleofConquest_35_29.adt
World\Maps\NorthrendBG\NorthrendBG_36_28.adt
World\Maps\NetherstormBG\NetherstormBG_27_30.adt
```

Bu çağrılar `WardenEmu`'da köprülenmediği için emülatör `0x00424F80` (`SFileOpenFile`) üzerinde
duruyordu; istemci 14 baytlık anlamsız bir cevap gönderiyor ve sunucu ~35 saniye sonra bağlantıyı
kesiyordu (kanıt: `Logs/warden.log` "module called client code at 00424f80 ... with no native
bridge", ardından `Logs/net-events.log` DISCONNECTED). Karakter seçim ekranı bu challenge'ı
almadığı için orada kopma olmuyordu.

Eklenen köprüler (`Engine/Source/Os/WardenEmu.h`, `Engine/Source/Os/Mac/WardenEmu.cpp`,
`WoW/Source/Game/GameClient/Mac/WardenClient.cpp`):

- `SFileOpenFile` (0x00424F80), `SFileGetFileSize` (0x004218C0), `SFileReadFile` (0x00422530),
  `SFileCloseFile` (0x00422910) — giriş adresleri Wow.exe giriş baytlarıyla doğrulanıyor.
- Modüle sahte (opaque) 32-bit handle döner; gerçek `SFileImpl*` istemcide kalır. Böylece modül
  dosyayı kendi koduna göre parça parça okuyup hash'ler; cevabı yine modül üretir.
- `WardenEmuServices::FileOpen/FileSize/FileRead/FileClose` arayüzü eklendi.
- **Kritik ayrıntı — `__stdcall`:** Storm API'si `ret 8/0x18/4` ile kendi argümanlarını temizler.
  Emülatör ilk denemede yalnız dönüş adresini pop ettiği için modülün yığını 8 bayt kayıyor,
  kaydedilmiş `edi` yerine dosya handle'ı geri okunuyor ve `[handle+0x20]` okuması fault
  veriyordu (`guest read fault at 000000000b000020`, `faulting EIP 0180267f`). `ClientFunction`
  tablosuna `argumentBytes` eklendi; köprü dönüşte `esp += 4 + argumentBytes` yapıyor.
  Doğrulama: `WardenSmoke --snapshot Logs/warden-snapshot.bin` aynı Process çağrısını hatasız
  tamamlıyor (320898 baytlık ADT, 19 okuma + close).

Offline doğrulama (`WARDEN_REPLAY_COLD=1 WardenSmoke <image> <traffic> <key>`): replay 364
baytlık paketi işliyor, üç ADT'yi Storm MPQ'larından okuyor (ör. 320752 bayt, 16K'lık parçalar)
ve **56 baytlık** gerçek cevabı üretiyor. `WardenSmoke` artık Storm'u başlatıp oyun dizinindeki
MPQ arşivlerini açıyor; `WARDEN_CLIENT_EXE` hem istemci imajını hem oyun dizinini belirliyor.

## CANLI DURUM (2026-10-19 gece) — SFile köprüsü sonrası

`WRATHMODERN_AUTOLOGIN=1` ile 19:08 oturumu: giriş, karakter listesi ve dünya girişi sorunsuz;
oturum **15 dakika** boyunca ayakta kaldı (19:08:03 → 19:23:04), Warden keepalive'ları devam etti
ve hiçbir `no native bridge` / fault oluşmadı. Bu oturumda sunucu dosya-kontrol challenge'ı
(314/364 B) hiç göndermedi; 15. dakikada bağlantıyı kapattı. Kapanıştan hemen önce 92 baytlık
buck challenge'ı 188 baytla cevaplandı; cevabın içeriğini çözünce istemci kodundan istenen üç
bellek bölgesi (`0x0086B5A0`+88, `0x004E31CE`+5, `0x0086B510`+86) **birebir** çıktı — yani 92
cevabı içerik olarak doğru.

Kalan sadakat farkı **192 baytlık modül taraması**: sunucunun kodu `CreateToolhelp32Snapshot` +
`Module32First/Next` ile yüklü modülleri geziyor; resmi istemci 1996 baytlık liste (≈33 modül,
ilk giriş `base 0x00400000 size 0x9FD000 Z:\...\wow.exe`, ikinci modül `0x7BF30000` + `0xB9000`),
bizim emülatör ise tek giriş (`wow.exe`, 56 bayt) döndürüyor (`BuckCodeEmu` `CLIENT_MODULE32_*`).
Sunucu bunu şimdilik tolere ediyor; 15. dakikadaki kapanışın nedeni bu olabilir. Sıradaki adım:
resmi istemcinin modül listesini (oyun DLL'leri + sistem DLL'leri, gerçek base/size/path) emüle
etmek. Bu yapılmadan oturumun 15 dakikadan fazla sürmesi garanti değil.

**Önemli:** Aynı hesapta iki istemci (bizim autologin + kullanıcının kendi istemcisi) aynı anda
açılırsa sunucu birini atar. Test sırasında tek istemci çalıştırılmalı.



Otomatik giriş (`WRATHMODERN_AUTOLOGIN=1`) ile doğrulandı: istemci kendisi giriş yaptı,
`Hendell` karakteriyle dünyaya girdi ve **12 dakikadan fazla** bağlantıda kaldı; Warden
keepalive'ları devam etti. Dünyada cevaplanan challenge'lar: **40, 660, 192, 88, 92 ve 84**
bayt. Eski ~1 dakikalık kopma artık yok.

- Otomatik giriş `CGlueMgr::LoginServerLogin` üzerinden yapılıyor (doğrudan
  `ClientServices::Logon` sürüm kanıtında takılıyordu); kimlik bilgileri repo dışındaki
  `~/.wrathmodern-autologin` dosyasında (account, password, realm), `chmod 600`.
  İlerleme `Logs/autologin.log`'da; istemci karakter girişinden sonra açık bırakılıyor.
  **Yeni testten önce çalışan örneği kapat:** `pkill -f WrathModern.app/Contents/MacOS/WrathModern`.
- 264 baytlık `SMSG 0x2E9` framework paketi artık BuckData tarafından tüketiliyor; stok
  battleground handler'ı onu 64-bit GUID sanıp `CDataStore::Get(uint64)` üzerinde çöküyordu.
- macOS çıkarılabilir disk izni: uygulama `WrathModern Dev` kimliğiyle kalıcı imzalanıyor
  (`Tools/sign-app.sh`, derleme sonrası otomatik; ExFAT `._*` dosyaları imzadan önce siliniyor).
  Bir kez izin verildikten sonra yeniden derlemelerde sorulmuyor.
- **Kalan sadakat farkı:** 88 ve 92 baytlık challenge'lar, sunucunun verdiği istemci
  adreslerinden bellek okuyup döndürüyor; bizim dönüş uzunluğu/içeriği resmi istemciden farklı
  (ör. 88 → bizde 212 bayt, resmide 20). Sunucu şu an bunu tolere ediyor; ileride bir kontrol
  reddederse ilk bakılacak yer burası. 84 baytlık tablo `0x00DD0FD8` hâlâ sıfır, çünkü 0x41C
  veri işleyicisi kurulmuyor. `BuckCodeEmu::RunDataHandler` / `BuckSmoke --data` bu işleyiciyi
  çalıştırmak için hazır.



Oyundan ~1 dk sonra atılmanın nedeni: Warmane, `battle.net.dll` (oyun klasöründe 15,5 MB, korumalı)
aracılığıyla **kod akıtan bir challenge-response** çalıştırıyor. Kanıtlar (resmi istemci, hook DLL):

- `SMSG 0x2E9` (264 B) gelince framework `SetMessageHandler(0x4B8, 0x03CD0020, 0x03CD0020)` kuruyor.
- Her `SMSG 0x4B8` için framework, blob'un [len][kod] parçalarını çalışma zamanı tamponlarına
  kopyalıyor (COPY this=0x02AEFE58 dst=0x03CD005E len=240 …), install parçasını çalıştırıp
  istek paketini gönderiyor, sonra handler parçasını 0x03D00000/… adreslerine kopyalayıp
  `SetMessageHandler` ile bağlıyor:
  - 0x41D → 0x03D00000 (1789 B; CVar/SFile/SMem/veri toplar), 0x41C → 0x03D10000 (133 B),
    0x4B9 → 0x03D20000 (350 B; Lua), 0x41B → 0x05660000 (316 B).
- `SMSG 0x41B` challenge'ları **sunucudan gelen x86 kodu** taşıyor: handler, mesajı
  `param` anahtarıyla XOR/ror açıp kodu `handler+0x154`'e kopyalıyor ve çağırıyor; dönen yanıt
  `CMSG 0x41B` olarak gidiyor (gözlenen çiftler: 40→12, 660→112, 192→1996, 84→48, 88→20, 80→20).
- Anahtar (param) her mesajda framework tarafından rastgele yenileniyor (ör. 0x4AC5131D).
- `SMSG 0x41D` içeriği: `[uint32 97165][zlib]` — Warmane'in özel içerik veritabanı
  (Battle Royale, Azshara Crater, özel spell/item metinleri). Native tarafta zlib ile açılabiliyor.
- Handler'ların kullandığı istemci fonksiyonları: CDataStore (0x47B0A0 Put, 0x47B560 GetArray,
  0x47B6B0 GetDataInSitu, 0x47B3C0 data), WDataStore (0x403EC0/0x403F10), ClientServices::Send2
  (0x6B0B50), SetMessageHandler (0x6B0B80), SMem (0x76E540/0x76E5E0), CVar (0x76677B0/767100/
  767460), SFile2 (0x422140), Lua (0x84DBF0/0x84EC50/0x84F860), SComp (0x778180).

Sonuç: tam uyum için sunucunun gönderdiği x86 kodunu istemci imajı eşliğinde çalıştıracak bir
sandbox + bu istemci fonksiyonlarının köprüleri gerekir (Unicorn altyapısı WardenEmu'da mevcut).
Mevcut durumda karakter listesi ve dünyaya giriş çalışıyor; dünya içi periyodik buck exchange
yanıtlanamadığı için oturum ~1 dk sonra düşüyor.

## YAPILDI (2026-10-17)


### Köprü (Engine/Source/Os/WardenEmu.h + Mac/WardenEmu.cpp)
- Unicorn 32-bit x86 makinesi; BLL görüntü yükleyici birebir (40-bayt başlık, run-length, relocation +base, import çözer).
- Guest bellek düzeni: TEB 0x000FE000, GDT 0x000FF000, stack 0x00100000+512K, misc 0x00200000, scratch 0x00210000+64K, heap 0x00300000+4MB(+ext 0x00F00000), image 0x00700000+8MB.
- OurLibrary sahte nesnesi + vtable'ı guest'te; her slot için `ret n` stub'ı + UC_HOOK_CODE → host handler (__thiscall ABI: ECX=this, stub kendi arg temizliğini ret n ile yapar).
- Modül çağrıları: `CallGuest(fn, args, count, ecx)` — her çağrı taze stack, STOP sentinel dönüş adresi, EAX okunur.
- Importlar: builtin'de yok; bilinenler (kernel32!Sleep, GetTickCount) stub'lanır; BİLİNMEYEN İTHALAT → yükleme reddedilir + log (dürüst yol).
- Sınır konuları: MAX_TRANSFER_BYTES=32K (Data/ModuleCache/StateSave), 50M adım sayacı, invalid-mem hook'u loglar + durdurur.

### Mac istemci (WoW/Source/Game/GameClient/Mac/WardenClient.cpp)
- Win32 dosyasının birebir port'u: LoadModule (RC4+isim kodları+SSignature doğrulama+zlib) → WardenEmu::Load.
- ActivateNextModule / Process / ReceiveWardenData / Destroy akışları Win32 ile aynı (UnloadActiveModule Shutdown çağırır dahil).
- SMSG_WARDEN_DATA handler kurulur; CMSG_WARDEN_DATA WDataStore üzerinden Send2 ile gider.

### Ortak veri/simge düzeltmeleri
- `WardenModuleData.h`: yerleşik modül blobu/anahtar/modulus/isim kodları (Win32 ile paylaşımlı; Win32 dosyası da bu başlığı kullanıyor).
- `WardenCachedModule.cpp` (yeni paylaşımlı dosya): WardenCachedModule tüm yöntemleri + CWardenKey operator<</>> — Win32 ve Win64 stub kopyaları silindi (aynı kod 3 yerde → tek yer).
- Win64 stub `WardenClient.cpp` artık sadece stub akışı; CWardenKey operatörleri paylaşımlı dosyada.

### CMake
- `WRATHMODERN_WARDEN_EMULATION` opsiyonu (mac'te varsayılan ON). Unicorn bulunamazsa OFF'e düşer (stub path'e döner).
- Platform.cmake: emulation açıksa mac derlemesine `/GameClient/Win64/WardenClient.cpp` paylaşılan listesinden çıkarılır (list(FILTER ... EXCLUDE REGEX "WardenClient") — liste girdileri regex, '$' ucu eşleşmez, dikkat!).
- Os hedefi unicorn'a bağlanır (find_library /opt/homebrew).

### Duman testi (Tools/Wardendump/warden_smoke.cpp)
Derleme: `c++ -O2 -std=c++17 -arch arm64 -I. -I/opt/homebrew/include -DWOWDECOMP_PLATFORM_MAC=1 -DWRATHMODERN_WARDEN_EMULATION=1 Tools/Wardendump/warden_smoke.cpp Engine/Source/Os/Mac/WardenEmu.cpp -L/opt/homebrew/lib -lunicorn -lz -o Tools/Wardendump/warden_smoke`
Sonuç: .load ok → .create ok → .initialize ok (result 1) → .processed → .done → .shutdown ok; StateLoad(520)/StateSave(520) modülün kendi state akışı köprüden düzgün geçiyor. Bu, BMC GÖNDERDRAW Handshake yok duman testinde bekleniris (sunucu verisi olmadan modül el sıkışma üretmez); gerçek el sıkışma testi Warden etkin bir sunucuya karşı yapılır.

## KALAN — SIRADAKİ ADIMLAR
1. Canlı Warden oturumu → `Logs/` altında `warden-module-image.bin` (şifresiz görüntü), `warden-session-key.bin`, `warden-traffic.log`, `warden-module-key.bin` oluşur → **tam offline replay**.
2. `WardenSmoke <image> <traffic> <session-key>` ile hatayı offline çoğalt; `Logs/warden.log`'da:
   - `LCMapStringA called from XXXX` → çağıran komutu disassemble et (RVA = XXXX-0x700000).
   - `... emu error ... (faulting EIP XXXX)` ve `guest read/write fault at ...` → gerçek fault adresi.
3. Fault'ı gider (muhtemel: modülün beklediği bir host yapısı eksik ya da bir import yanlış değer döndürüyor).
4. `LCMapStringA` bayraklarını gerçek anlamıyla ele al (bkz. alttaki analiz).
5. Donma ("authentication successful" anında): modülün `Process`'i ana iş parçacığında senkron çalışıyor; adım bütçesi/log sonrası bak.
6. Warmane ayrı: "oyun sürümü farklı" → `DivxDecoder.dll.bak` orijinali geri koy + Wow.exe orijinal mi doğrula.


## RİSK / HATIRLATMA
- Unicorn hook'ları uc_emu_start içinde çalışır; içeride uc_mem_map çağrısı GuestAlloc uzatması HARİÇ YAPILMAMALI (heap uzatması hook içinde uc_mem_map yapiyorsa sorun çıkarabilir — şimdilik ext heap mapesi yalnız NewImportStub ve heap taşmasında; test edilirse!).
- `list(FILTER EXCLUDE REGEX "WardenClient")` genelliği: başka "WardenClient" girdisi eklenirse yanlışcı çıkarır — dikkat.
- Kullanıcı Türkçe konuşur; hile özelliği ve sahte-check cevabı KESİNLİKLE yok.

## BİLGİ (sunucu tarafında beklenti)
- Önceki test sunucusunda SMSG_WARDEN_DATA 0x02E6 AUTH_RESPONSE'dan ÖNCE geliyordu; CMSG_CHAR_ENUM 0x0037 Warden cevabına kadar tutuluyor olabilirdi.
- 3.3.5 Warden protokolü tamamen modül içinde; istemci sadece bayt önden-arkaya taşır (Data/ModuleOutput) — emülasyon = gerçek davranış.

## VERSIYON KANITI (Warmane kapısı) — 2026-10-17

Bulgu: mac istemci versiyon kanıtını (login proof, ChecksumExecutables HMAC-SHA1) KENDİ binary'sinin
üstünden üretiyordu → hiçbir sunucunun bilmediği kanıt → Warmane 'oyun sürümü farklı' (AUTH_VERSION_MISMATCH=20).
Orijinal mac istemcisi de kendi binary'sini kanıtlardı; ama Warmane Windows kanıtını bekliyor.

Çözüm (Engine/Source/Os/Mac/OsVersionHash.cpp yeniden yazıldı):
- Oyun klasöründeki Wow.exe ve DivxDecoder.dll, loader eşlemesi taklit edilerek (header+RVAs+zero padding,
  reloc'lar preferred-base değerlerinde = uygulanmamış) Windows istemcinin ComputeChecksum algoritmasıyla
  hash'leniyor: e_csum maskeli (type 3), import thunk'ları OriginalFirstThunk'tan (INT; loader dokunmaz),
  import descriptor'ları TimeDateStamp/ForwarderChain sıfırlı, writable section'lar ve alignment padding
  SIFIR olarak hash'lenir. qsort offset sıralaması dahil birebir.
- delta her zaman 0 (dosyadan mapped) → Windows'un 'unrelocate' sonucu = dosya değerleri, çıktı eşdeğer.
- Padding'e düşen girdiler de hash'e katılır (Windows davranışı; ayrıca Windows'un taşma/çift-hash
  kenar davranışları aynen korunur).
- unicows.dll aynı: ham dosya baytları. Wow.exe yoksa: kendi binary (orijinal mac davranışı fallback).
- Doğrulama: Tools/Wardendump/WardenVersionProbe (CMake hedefi `cmake --build build/mac --target WardenVersionProbe`).
  - `/tmp/gamedir/x.app/Contents/MacOS/x <gamedir>` ile test: find Wow.exe ✓, mapped digests ✓, proof ✓.
  - ClientDirectory = .app'in ÜST dizini; probe binary'si .app dışında ise find başarısız olur (normal).

DİKKAT: kanıt oyun klasöründeki DOSYALARDAN üretir. DivxDecoder.dll değiştirildiyse (mod loader — her iki
klasörde de .bak var!) kanıt orijinal olmaz; Warmane reddedebilir. Çözüm: orijinal .bak dosyasını geri koy.


## MODÜL IMPORT KATMANI (tarihsel el sıkışma blokajı) — 2026-10-17 (devam)

Bulgu (önceki test sunucusundaki gerçek oturumun warden.log'u): yerleşik modül sunucu modülünü
parça parça (503B xN) alıp ModuleCache ile önbelleğe alıyor, sonra LoadModule
çağrılıyor ama modül **import çözümlemesinde reddediliyordu** (LCMapStringA yok).
Yeni "collect-all" davranışı tek oturumda TAM listeyi verdi: modül 33 fonksiyon
import ediyor (retail 3.3.5a Warden modülü + MSVC CRT):
  KERNEL32: Sleep, GetTickCount, LCMapStringA, lstr*, CharUpperBuffA, MulDiv,
    VirtualAlloc/Free/Protect/Query, Read/WriteProcessMemory, GetProcessHeap,
    HeapFree, GetCurrentProcess(Id)/ThreadId, TlsAlloc/Free/GetValue/SetValue,
    GetSystemInfo, GetVersionExA, QueryPerformanceCounter, GetSystemTimeAsFileTime,
    GetModuleHandleA, LoadLibraryA, FreeLibrary, GetProcAddress, QueryDosDeviceA,
    CloseHandle, DuplicateHandle, RaiseException, RtlUnwind,
    UnhandledExceptionFilter, SetUnhandledExceptionFilter, TerminateProcess
  USER32: CharUpperBuffA, DestroyIcon, DrawTextA

Yapılan (Engine/Source/Os/Mac/WardenEmu.cpp):
- Load: importlar DLL'den BAĞIMSIZ isimle çözülür (kernel32+user32), case-insensitive;
  bilinmeyenler toplanır ve TEK seferde "module load refused: N unknown import(s)".
- Ayrı UC_HOOK_CODE aralığı (HEAP_EXT_BASE..+0x1000) → import stub çağrıları hook'a
  düşer; stub zaten code-hook'ta yoktu (önceki hata: hiç çağrılmıyordu).
- Gerçek implementasyonlar: TLS tablosu, heap (GuestAlloc), GetProcAddress→StubForName
  (aynı stub'ları çözer), LoadLibraryA/GetModuleHandleA (sözde handle'lar),
  GetSystemInfo/GetVersionExA (Win7 6.1.7601 — emüle ortamın sunduğu sürüm),
  VirtualQuery (bölge tablosu), QueryPerformanceCounter/GetSystemTimeAsFileTime (gerçek saat),
  MulDiv/CharUpperBuffA (birebir), process/handle kimliği.
- RaiseException/RtlUnwind/TerminateProcess: misafir SEH unwind YOK → m_faulted=true +
  uc_emu_stop; çağrı sahte başarıyla dönmez, dürüstçe başarısız olur.
- MacWardenServices::ModuleCache: her sunucu modülünü Logs/warden-module-<id>.bin'e YAZAR
  (offline import/kod analizi için). Sonraki oturumda bu dosya incelenecek.

Dönemin durumu: iki macOS istemci dalı derlendi, iki app güncellendi. Smoke yeşil. Sıradaki test:
Warden etkin sunucuda giriş → modül artık YÜKLENMELİ; log'da "import X was called but has no behavior"
veya "RaiseException/RtlUnwind" satırları çıkarsa o çağrılar modülün gerçekten kullandığı
yollardır → sırayla gerçek davranış eklenir.

---

## NEREDE TAKILDIK — Warden el sıkışması (2026-10-17 akşam, tarihsel)

### Ne ÇALIŞIYOR (kanıtlanmış)
Önceki test sunucusundaki gerçek oturumun `Logs/warden.log`'u (2. oturum, 11:22):
```
[1] module loaded: imageSize=24576 ...          <- yerleşik modül
[3] Initialize done (client key length 40, result 00000001)
SMSG_WARDEN_DATA 37 bytes, active=1             <- MODULE_USE
[37] module loaded: imageSize=45056 exports=1 imports=2 export=007012f1   <- SUNUCU MODÜLÜ YÜKLENDİ
[38..42] GetProcAddress(AddVectoredExceptionHandler / RemoveVectoredExceptionHandler /
         CreateToolhelp32Snapshot / wine_get_unix_file_name) + LoadLibraryA(psapi.dll)
         -> hepsi dürüstçe "yok" (modül bunları yokluyor; bizde Wine da yok)
[43] module created, object 00300000 vtable 00708168
[44] Initialize done (client key length 40, result 00000001)
module consumed 37 bytes / 17 / 57 / 39 / 288 bytes    <- PAKETLERİ TÜKETİYOR
CMSG: 1, 21, 13 bayt gönderiliyor (warden-traffic.log: SMSG/CMSG dizesi)
```
Yani: import katmanı tamam, modül yükleniyor/çalışıyor, sunucuyla bayt alışverişi var.
Karakter listesi **yine de gelmiyor** (`COP_GET_CHARACTERS` → 30 sn sonra RESPONSE_CANCELLED).

### İki hata, modülün gerçek `Process`'ini kırıyor
```
[45] LCMapStringA args: locale=0030064c flags=00300000 src=00000000 cchSrc=0 dest=00000000 cchDest=0
[46] WardenModule::Process at 007063b8: emu error Invalid memory read (UC_ERR_READ_UNMAPPED)
[47] LCMapStringA: bad source length 3147340        (= 0x30028C)
[48] LCMapStringA args: locale=0017fdc0 flags=00000000 src=00000000 cchSrc=3147340 dest=00300000 cchDest=1572591
[49] ...
[51] LCMapStringA args: locale=0030064c flags=00300000 src=0017feef cchSrc=0 dest=00000000 cchDest=0
[52..55] WardenModule::Process ... READ_UNMAPPED (tekrar tekrar)
```
Notlar:
- `[45]` ve `[51]` AYNI argümanlarla geliyor → deterministik bir çağrı, çöp değil.
- `locale=0x0030064C`, `flags=0x00300000` = modül nesnesinin HEAP adresi (nesne 0x00300000'de,
  `0x30064C = nesne + 0x64C`). Yani elimizde okuduğumuz "argümanlar" aslında modülün kendi
  nesne alanlarına işaret eden **pointer'lar** — API'ye giden gerçek değerler değil.
- `0x00300000 = LCMAP_HIRAGANA(0x100000) | LCMAP_KATAKANA(0x200000)`. Bu kombinasyon Windows'ta
  anlamsız/geçersiz; yani bayrağın kendisi zaten şüpheli → çağrı yeri yanlış çözülüyor olabilir.

### Statik analiz (capstone + import tablosu; `Tools/Wardendump/dump/image_raw.bin` = 45056 B görüntü)
- Import thunk'ları: KERNEL32 thunkRVA=0x8000 (LCMapStringA ilk sırada), USER32 thunkRVA=0x807C; 33 giriş.
- Modül nesnesi = `0x00300000`; `Process` fonksiyonu RVA **0x63B8** (`vtable+12`); `esi`=nesne.
  0x63C3: `cmp dword ptr [esi+0x42c], 0` → iş kuyruğu; 0x63D0: `lea edi,[esi+0x64c]`.
- Sadece ÜÇ import `jmp dword ptr [slot]` **thunk**'ına sahip: 0x7AEC=LCMapStringA, 0x7AF2=MulDiv,
  0x7AF8=DrawTextA. (Diğerleri doğrudan `call [slot]`.) Modül LCMapStringA'yı bu thunk üzerinden
  çağırıyor gibi; ama 0x7AEC'ye giden bir `E8` call bulunamadı → çağrı bir **fonksiyon pointer
  tablosu/indirection** üzerinden olabilir. Bu yüzden log'a **çağıran adresi** ekledik.

### Bu round yapılanlar (kod)
1. **Gerçek Windows process ortamı** (asıl muhtemel kök neden): `Boot()` artık TEB'i dolduruyor
   (SEH zinciri sonu, StackBase/Limit, `fs:[0x18]` self, `fs:[0x2C]` static-TLS dizi, `fs:[0x30]` PEB)
   ve bir **PEB** yaratıyor (ImageBaseAddress, Ldr + tek girdili loader listesi, ProcessHeap,
   processor/heap sayıları). Öncesinde TEB haritalı ama bomboştu, PEB hiç yoktu → modülün MSVC
   runtime'ı `fs:[0x30]`/`fs:[0x2C]` okurken 0'a erişip fault veriyordu. Sayfalar: PEB 0x000FD000,
   TLS dizisi 0x000F8000, ENV (Ldr/listeler/isim) 0x000F7000. VirtualQuery bölge tablosuna eklendi.
2. **Tanı logging'i:** `CallGuest` artık hatada **faulting EIP**'i basıyor; `UC_HOOK_MEM_INVALID`
   aralığı (0,0)→(1,0) düzeltildi (öncesi sadece adres 0'ı kapsıyordu, gerçek fault hook'a düşmüyordu);
   `HandleImport` **`LCMapStringA called from XXXX`** (çağıran adres) basıyor.
3. **Offline replay için veri dump'ları:**
   - `LoadModule` → `Logs/warden-module-image.bin` (şifresiz, inflate edilmiş 45056 B görüntü).
   - `ActivateNextModule` → `Logs/warden-session-key.bin` (40 B oturum anahtarı).
   - `ModuleUse` → `Logs/warden-module-key.bin` (16 B, modül RC4 anahtarı) + id/key log satırı.
   - (Önceden) `ModuleCache` → `Logs/warden-module-<id>.bin` (şifreli BLL blobu); `WardenTrace` → tam trafik.
4. **WardenSmoke 3. argüman:** `WardenSmoke <module.bin> <trace.log> <session-key.bin>`.
   Ayrıca `WARDEN_IMAGE_DUMP=<path>` env değişkeni genişletilmiş görüntüyü disassembly için yazar.
5. Smoke yeşil kaldı (builtin modül lifecycle bozulmadı).

### Tam olarak TAKILDIĞIM yer
- **Offline replay şu an modülü tüketmiyor (`consumed=0`)**: modül her sunucu paketini oturum
  anahtarıyla RC4 çözüyor; smoke'ta sahte anahtar kullandığım için `Data()` paketi reddediyor ve
  modül hiç iş yapmıyor → fault (Process) ve yanlış `LCMapStringA` çağrısı **offline tekrarlanmıyor**.
- Yani: hatayı düzeltmek için **gerçek oturum anahtarı** gerekiyor. Artık dump ediliyor;
  bir sonraki login'den sonra `WardenSmoke <image> <traffic> <session-key>` ile tam offline iterasyon
  mümkün — kullanıcıdan başka login gerekmez.
- İkinci takıldığım nokta: `LCMapStringA`'nın gerçek argümanlarını okuduğumuzdan emin olamıyorum
  (yukarıdaki pointer'lar API değerlerine benzemiyor). Çağıran adresi + fault EIP geldiğinde
  çağrı yeri disassemble edilip kesinleştirilecek.

### Sıradaki somut adım
1. Kullanıcı **bir kez** login → yeni log + `warden-session-key.bin` + `warden-module-image.bin`.
2. `WardenSmoke <image> <traffic> <session-key>` ile offline çoğalt.
3. Log'daki `LCMapStringA called from` / `faulting EIP` / `guest ... fault at` satırlarına göre:
   - Fault adresi modülün hangi yapıyı okuduğunu söyler → o yapıyı (veya import değerini) düzelt.
   - `LCMapStringA` bayrakları: gerçek `LCMAP_*` bitleri çıkarılıp birebir ele alınır
     (LOWERCASE/UPPERCASE zaten var; gerekirse HALFWIDTH/FULLWIDTH/KATAKANA tabloları eklenir).
4. Sonra donma ("authentication successful" anı) ve karakter listesi.

### Hile YOK / dürüstlük ilkesi (yine)
- `Process` fault verdiğinde modül **başarısız** sayılıyor; sahte "check geçti" üretilmiyor.
- `GetProcAddress(AddVectoredExceptionHandler/CreateToolhelp32Snapshot/wine_get_unix_file_name)`
  bizde gerçekten yok → 0 dönüyoruz (Win32 istemci de VEH/Toolhelp kurmaz; Wine tespiti doğru: değiliz).


---

## GÜNCELLEME (2026-10-17 öğle) — yem importlar, dal izi, snapshot

### Bulgu: `LCMapStringA` çağrısı gerçek bir API çağrısı değil
- `image_raw.bin` üzerinde ham bayt taraması: `0x7AEC` (LCMapStringA), `0x7AF2` (MulDiv) ve
  `0x7AF8` (DrawTextA) thunk'larına giden **hiçbir** `E8/E9/Jcc` yok. Relocation tablosunda da
  bu thunk'ları gösteren bir pointer yok. Yani bu üç import modülde **ölü** (yem).
- Sonuç: log'daki `LCMapStringA` girişleri, bozuk bir fonksiyon pointer'ı ya da bozuk bir dönüş
  adresi üzerinden stub'a (0x00F00000) **vahşi sıçrama**. "Argümanlar" yalnızca o anki yığın içeriği:
  `[esp]` = nesne (0x300000), `[esp+4]` = nesne+0x64C. Bu, `Process`'in 0x7063DD–0x7063E2'de
  `call 0x702CFD` öncesi yaptığı push'larla birebir uyuşuyor.
- `obj+0x7B0` = modülün coroutine devam pointer'ı (ör. 0x702E73: `mov eax, 0x11E4` → `[ebx+0x7B0]`),
  `0x702D77: call eax` ile çağrılıyor. Relocation'lar doğru (0x2E74 reloc listesinde).
  Şüpheli: o devam zincirindeki bir çağrı ya da `[ecx]` vtable'ı yanlış değer okuyor.

### Eklenen tanı (commit 08a8eda)
- **Dal izi:** `UC_HOOK_BLOCK` halkası (son 256 blok, adres + ESP). Başarısız giriş çağrısında ve
  dönüş adresi image dışında olan her import girişinde `trace:` satırları yazılır: registerlar,
  yığının ilk 12 kelimesi, son 64 blok (RVA ile).
- **Snapshot:** Her giriş çağrısından önce guest durumu alınır. Bir emülatörün ilk hatasında durum
  `Logs/warden-snapshot.bin`'e yazılır. `WardenSmoke --snapshot <dosya>` aynı çağrıyı offline birebir
  tekrarlar; yamalı test görüntüsüyle doğrulandı (aynı EIP ve aynı registerlar).
- **Doğru oturum replay'i:** `WardenSmoke <image> <traffic> <session-key>` artık istemci gibi çalışıyor:
  önce yerleşik modül, `ModuleUse` sonrası sunucu modülü, `StateSave`/`StateLoad` ile RC4 durumu
  aktarılır ve çıktılar trace'teki CMSG'lerle karşılaştırılır. Eski replay bu aktarımı yapmadığı için
  doğru anahtarla da çalışamazdı. İstemci her girişte trafik loguna `SESSION` satırı yazar; replay
  son oturumu oynatır.

### Sıradaki adım
1. Yeni veri gerektiğinde Warden etkin sunucuda bir canlı oturum yakalanır (karakter listesine kadar).
2. `Logs/warden.log` → `trace:` satırları vahşi sıçramanın yerini doğrudan gösterir.
3. `Logs/warden-snapshot.bin` + `warden-session-key.bin` ile tüm iterasyon offline yürür; yeni giriş gerekmez.

---

## ÇÖZÜLDÜ: vahşi sıçrama ve donma (2026-10-17, commit 4981633)

**Kök neden (canlı oturumun dal izinden):** Sunucu, modüle "ana image + 0x46AE20'yi çöz, zaman
kaynağı olarak çağır" isteği gönderiyor (`rva 0x2841` → `obj+0x7EC`, çağrı `rva 0x31AC: call ebx`).
Windows'ta `GetModuleHandle(NULL)` = Wow.exe @ 0x00400000 → `0x0086AE20` = `OsGetAsyncTimeMs`
(hız hilesi kontrolü: GetTickCount ile karşılaştırılıyor). Emülatör NULL için modülün kendi tabanını
veriyordu → çağrı image'ın boş kuyruğuna düşüp megabaytlarca sıfırın üstünden ilk import stub'ına
(LCMapStringA) kayıyordu. Her `Process`'te ~4M komutluk bu kayma = "authentication successful" donması.

**Çözüm:**
- Guest düzeni: 0x00400000–HEAP_BASE istemci exe'sine ait. Heap 0x01000000, modül 0x01800000,
  stub/ext heap 0x02000000, sahte handle'lar 0x03000000+ (eşlenmemiş).
- `WardenEmuServices::ClientImage`: oyun klasöründeki Wow.exe, Windows loader düzeninde
  (`OsMacMapPeImage`, sürüm kanıtıyla aynı `MappedImage`) 0x00400000'a eşlenir. PEB/Ldr/`GetModuleHandle(NULL|"Wow.exe")` onu gösterir.
- İstemci fonksiyon köprüleri (`s_clientFunctions`, prolog baytları doğrulanınca açılır):
  `OsGetAsyncTimeMs` (0x86AE20) → bizim `OsGetAsyncTimeMs`; `OsLoadLibrary` (0x86C4E0) → emüle LoadLibraryA.
  Köprüsüz bir istemci adresi çalıştırılırsa çağrı durur ve loglanır; Windows'un x86 kodu ÇALIŞTIRILMAZ.
- Modül image'ının kullanılmayan kuyruğu çalıştırılamaz → vahşi sıçrama anında fault.
- Doğrulama: yakalanan oturum offline 0,36 sn'de hatasız oynuyor; ilk iki cevap canlıyla birebir,
  saat köprüsü iki kez çağrılıp doğru adrese dönüyor; yerleşik/sunucu smoke'ları yeşil.

**Replay komutu (artık exe gerekli):**
`WARDEN_CLIENT_EXE="<oyun>/Wow.exe" Tools/Wardendump/WardenSmoke <image> <traffic> <session-key>`
(Eski manuel derleme satırı geçersiz; `ninja -C build/mac WardenSmoke` kullan. Eski snapshot'lar eski
bellek düzenine ait, yeni sürümle oynatılamaz.)

## Bellek haritası (2026-10-17, commit f3e0fe1)
Canlı oturumda modül `VirtualQuery` ile tüm adres alanını tarıyor ve `MEM_FREE` olup `PAGE_NOACCESS`
olmayan bölgeleri gizli kod adayı sayıp okuyor (rva 0x3950 döngüsü). Emülatör boşlukları 4 KB/koruma 0,
her şeyi özel RWX olarak bildiriyordu → 0x10000/0x201000 okumalarında fault. Artık `m_memoryMap`
Windows görünümünü tutuyor (boşluk MEM_FREE/NOACCESS ve gerçek boyut; Wow.exe bölüm bölüm MEM_IMAGE;
modül BLLLoader'ın bıraktığı gibi; stack/heap özel RW; import stub sayfası sistem kütüphanesi kodu olarak
MEM_IMAGE RX). VirtualAlloc/Protect/Free bu haritayı günceller. Replay 3000 ek kareyle hatasız; tarama
kullanıcı alanını uçtan uca dolaşıyor. Not: MISC (OurLibrary stub'ları, STOP) gerçek süreçte istemcinin
kendi verisine/koduna denk geldiği için özel RW bildiriliyor.

---

## 2026-10-20 — WardenSmoke "consumed 0" kök nedeni ve tek-check iterasyonu

### Kök neden: replay'in modül önbelleği canlıdan farklıydı (RC4/framing değil)
Canlı istemci her girişte **yerleşik (builtin) modülle** başlar ve sunucu modülü o oturumda
indirilene kadar `g_wardenCache` boştur: ilk `ModuleUse` **cache miss** döner, yerleşik modül
1 baytlık "module missing" cevabını verir, ardından sunucu modülü **503 baytlık transfer
paketleriyle** parça parça gelir ve ancak `ModuleCache` çağrısından sonraki `ModuleUse` başarılı
olur. `WardenSmoke`, `<module.bin>` verildiğinde ilk `ModuleUse`'u "cache hit" sayıyordu; bu yüzden
yerleşik modül "module ok" (canlı `bf` yerine `be`, 2026-10-20 oturumunda `fa` yerine `fb`)
diyor ve **sunucu modülü hemen aktive ediliyordu**. Sunucu modülünün RC4 durumu yerleşik modülün
503'lük paketlerini çözemez → her paket `consumed 0`.

Kanıt (oturum 71, aynı anahtar):
- `--cache-hit` (eski davranış): `MODULE OUTPUT #0 ... fb [DIFFERS]`, live `fa`; ardından
  `packet[1] 503 bytes, consumed 0`.
- Yeni varsayılan (boş önbellek): `MODULE OUTPUT #0 ... fa [matches]`; `packet[1..36] 503 bytes,
  consumed 503`; 287 baytlık pakette `MODULE CACHE ... 18284` → `MODULE USE ... cached module
  (28780 byte image)`; 17 baytlık paket → 21 bayt canlı cevap.

### Değişiklikler
- `Tools/Wardendump/warden_smoke.cpp`:
  - `ModuleUse` artık canlı `g_wardenCache` gibi: yalnızca **bu oturumdaki** `ModuleCache`
    girdisiyle eşleşen id başarılı; `<module.bin>` sadece `--cache-hit` /
    `WARDEN_REPLAY_CACHE_HIT=1` ile önceden yüklenmiş sayılır (önceki girişten cache).
  - `SESSION` başına oturum listesi: `--list`, `--list-packets`, `--session <n>` (1 tabanlı,
    negatif sondan). Varsayılan: paket içeren son oturum.
  - `--until <n>`: yalnızca 0..n sunucu paketini besle ve dur (tek check üzerinde çalışmak için).
  - `--dump-plain <dir>`: sunucu modülünün gördüğü **çözülmüş** paketi `packet-NNN.plain`
    olarak yazar (modülün inbound RC4 durumunun kopyasıyla).
  - `--feed <hex|@file>`: verilen **düz metin** Warden check payload'ını, modülün o anki
    inbound RC4 durumuyla şifreleyip `Data()`'ya verir ve üretilen CMSG'yi basar. Böylece
    check içeriği düzenlenip aynı RC4 konumunda yeniden oynatılabilir.
  - `--account <name>`: `.zdata` hesap adı (canlı `LORADONAA`); yanlış ad 18 baytlık hesap
    cevabını 15 bayta düşürüyordu.
- `Engine/Source/Os/WardenEmu.h/.cpp`: yalnızca tanı için `CopyInboundRc4(state[0x102])`
  (sunucu modüllerinin `object+0x122` inbound stream'i).

### Doğrulama (canlı oturum 71, PID 27892, anahtar `Logs/warden-session-key.bin`)
```
WARDEN_CLIENT_EXE="/Volumes/Yaman SSD/wow3.3.5a/Wow.exe" \
  Tools/Wardendump/WardenSmoke --account LORADONAA - \
  "/Volumes/Yaman SSD/wow3.3.5a/Logs/warden-traffic.log" \
  "/Volumes/Yaman SSD/wow3.3.5a/Logs/warden-session-key.bin"
```
(Repo kökünden çalıştırın: emülatörün tanı logu `./warden.log`'a gider; oyun dizininden
çalıştırılırsa canlı `Logs/warden.log`'a eklenir.)
Sonuç: **32/32 CMSG birebir** (02:52; 69 sunucu paketi). 02:45'teki 15/15 koşusu da birebir;
ilk `fa`, 18284 baytlık blob → 28780 baytlık image, 21 baytlık `26aedd...`, 18 baytlık
`eea93028...4c4f5241444f4e4141` (son 9 bayt `LORADONAA`) dahil.

Aynı gün 03:04'te canlı oturum 73 (54 sunucu paketi, 17 cevap) **17/17 birebir** oynadı.
Sunucu bu oturumda 341/364/400/384/310 baytlık **dosya check'leri** gönderdi; replay'in
ürettiği 57/77/117/117/66/63 baytlık cevaplar canlı CMSG'lerle aynı (ADT SHA1'leri dahil).
`warden.log`'daki "payload bytes=62" satırları kısaltmadır: trace'te aynı cevap 63, 70, 77…
bayt; örneğin 310'luk check'in canlı logu 62 bayt gösterirken trace ve replay 63 bayt
(`...4f975c`). Oturum 73 ayrıca ikinci bir modül indirmesi (`cb9e43d6...`, 18575 → 31718)
içeriyor; replay onu da canlı gibi işledi.

Tek check feed testi (aynı oturum):
```
... --session 71 --until 38 --dump-plain /tmp/plain - ...
# PLAIN #38: 058411f31dd8ed0efee10e3621e9e33105
... --session 71 --until 37 --feed @/tmp/plain/packet-038.plain - ...
# MODULE OUTPUT: 26aedd8b26999b77e58228073fdd62375a370a0265  (canlıyla aynı)
```
Plaintext'te 1 bayt değiştirilince cevap da değişiyor (`26f4eeba...`), yani feed gerçekten
modülü sürüyor.

### 314 baytlık check (oturum 70) — blokaj: oturum anahtarı
- Oturum 70: 63 sunucu paketi / 24 cevap. Hedef **packet[62] = 314 bayt**, beklenen cevap
  **output[23] = 70 bayt**: `234e00b1...d510eb1def322190`.
- `warden.log` satırı "payload bytes=62" yalnızca **kısaltma** (`WardenLogHex` 62'de kesiyor);
  tam payload 70 bayt, `realm-packets.log` CMSG `size=74` = 4 opcode + 70.
- Oturum 70'in 40 baytlık anahtarı yok: `Logs/warden-session-key.bin` 02:39:20'de oturum 71
  tarafından üzerine yazıldı; oturum 70 482267129'da DISCONNECTED, süreci çıktı. Aynı dosyayla
  oturum 70 oynatılınca `packet[0] 37 bytes, consumed 0` (yerleşik modül ilk paketi bile çözemiyor).
  Trace + `warden-module-473d97b0.bin` + `ModuleUse key=ef7f...` yeterli, eksik olan tek şey
  o oturumun 40 baytlık session key'i.
- Gereken dump: 314'lük check'i alan oturumun `warden-session-key.bin` kopyası (bir sonraki
  girişten **önce**). Anahtar hazır olunca:
```
WARDEN_CLIENT_EXE="/Volumes/Yaman SSD/wow3.3.5a/Wow.exe" \
  Tools/Wardendump/WardenSmoke --session 70 --until 62 --trailing 1200 --dump-plain /tmp/p70 \
  - "/Volumes/Yaman SSD/wow3.3.5a/Logs/warden-traffic.log" /tmp/warden-session-key-session70.bin
# /tmp/p70/packet-062.plain = 314 baytlık check; cevap yukarıdaki 70 baytla karşılaştırılır.
# İçeriği değiştirip aynı konumda denemek için:
#   --session 70 --until 61 --feed @/tmp/p70/packet-062-edited.plain
```
`--feed` sunucu modülü için kalibre edildi (builtin modülün RC4 alanı farklı offsette).
