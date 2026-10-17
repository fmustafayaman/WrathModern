# Grimfall — durum notu

WrathModern'in native macOS istemcisiyle Grimfall'a bağlanma çalışmasının durumu.
Son güncelleme: 2026-10-17.

## Özet

| Adım | Durum |
|---|---|
| Realmlist adresi (`XRL1:` biçimi) | ✅ Çözülüyor (`151.80.21.213:3724`) |
| Login düğmesi (Scan.dll engeli) | ✅ Düzeltildi |
| Giriş sunucusu (GRUNT) | ✅ `LOGIN_STATE_AUTHENTICATED` |
| Oyun sunucusu kimlik doğrulama | ✅ `AUTH_OK` (istemci platformu `Win` bildirince) |
| Warden modülü (emülasyon) | ✅ Hatasız çalışıyor, donma giderildi (bkz. WARDEN.md) |
| Karakter listesi (`CMSG_CHAR_ENUM`) | ⛔ Sunucu cevap vermiyor — modül `ClientExtensions` DLL'ini arıyor |
| Karakter ekranında "realm split" döngüsü | ➖ Orijinalde de var, bırakıldı |

## Dosyalar ve konumlar

- **Grimfall istemcisi:** `/Volumes/Yaman SSD/GimfallWow/Wotlk`
- **Grimfall başlatıcısı:** `/Volumes/Yaman SSD/Grimfall Launcher` (Electron). Wow.exe'yi argümansız çalıştırıyor, realmlist'e dokunmuyor.
- **Grimfall'ın Windows eklentileri:**
  - `Data/extensions/clientextensions.dll` (4,5 MB, metinleri gizlenmiş). Grimfall'ın içerik modu: yeni ırklar, sınıflar ve diğer özel içerik. Warden ya da hile korumasıyla ilgisi yok.
  - `Data/extensions/discord_game_sdk.dll`.
  - `DivxDecoder.dll` yükleyicisi, `dlls.txt` ve `mods/` (Wine/Apple Silicon yamaları).
- **Eklentinin kullandığı özel DBC'ler** (`Logs/Extensions.log` ve DLL içi metinlerden): `MysticEnchantments`, `StoreItemEntryRealmDisable`, `CharacterGeosetMapping`.
- **Loglar:**
  - `Logs/connection.log` — giriş akışı ve "Login ignored" teşhisi.

## Branch ve derleme

