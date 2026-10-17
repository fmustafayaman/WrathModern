#if defined(WOWDECOMP_ARCH_32)

#include "WoW/Source/Game/GameClient/WardenClient.h"
#include "Engine/Source/Os/BLLLoader.h"
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
#include <cstring>
#include <new>

// STATIC: 0x00AF29AC
static WardenClient::OurLibrary s_ourLibrary;

// STATIC: 0x00D31A44
static BLLLoader* s_pendingModule;

// STATIC: 0x00D31A48
static BLLLoader* s_activeModule;

// STATIC: 0x00D31A4C
static WardenModule* s_moduleInterface;

// STATIC: 0x00D31A58
static uint32_t s_lastProcessTime;

// STATIC: 0x00D31A60
static SCritSect s_wardenCritSect;

// STATIC: 0x00D31A50
static uint8_t* s_savedState;

// STATIC: 0x00D31A54
static uint32_t s_savedStateSize;


// VERIFY: c0002692 auto
// OFFSET: 0x007DA2C0
static void UnloadPendingModule() {
    if (s_pendingModule) {
        s_pendingModule->Unload();

        BLLLoader* module = s_pendingModule;

        if (module) {
            module->~BLLLoader();
            SMemFree(module, "delete", SLOG_FUNCTION, 0);
        }

        s_pendingModule = nullptr;
    }
}

// VERIFY: 1f0c3c6a auto
// OFFSET: 0x007DA300
static void UnloadActiveModule() {
    if (s_activeModule) {
        s_moduleInterface->Shutdown();
        s_activeModule->Unload();

        BLLLoader* module = s_activeModule;

        if (module) {
            module->~BLLLoader();
            SMemFree(module, "delete", SLOG_FUNCTION, 0);
        }

        s_activeModule = nullptr;
        s_moduleInterface = nullptr;
    }
}

// VERIFY: ef688b60 auto
// OFFSET: 0x007DA360
static bool ActivateNextModule() {
    if (!s_pendingModule) {
        return false;
    }

    UnloadActiveModule();

    WARDENMODULEEXPORT exportFunc = reinterpret_cast<WARDENMODULEEXPORT>(s_pendingModule->GetExport(1));

    if (exportFunc) {
        s_moduleInterface = exportFunc(&s_ourLibrary);

        if (s_moduleInterface) {
            s_moduleInterface->Initialize(ClientServices::Connection()->GetSessionKey(), sizeof(LoginData::m_sessionKey));
            s_activeModule = s_pendingModule;
            s_pendingModule = nullptr;

            return true;
        }
    }

    UnloadPendingModule();

    return false;
}

// VERIFY: eed4f948 auto
// OFFSET: 0x007DA3D0
void WardenClient_Process() {
    s_wardenCritSect.Enter();

    uint32_t now = OsGetAsyncTimeMs();
    uint32_t elapsed = now - s_lastProcessTime;
    s_lastProcessTime = now;

    if (s_activeModule) {
        s_moduleInterface->Process(elapsed);
        ActivateNextModule();
    }

    s_wardenCritSect.Leave();
}

// VERIFY: bbb9c719 auto
// OFFSET: 0x007DA420
void WardenClient_Destroy() {
    s_wardenCritSect.Enter();
    UnloadPendingModule();
    UnloadActiveModule();

    if (s_savedState) {
        SMemFree(s_savedState, __FILE__, __LINE__, 0);
    }

    s_savedState = nullptr;
    s_savedStateSize = 0;
    ClientServices::ClearMessageHandler(SMSG_WARDEN_DATA);
    s_wardenCritSect.Leave();
}

// VERIFY: 79b6978e auto
// OFFSET: 0x007DA4B0
void* WardenClient::OurLibrary::MemoryAlloc(uint32_t size) {
    return SMemAlloc(size, __FILE__, __LINE__, 0);
}

// VERIFY: fabce06c auto
// OFFSET: 0x007DA4D0
void WardenClient::OurLibrary::MemoryFree(void* ptr) {
    SMemFree(ptr, __FILE__, __LINE__, 0);
}

// VERIFY: a525f101 auto
// OFFSET: 0x007DA4F0
void WardenClient::OurLibrary::StateSave(const void* data, uint32_t size) {
    if (s_savedState) {
        SMemFree(s_savedState, __FILE__, __LINE__, 0);
    }

    s_savedState = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    s_savedStateSize = size;
    memcpy(s_savedState, data, size);
}

