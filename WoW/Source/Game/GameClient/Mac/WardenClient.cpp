#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

/*
 * Warden client for the macOS port.
 *
 * This mirrors WoW/Source/Game/GameClient/Win32/WardenClient.cpp: the Win32 build loads the
 * server module natively in-process, but the macOS build runs 32-bit Windows module images in
 * the emulated loader (Engine/Source/Os/WardenEmu.h) instead. The flow, caching and the data
 * paths are the same as the original; only the loader differs.
 *
 * The emulation executes the module's own code, so the warden protocol behaves as authored.
 */

#include "WoW/Source/Game/GameClient/WardenClient.h"
#include "Engine/Source/Os/WardenEmu.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/SLock.h"
#include "WoW/Source/Net/NetClient/NetClient.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"
#include "Engine/Source/Base/CDataStore.h"
#include "WoW/Source/DB/DBClient/DBCacheInstances.h"
#include "WoW/Source/Net/Types.h"
#include "WoW/Source/WowServices/WDataStore.h"
#include "ThirdParty/zlib/zlib.h"
#include "WoW/Source/Game/GameClient/WardenModuleData.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

// STATIC equivalents of the Win32 file.
WardenEmu* s_pendingEmu;
WardenEmu* s_activeEmu;

uint8_t* s_savedState;
uint32_t s_savedStateSize;

uint32_t s_lastProcessTime;

SCritSect s_wardenCritSect;

// ---------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------

