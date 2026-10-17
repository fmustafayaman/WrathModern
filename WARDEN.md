# Warden — durum ve devam notları

Amaç: native macOS/arm64 WrathModern istemcisinin Warden etkin sunucularda (Grimfall, Warmane) tam değerli istemci gibi davranması.
Yol: istemci tarafı Warden'ı emülasyonla çalıştırmak. Hile/sahte cevap YOK — sunucunun modülü gerçek kod olarak çalışıyor; "honest-compat".

> **GÜNCEL DURUM (2026-10-17 öğle, en altta güncelleme var):** Sunucu modülü artık **yükleniyor, oluşturuluyor, initialize oluyor, sunucu paketlerini tüketiyor ve cevap üretiyor** — ama modülün `Process`'i okunamayan belleğe erişip fault veriyor ve `LCMapStringA` anlamsız argümanlarla çağrılıyor. El sıkışma tamamlanmadığı için karakter listesi gelmiyor.
> **Takıldığım yer:** Gerçek oturum olmadan (doğru oturum anahtarı) bu iki hatayı offline çoğaltamıyorum. Bu yüzden tanı dump'ları eklendi; **bir kez daha login** gerekiyor — sonrası tamamen offline. Ayrıntı: en alttaki "NEREDE TAKILDIK" bölümü.


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
Sonuç: .load ok → .create ok → .initialize ok (result 1) → .processed → .done → .shutdown ok; StateLoad(520)/StateSave(520) modülün kendi state akışı köprüden düzgün geçiyor. Bu, BMC GÖNDERDRAW Handshake yok duman testinde bekleniris (sunucu verisi olmadan modül el sıkışma üretmez); gerçek test Grimfall'a karşı.

## KALAN — SIRADAKİ ADIMLAR
1. Kullanıcı Grimfall login (loradona) → `Logs/` altında `warden-module-image.bin` (şifresiz görüntü), `warden-session-key.bin`, `warden-traffic.log`, `warden-module-key.bin` oluşur → **tam offline replay**.
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
- Grimfall branch sadece System.mm (Win/x86 raporu) + GRIMFALL.md; warden emülasyonu macos'a merge ederken platform-override da gelir (kullanıcı onaylı yol: "replicate or merge into whatever branch the fix lands on").
- Kullanıcı Türkçe konuşur; hile özelliği ve sahte-check cevabı KESİNLİKLE yok.

## BİLGİ (sunucu tarafında beklenti)
- Grimfall: SMSG_WARDEN_DATA 0x02E6 AUTH_RESPONSE'dan ÖNCE geliyor; CMSG_CHAR_ENUM 0x0037 warden cevabına kadar tutuluyor olabilir.
- İkinci şüpheli (warden yeterli olmazsa): istemci `clientextensions.dll` (Grimfall içerik modu) kendi handshake paketini gönderebilir.
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


## MODÜL IMPORT KATMANI (Grimfall blocker'ı) — 2026-10-17 (devam)

Bulgu (gerçek Grimfall oturumunun warden.log'u): yerleşik modül sunucu modülünü
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

Durum: macos + grimfall derlendi, iki app güncellendi. Smoke yeşil. Sıradaki: kullanıcı
Grimfall login → modül artık YÜKLENMELİ; log'da "import X was called but has no behavior"
veya "RaiseException/RtlUnwind" satırları çıkarsa o çağrılar modülün gerçekten kullandığı
yollardır → sırayla gerçek davranış eklenir.

---

## NEREDE TAKILDIK — Grimfall el sıkışması (2026-10-17 akşam, güncel)

### Ne ÇALIŞIYOR (kanıtlanmış)
Gerçek Grimfall oturumunun `Logs/warden.log`'u (2. oturum, 11:22):
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
1. Kullanıcı Grimfall'a **bir kez** giriş yapar (karakter listesine kadar beklemesi yeterli).
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

## SIRADAKİ BLOKAJ: Grimfall `ClientExtensions` kontrolü
Hatasız çalışan modül, sunucunun isteğiyle `OsLoadLibrary("Data\Extensions\ClientExtensions")` yapıyor:
Grimfall'ın içerik modu DLL'i yüklü mü diye bakıyor. Native istemcide bu Windows DLL'i yok; emüle loader
dürüstçe 0 döndürüyor. Sunucu karakter listesini büyük ihtimalle bu kontrole bağlıyor. Sahte handle
vermek hile korumasını kandırmak olur — YAPILMAYACAK. Yollar: (1) Grimfall geliştiricileriyle konuşmak
(native istemci için istisna), (2) ClientExtensions'ın işlevini native yazmak; ama sunucu DLL'in kendisini
de inceliyorsa bu da yetmez. Ayrıca modül `QueryDosDeviceA("IPSect")` soruyor (emülasyonda cihaz yok → başarısız).

## Bellek haritası (2026-10-17, commit f3e0fe1)
Canlı oturumda modül `VirtualQuery` ile tüm adres alanını tarıyor ve `MEM_FREE` olup `PAGE_NOACCESS`
olmayan bölgeleri gizli kod adayı sayıp okuyor (rva 0x3950 döngüsü). Emülatör boşlukları 4 KB/koruma 0,
her şeyi özel RWX olarak bildiriyordu → 0x10000/0x201000 okumalarında fault. Artık `m_memoryMap`
Windows görünümünü tutuyor (boşluk MEM_FREE/NOACCESS ve gerçek boyut; Wow.exe bölüm bölüm MEM_IMAGE;
modül BLLLoader'ın bıraktığı gibi; stack/heap özel RW; import stub sayfası sistem kütüphanesi kodu olarak
MEM_IMAGE RX). VirtualAlloc/Protect/Free bu haritayı günceller. Replay 3000 ek kareyle hatasız; tarama
kullanıcı alanını uçtan uca dolaşıyor. Not: MISC (OurLibrary stub'ları, STOP) gerçek süreçte istemcinin
kendi verisine/koduna denk geldiği için özel RW bildiriliyor.
