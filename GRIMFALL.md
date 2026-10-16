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
| Karakter listesi (`CMSG_CHAR_ENUM`) | ⛔ Sunucu cevap vermiyor — **takıldığımız yer** |
| Karakter ekranında "realm split" döngüsü | 🔧 Bizim hata, inceleniyor |

## Dosyalar ve konumlar

- **Grimfall istemcisi:** `/Volumes/Yaman SSD/GimfallWow/Wotlk`
- **Grimfall başlatıcısı:** `/Volumes/Yaman SSD/Grimfall Launcher` (Electron). Wow.exe'yi argümansız çalıştırıyor, realmlist'e dokunmuyor.
- **Grimfall'ın Windows eklentileri:**
  - `Data/extensions/clientextensions.dll` (4,5 MB, metinleri gizlenmiş).
  - `Data/extensions/discord_game_sdk.dll`.
  - `DivxDecoder.dll` yükleyicisi, `dlls.txt` ve `mods/` (Wine/Apple Silicon yamaları).
- **Eklentinin kullandığı özel DBC'ler** (`Logs/Extensions.log` ve DLL içi metinlerden): `MysticEnchantments`, `StoreItemEntryRealmDisable`, `CharacterGeosetMapping`.
- **Loglar:**
  - `Logs/connection.log` — giriş akışı ve "Login ignored" teşhisi.
  - `Logs/realmsplit.log` — geçici Lua yığını teşhisi, aşağıya bak.

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
- **Commit edilmemiş geçici teşhis:** `WoW/Source/Glue/GlueScriptEvents_Scripts.cpp` → `Script_RequestRealmSplitInfo`. İlk 4 çağrının Lua yığınını `Logs/realmsplit.log`'a yazar. İş bitince geri al: `git checkout -- WoW/Source/Glue/GlueScriptEvents_Scripts.cpp`.

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

## Açık işler

### 1. "Realm split" döngüsü (bizim hata, tüm sunucuları ilgilendirir)

**Belirti:** Karakter listesi gelmezken istemci her karede (~16 ms) `CMSG_REALM_SPLIT` (0x38C) gönderiyor. Grimfall'da 30 saniyede 500+ istek gitti.

**Bulunanlar:**
- lldb yığını: çağrı, karakter seçim ekranının ModelFFX frame'inin `OnUpdate` script'inden geliyor (`CSimpleFrame::OnLayerUpdate` → `FrameScript_Execute` → `Script_RequestRealmSplitInfo`).
- Lua'da `RequestRealmSplitInfo()` yalnızca `CharacterSelect_OnShow` içinde geçiyor (`Interface\GlueXML\CharacterSelect.lua:160`). Grimfall'ın GlueXML dosyaları ChromieCraft'takilerle aynı.
- Şüphe 1: `GlueParent.lua` → `GlueFrameFadeUpdate`'teki geçiş efekti (`GoToPendingGlueScreen` → `SetGlueScreen("charselect")`) her karede yeniden tetikleniyor.
- Şüphe 2: frame'in `OnUpdate`'i yanlış script'e bağlanmış.

**Sonraki adım:**
- Grimfall'da giriş yap, karakter ekranında bekle.
- `Logs/realmsplit.log`'daki Lua yığınından hangi fonksiyonun çağırdığını bul.
- Düzeltmeyi `macos`'a yap; `grimfall`'ı `macos`'un üstüne rebase et.

### 2. Karakter listesi (Warden)

- Yukarıdaki "Takıldığımız yer" bölümüne bak.
- Sunucu sahipleriyle konuşulabilir: hesap `loradona`, istemci platformu `Win`.

### 3. Karakter listesinden sonrası (henüz görülmedi)

Grimfall'ın özel özellikleri Windows eklentisinde; native istemcide karşılıkları yok. Oyuna girilirse beklenenler:
- Özel DBC'ler (`MysticEnchantments` vb.) okunmayacak; bunlara dayanan arayüzler eksik ya da hatalı olabilir.
- Sunucunun özel paketleri tanınmayacak ve yok sayılacak.
- `Data/patch-*.mpq` içindeki özel modeller ve dokular normal yükleniyor.

### 4. Temizlik

- Grimfall işi bittiğinde geçici teşhis kodunu geri al.
- `XRL1` çözümünü istersen `macos`'tan `grimfall`'a taşı; şu an `macos`'ta duruyor ve başka adresleri etkilemiyor.