void WardenClientLog(const char* format, ...) {
    // The app chdirs into the game directory; the Logs subdirectory is shared with the other
    // client logs and may not exist on a first run, so fall back next to the application.
    FILE* file = fopen("Logs/warden.log", "a");

    if (!file) {
        file = fopen("warden.log", "a");
    }

    if (!file) {
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fclose(file);
}

// Later parts of the flow log the shape of the warden conversation: the first bytes of every
// server packet and of every module reply.
static void WardenLogHex(const char* what, const uint8_t* data, uint32_t size) {
    if (size > 62) {
        size = 62; // first packet shapes matter; keep it to one line
    }

    char hex[256];
    uint32_t hexUsed = 0;

    for (uint32_t i = 0; i < size && hexUsed + 3 < sizeof(hex); i++) {
        hexUsed += static_cast<uint32_t>(snprintf(hex + hexUsed, sizeof(hex) - hexUsed, "%02x ", data[i]));
    }

    WardenClientLog("%s bytes=%u %s", what, size, hex);
}

// The full packet trace, byte for byte, in a format a tool can replay: one line per packet,
// "SMSG <hex>" for what the server sent and "CMSG <hex>" for what the module answered. The
// summary log above truncates packets; this one does not, so a single session is enough to
// reproduce a whole warden conversation offline.
static void WardenTrace(const char* tag, const uint8_t* data, uint32_t size) {
    FILE* file = fopen("Logs/warden-traffic.log", "a");

    if (!file) {
        file = fopen("warden-traffic.log", "a");
    }

    if (!file) {
        return;
    }

    fputs(tag, file);
    fputc(' ', file);

    for (uint32_t i = 0; i < size; i++) {
        fprintf(file, "%02x", data[i]);
    }

    fputc('\n', file);
    fclose(file);
}

// ---------------------------------------------------------------------
// The client services handed to the emulated module
// ---------------------------------------------------------------------

class MacWardenServices : public WardenEmuServices {
public:
    // WardenClient::OurLibrary::Data in the Win32 file.
    void ModuleOutput(const uint8_t* data, uint32_t size) override {
        WardenLogHex("CMSG_WARDEN_DATA payload", data, size);
        WardenTrace("CMSG", data, size);

        WDataStore msg;
        msg.Put(static_cast<uint32_t>(CMSG_WARDEN_DATA));

        for (uint32_t i = 0; i < size; i++) {
            msg.Put(data[i]);
        }

        msg.Finalize();
        ClientServices::Send2(&msg);
    }

    // WardenClient::OurLibrary::ModuleUse in the Win32 file.
    bool ModuleUse(const uint8_t* id, const uint8_t* key) override {
        WOWGUID guid = { 0 };
        const CWardenKey& cacheKey = *reinterpret_cast<const CWardenKey*>(id);

        WardenClientLog("ModuleUse id=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
                        "key=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11], id[12], id[13],
            id[14], id[15], key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], key[8], key[9],
            key[10], key[11], key[12], key[13], key[14], key[15]);

        WardenCachedModule* cached = g_wardenCache.GetRecord(cacheKey, guid, nullptr, nullptr, false);

        if (!cached) {
            WardenClientLog("ModuleUse: cache miss for %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11],
                id[12], id[13], id[14], id[15]);
            return false;
        }

        // The loader decrypts the cached module with this key; keeping it lets the same blob be
        // decrypted offline too.
        {
            FILE* keyFile = fopen("Logs/warden-module-key.bin", "wb");

            if (keyFile) {
                fwrite(key, 1, 16, keyFile);
                fclose(keyFile);
            }
        }

        UnloadPendingModule();
        s_pendingEmu = LoadModule(cached->m_data, key, cached->m_size);
        return s_pendingEmu != nullptr;
    }

    // WardenClient::OurLibrary::ModuleCache in the Win32 file.
    void ModuleCache(const uint8_t* id, const void* data, uint32_t size) override {
        const CWardenKey& cacheKey = *reinterpret_cast<const CWardenKey*>(id);

        // Keep a copy of every server-delivered module next to the logs; offline analysis of
        // its imports and code keeps new emulator work grounded in what servers really send.
        {
            char name[64];
            snprintf(name, sizeof(name), "Logs/warden-module-%02x%02x%02x%02x.bin", id[12], id[13], id[14],
                id[15]);

            FILE* file = fopen(name, "wb");

            if (file) {
                fwrite(data, 1, size, file);
                fclose(file);
                WardenClientLog("ModuleCache: wrote %s (%u bytes)", name, size);
            } else {
                file = fopen("warden-module.bin", "wb");

                if (file) {
                    fwrite(data, 1, size, file);
                    fclose(file);
                    WardenClientLog("ModuleCache: wrote warden-module.bin fallback (%u bytes)", size);
                }
            }
        }

        g_wardenCache.Invalidate(cacheKey);

        WardenCachedModule module;

        if (data) {
            module.m_size = size;
            module.m_data = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
            memcpy(module.m_data, data, size);
        }

        g_wardenCache.AddItem(&module, cacheKey);
    }

    // WardenClient::OurLibrary::StateSave in the Win32 file.
    void StateSave(const void* data, uint32_t size) override {
        if (s_savedState) {
            SMemFree(s_savedState, __FILE__, __LINE__, 0);
        }

        s_savedState = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
        s_savedStateSize = size;
        memcpy(s_savedState, data, size);
    }

    // WardenClient::OurLibrary::StateLoad in the Win32 file.
    bool StateLoad(void* data, uint32_t& size) override {
        uint8_t* state = s_savedState;

        if (!state) {
            return false;
        }

        if (size > s_savedStateSize) {
            size = s_savedStateSize;
        }

        memcpy(data, state, size);
        SMemFree(state, __FILE__, __LINE__, 0);
        s_savedState = nullptr;
        s_savedStateSize = 0;
        return true;
    }

    static void UnloadPendingModule() {
        if (!s_pendingEmu) {
            return;
        }

        WardenClientLog("unloading pending module (already superseded)");
        delete s_pendingEmu;
        s_pendingEmu = nullptr;
    }

    // The Win32 LoadModule: RC4-decrypt, verify the module signature against the embedded
    // modulus, inflate, and hand the image to the loader.
    static WardenEmu* LoadModule(const void* data, const uint8_t* key, uint32_t size) {
        if (size < 264) {
            WardenClientLog("LoadModule: image too small (%u)", size);
            return nullptr;
        }

        uint8_t* buffer = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));

        if (!buffer) {
            return nullptr;
        }

        memcpy(buffer, data, size);

        SARC4Key rc4;
        SARC4PrepareKey(key, 16, &rc4);
        SARC4ProcessBuffer(buffer, size, &rc4, &rc4);

        char name[16];
        name[0] = 'M';

        for (uint32_t i = 0;; i++) {
            char c = static_cast<char>(s_moduleNameCodes[i] * 0xB9);
            name[i + 1] = c;

            if (!c) {
                break;
            }
        }

        SStrUpper(name);

        SSignatureData* signature;
        SSignatureVerifyStream_Begin(&signature, 256, 4);
        SSignatureVerifyStream_ProvideData(signature, buffer, size - 260);
        SSignatureVerifyStream_ProvideData(signature, reinterpret_cast<const uint8_t*>(name),
            SStrLen(name));
        SSignatureVerifyStream_ProvideData(signature, &buffer[size - 260], 260);

        if (!SSignatureVerifyStream_Finish(signature, s_moduleModulus, s_moduleExponent)) {
            WardenClientLog("LoadModule: signature check failed (%u bytes)", size);
            SMemFree(buffer, __FILE__, __LINE__, 0);
            return nullptr;
        }

        uLongf imageSize = *reinterpret_cast<uint32_t*>(buffer);
        uint8_t* image = static_cast<uint8_t*>(SMemAlloc(imageSize, __FILE__, __LINE__, 0));

        if (!image) {
            SMemFree(buffer, __FILE__, __LINE__, 0);
            return nullptr;
        }

        if (uncompress(image, &imageSize, &buffer[4], size - 4)) {
            WardenClientLog("LoadModule: uncompress failed");
            SMemFree(buffer, __FILE__, __LINE__, 0);
            SMemFree(image, __FILE__, __LINE__, 0);
            return nullptr;
        }

        SMemFree(buffer, __FILE__, __LINE__, 0);

        // Keep the exact image handed to the loader: WardenSmoke takes it directly, so a whole
        // session can be replayed offline without the encryption key.
        {
            FILE* dump = fopen("Logs/warden-module-image.bin", "wb");

            if (dump) {
                fwrite(image, 1, imageSize, dump);
                fclose(dump);
                WardenClientLog("LoadModule: wrote Logs/warden-module-image.bin (%u bytes)", (uint32_t)imageSize);
            }
        }

        WardenEmu* module = new WardenEmu(&macServices);

        if (!module->Load(image, imageSize)) {
            WardenClientLog("LoadModule: guest load failed (%u bytes image)", (uint32_t)imageSize);
            SMemFree(image, __FILE__, __LINE__, 0);
            delete module;
            return nullptr;
        }

        SMemFree(image, __FILE__, __LINE__, 0);
        WardenClientLog("LoadModule: module accepted (%u bytes image)", (uint32_t)imageSize);
        return module;
    }

    static MacWardenServices macServices;
};