- **`macos`:** Ana branch, ChromieCraft için. Ana klasörde: `~/Projects/WrathModern`.
- **`grimfall`:** `macos`'un üstünde Grimfall'a özel değişiklikler. Ayrı worktree'de: `~/Projects/WrathModern-grimfall`. `macos`'u etkilememesi için ayrı tutuluyor.
- **Derleme:** `ninja -C ~/Projects/WrathModern-grimfall/build/mac`. Çıktı otomatik olarak Grimfall klasöründeki `WrathModern.app`'e kopyalanır.
  - Yapılandırma: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON -DWRATHMODERN_GAME_DIR="/Volumes/Yaman SSD/GimfallWow/Wotlk"`

### `macos`'ta olan (Grimfall için de geçerli)

- **`d7033e4`:** `XRL1:` realmlist biçimini çözer.
  - Biçim: adresin onaltılık gösterimi, sabit anahtarla XOR'lanmış.
  - Anahtar: `Qx7P9rT2mL5vC`, 13 bayt; mevcut adresten çıkarıldı.
  - Uzun adreslerde anahtar daha uzun olabilir.
  - Kod: `WoW/Source/WowSvcs/WowSvcsClient/ClientServices.cpp` → `DecodeRealmList`.
- **`60b885d`:**
  - Scan.dll macOS'ta çalışamadığında tarama "temiz" sayılır; önceden Login sessizce yok sayılıyordu.
  - Giriş yok sayılırsa nedeni `connection.log`'a yazılır.

### Yalnızca `grimfall`'da olan

- **`27ae3ee`:** İstemci platformu `OSX` yerine `Win` olarak bildirilir (`Engine/Source/Os/Mac/System.mm` → `OsGetOsId`). `OSX` ile oyun sunucusu doğrulamadan hemen sonra bağlantıyı kesiyordu.

## Takıldığımız yer: karakter listesi

Oyun sunucusu doğrulamasından sonra paket akışı:

```
RECV 0x01EC SMSG_AUTH_CHALLENGE
SEND 0x01ED CMSG_AUTH_SESSION
RECV 0x02E6 SMSG_WARDEN_DATA        ← doğrulamadan önce geliyor
RECV 0x01EE SMSG_AUTH_RESPONSE      (AUTH_OK)
RECV 0x02EF SMSG_ADDON_INFO
RECV 0x04AB SMSG_CLIENTCACHE_VERSION
RECV 0x00FD SMSG_TUTORIAL_FLAGS
SEND 0x04FF CMSG_READY_FOR_ACCOUNT_DATA_TIMES
SEND 0x0037 CMSG_CHAR_ENUM          ← cevap hiç gelmiyor
RECV 0x0209 SMSG_ACCOUNT_DATA_TIMES
```

- Sunucu `SMSG_WARDEN_DATA` gönderiyor. Bizim istemcide Warden yalnızca boş bir kalıp; paketi alıyor, cevap göndermiyor (`WoW/Source/Game/GameClient/Win64/WardenClient.cpp`).
- Sunucu karakter listesini Warden cevabına bağlamış görünüyor. Wine'daki Windows istemcisi (gerçek Wow.exe ve eklentileriyle) aynı noktadan sorunsuz geçiyor.
- Sunucu tarafında bir hesap istisnası tanımlanırsa (Warden'ın bu hesap için kapatılması), istemci büyük ihtimalle bu noktayı geçer.
- İkinci olasılık: `clientextensions.dll` oyun sunucusuna bağlanınca kendine özel bir el sıkışma paketi gönderiyor ve sunucu karakter listesini bu pakete bağlıyor. Öyleyse yapılacak iş, o paketi native istemciye eklemek.
- Grimfall geliştiricilerinden öğrenilecekler:
  1. Eklentinin girişten sonra gönderdiği paket(ler)in opcode'u ve içeriği.
  2. Sunucunun karakter listesinden önce neyi beklediği.
  3. Özel ırk ve sınıflar için kullanılan DBC'ler ve paketler.

## Açık işler

### 1. "Realm split" döngüsü (bırakıldı)

Karakter listesi hiç gelmezken karakter ekranı her karede `CMSG_REALM_SPLIT` gönderiyor. ChromieCraft'ta tek çağrı görüldü; orijinal istemcide de aynı davranış var. Yalnızca listenin hiç gelmediği durumda ortaya çıkıyor, düzeltilmedi.

### 2. Karakter listesi (Warden)

- Yukarıdaki "Takıldığımız yer" bölümüne bak.
- Sunucu sahipleriyle konuşulabilir: hesap `loradona`, istemci platformu `Win`.

### 3. Grimfall içerik modu (`clientextensions.dll`) için native destek

Bu mod Grimfall'ın özel ırklarını, sınıflarını ve diğer içeriğini istemciye ekliyor. Native istemcide karşılıkları henüz yok; uyumluluk işi olarak yapılacak:
- Özel DBC'leri okumak (`MysticEnchantments`, `StoreItemEntryRealmDisable`, `CharacterGeosetMapping` ve ırk/sınıf tabloları).
- Karakter oluşturma ve seçim ekranında yeni ırk ve sınıfları göstermek.
- Sunucunun bu içerik için gönderdiği özel paketleri işlemek.
- `Data/patch-*.mpq` içindeki özel modeller ve dokular zaten normal yükleniyor.

Karakter listesi geldiğinde (madde 2) hangi verinin eksik kaldığı oradan görülecek.

### 4. Temizlik

- `XRL1` çözümünü istersen `macos`'tan `grimfall`'a taşı; şu an `macos`'ta duruyor ve başka adresleri etkilemiyor.

## Warden ve versiyon kanıtı durumu (2026-10-17, macos merge sonrası)

- Windows modülleri artık Unicorn emülasyonuyla çalışıyor (Engine/Source/Os/WardenEmu.h). İstek: düz oturum,
  `Logs/warden.log` ve `Logs/connection.log` izle; SMSG_WARDEN_DATA modül CMSG ile yanıtlarsa karakter listesi
  beklenir. warden.log hex dump'lar içerir: "SMSG payload" gelen, "CMSG_WARDEN_DATA payload" giden.
- Versiyon kanıtı artık oyun klasöründeki Wow.exe/DivxDecoder.dll/unicows.dll'den üretilir (Windows kanıtı birebir).
  Verdigi dijital iz: `Tools/Wardendump/WardenVersionProbe "<oyun klasörü>"` (build: `cmake --build build/mac --target WardenVersionProbe`).
- DİKKAT: DivxDecoder.dll değiştirilmişse (mod loader'ları değiştirir; klasörde .bak duruyor) kanıt orijinal olmaz.
  Warmane/kesin denetimi olan sunucular için orijinal .bak dosyasını geri koymak gerekir.

## Durum (2026-10-17 12:14 girişi)
- Warden el sıkışması baştan sona hatasız tamamlanıyor: MODULE_USE → OK, HASH_REQUEST → HASH_RESULT,
  modüle özel kontrol isteği (288 B) → modül raporu (38 B). Fault yok, donma yok (AUTH_OK → karakter isteği 0,5 sn).
- Sunucu rapordan sonra hiçbir şey göndermiyor; `COP_GET_CHARACTERS` cevapsız kalıyor.
- Raporun içinde modülün `OsLoadLibrary("Data\Extensions\ClientExtensions")` sonucu var: native istemcide
  bu DLL yok → "yüklü değil". Karakter listesi büyük ihtimalle buna (ya da DLL'in kendi el sıkışma paketine)
  bağlı. Sahte "yüklü" cevabı verilmeyecek.
- Kalan yollar: (1) Grimfall geliştiricilerinden native istemci için istisna, (2) clientextensions.dll'in
  sunucuyla protokolünü çözüp native yazmak (DLL 4,5 MB ve gizlenmiş; sunucu DLL'in kendisini de
  inceliyorsa yetmeyebilir).