// VERIFY: f566435f auto
// OFFSET: 0x007DA550
bool WardenClient::OurLibrary::StateLoad(void* data, uint32_t& size) {
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

// VERIFY: 1b770d98 manual
// OFFSET: 0x007DA610
static BLLLoader* LoadModule(const void* data, const uint8_t* key, uint32_t size) {
    if (size < 264) {
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
    SSignatureVerifyStream_ProvideData(signature, reinterpret_cast<const uint8_t*>(name), SStrLen(name));
    SSignatureVerifyStream_ProvideData(signature, &buffer[size - 260], 260);

    if (!SSignatureVerifyStream_Finish(signature, s_moduleModulus, s_moduleExponent)) {
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
        SMemFree(buffer, __FILE__, __LINE__, 0);
        SMemFree(image, __FILE__, __LINE__, 0);
        return nullptr;
    }

    SMemFree(buffer, __FILE__, __LINE__, 0);

    void* mem = SMemAlloc(sizeof(BLLLoader), __FILE__, __LINE__, 0);

    if (!mem) {
        SMemFree(image, __FILE__, __LINE__, 0);
        return nullptr;
    }

    BLLLoader* module = new (mem) BLLLoader();

    if (!module->Load(image, imageSize)) {
        SMemFree(image, __FILE__, __LINE__, 0);
        module->~BLLLoader();
        SMemFree(module, "delete", SLOG_FUNCTION, 0);
        return nullptr;
    }

    SMemFree(image, __FILE__, __LINE__, 0);
    return module;
}

// VERIFY: 77b94fe5 auto
// OFFSET: 0x007DA850
static int32_t ReceiveWardenData(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    if (msgId != SMSG_WARDEN_DATA) {
        return 0;
    }

    s_wardenCritSect.Enter();

    uint32_t size = msg->m_size - msg->m_read;
    void* data;
    msg->GetDataInSitu(data, size);

    uint32_t consumed = 0;

    if (s_activeModule) {
        s_moduleInterface->Data(data, size, &consumed);
        ActivateNextModule();
    }

    s_wardenCritSect.Leave();
    return 1;
}

// VERIFY: 62b407d6 auto
// OFFSET: 0x007DA8C0
bool WardenClient_Initialize() {
    s_wardenCritSect.Enter();
    WardenClient_Destroy();
    s_pendingModule = LoadModule(s_builtinModule, s_builtinModuleKey, sizeof(s_builtinModule));
    ActivateNextModule();
    s_savedState = nullptr;
    s_savedStateSize = 0;
    s_lastProcessTime = OsGetAsyncTimeMs();
    ClientServices::SetMessageHandler(SMSG_WARDEN_DATA, ReceiveWardenData, nullptr);
    s_wardenCritSect.Leave();
    return false;
}

// VERIFY: 259d45fa auto
// OFFSET: 0x007DA930
bool WardenClient::OurLibrary::ModuleUse(const WardenClient::Id& id, const WardenClient::Key& key) {
    WOWGUID guid = { 0 };
    WardenCachedModule* cached = g_wardenCache.GetRecord(*reinterpret_cast<const CWardenKey*>(&id), guid, nullptr, nullptr, false);

    if (!cached) {
        return false;
    }

    UnloadPendingModule();
    s_pendingModule = LoadModule(cached->m_data, reinterpret_cast<const uint8_t*>(&key), cached->m_size);
    return s_pendingModule != nullptr;
}

// VERIFY: b5cb950a manual
// OFFSET: 0x007DA9F0
void WardenClient::OurLibrary::ModuleCache(const WardenClient::Id& id, const void* data, uint32_t size) {
    const CWardenKey& cacheKey = *reinterpret_cast<const CWardenKey*>(&id);
    g_wardenCache.Invalidate(cacheKey);

    WardenCachedModule module;

    if (data) {
        module.m_size = size;
        module.m_data = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
        memcpy(module.m_data, data, size);
    }

    g_wardenCache.AddItem(&module, cacheKey);
}

// VERIFY: 710b5512 manual
// OFFSET: 0x007DAAB0
void WardenClient::OurLibrary::Data(const void* data, uint32_t size) {
    WDataStore msg;
    msg.Put(static_cast<uint32_t>(CMSG_WARDEN_DATA));

    for (uint32_t i = 0; i < size; i++) {
        msg.Put(static_cast<const uint8_t*>(data)[i]);
    }

    msg.Finalize();
    ClientServices::Send2(&msg);
}

#endif