MacWardenServices MacWardenServices::macServices;

// ---------------------------------------------------------------------
// Module lifecycle helpers, mirroring ActivateNextModule / Unload*
// ---------------------------------------------------------------------

void UnloadActiveModule() {
    if (!s_activeEmu) {
        return;
    }

    s_activeEmu->Shutdown();
    delete s_activeEmu;
    s_activeEmu = nullptr;
}

bool ActivateNextModule() {
    if (!s_pendingEmu) {
        return false;
    }

    UnloadActiveModule();

    if (!s_pendingEmu->CreateModule()) {
        MacWardenServices::UnloadPendingModule();
        return false;
    }

    bool ok = s_pendingEmu->Initialize(ClientServices::Connection()->GetSessionKey(),
        sizeof(LoginData::m_sessionKey));

    if (!ok) {
        WardenClientLog("ActivateNextModule: module Initialize failed");
        MacWardenServices::UnloadPendingModule();
        return false;
    }

    s_activeEmu = s_pendingEmu;
    s_pendingEmu = nullptr;
    return true;
}

// ---------------------------------------------------------------------
// Network plumbing
// ---------------------------------------------------------------------

int32_t ReceiveWardenData(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    if (msgId != SMSG_WARDEN_DATA) {
        return 0;
    }

    s_wardenCritSect.Enter();

    uint32_t size = msg->m_size - msg->m_read;
    void* data;
    msg->GetDataInSitu(data, size);

    WardenClientLog("SMSG_WARDEN_DATA %u bytes, active=%d", size, s_activeEmu != nullptr);
    WardenLogHex("SMSG payload", reinterpret_cast<const uint8_t*>(data), size);
    WardenTrace("SMSG", reinterpret_cast<const uint8_t*>(data), size);

    uint32_t consumed = 0;

    if (s_activeEmu) {
        s_activeEmu->Data(data, size, consumed);
        WardenClientLog("module consumed %u bytes", consumed);
        ActivateNextModule();
    } else {
        WardenClientLog("no active module; packet dropped (server warden is on)");
    }

    s_wardenCritSect.Leave();
    return 1;
}

}

// ---------------------------------------------------------------------
// Public entry points, mirroring the Win32 file
// ---------------------------------------------------------------------

void WardenClient_Destroy() {
    s_wardenCritSect.Enter();

    MacWardenServices::UnloadPendingModule();
    UnloadActiveModule();

    if (s_savedState) {
        SMemFree(s_savedState, __FILE__, __LINE__, 0);
    }

    s_savedState = nullptr;
    s_savedStateSize = 0;

    ClientServices::ClearMessageHandler(SMSG_WARDEN_DATA);
    WardenClientLog("WardenClient destroyed");
    s_wardenCritSect.Leave();
}

bool WardenClient_Initialize() {
    s_wardenCritSect.Enter();

    WardenClient_Destroy();

    s_pendingEmu = MacWardenServices::LoadModule(s_builtinModule, s_builtinModuleKey,
        sizeof(s_builtinModule));

    if (!s_pendingEmu || !ActivateNextModule()) {
        WardenClientLog("WardenClient_Initialize: builtin module not running; warden stays out of the flow");
    }

    s_savedState = nullptr;
    s_savedStateSize = 0;
    s_lastProcessTime = OsGetAsyncTimeMs();

    ClientServices::SetMessageHandler(SMSG_WARDEN_DATA, ReceiveWardenData, nullptr);
    s_wardenCritSect.Leave();

    return false;
}

void WardenClient_Process() {
    s_wardenCritSect.Enter();

    uint32_t now = OsGetAsyncTimeMs();
    uint32_t elapsed = now - s_lastProcessTime;
    s_lastProcessTime = now;

    if (s_activeEmu) {
        s_activeEmu->Process(elapsed);
        ActivateNextModule();
    }

    s_wardenCritSect.Leave();
}

#endif
