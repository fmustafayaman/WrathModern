# Warden — durum ve devam notları

Amaç: native macOS/arm64 WrathModern istemcisinin Warden etkin sunucularda (Grimfall, Warmane) tam değerli istemci gibi davranması.
Yol: istemci tarafı Warden'ı emülasyonla çalıştırmak. Hile/sahte cevap YOK — sunucunun modülü gerçek kod olarak çalışıyor; "honest-compat".

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
1. Commit (macos branch) → grimfall merge → grimfall worktree'de derle. (Kullanıcı GRIMFALL.md'deki klasöre app atıp test eder; login: loradona)
2. `warden.log` (CWD'de) uzunluğu: module created / Initialize / SMSG_WARDEN_DATA n bytes / module consumed n lines — tüm akışı gösterir. SMSG_WARDEN_DATA geldiğinde "module consumed" satırı ve CMSG çıkmıyorsa log'da fault/import satırlarına bak.
3. Warmane ayrı: "oyun sürümü farklı" — versiyon gate muhtemelen SMSG_AUTH_RESPONSE öncesi platform/versiyon denetimi; Warden bitince bakılır.
4. Custom modüllerde import gelirse: log'daki `import xxx!yyy has no emulator implementation` satırına göre köprüde o fonksiyon için gerçek GuestAlloc-tabanlı implementasyon eklenir (kernel32 VirtualAlloc/HeapAlloc vb. zaten hazır kalıpta).
5. NOT: module Data'nın guest arg'ı SCRATCH'in dışında olabilir (modül kendi heap'ini kullanır) — okuma pointer'ından bağımsız; yalnız kidemli sınır 32K cap.

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
