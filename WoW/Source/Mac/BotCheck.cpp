#include <cstdint>
#include "Engine/Source/Os/OsLibrary.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Base/CDataStore.h"
#include "WoW/Source/Client.h"
#include "WoW/Source/Net/Types.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/AccountData.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/FriendList.h"

// The client's bot detection hooks, as the x64 Windows build has them: the scan modules a server can
// send are Windows code and are not loaded, so the checks find nothing. Without structured exception
// handling on macOS, the check reads its probe byte directly; it is the first instruction of a
// function in the client's own text segment and always readable.

void AdditionalCheck1() {
    if (!s_botCheckEnabled || s_botCheckDelay <= 0 || s_botCheckDelay > 420) {
        return;
    }

    if (__sync_val_compare_and_swap(&s_additionalCheckArmed, 1, 0) != 1) {
        return;
    }

    AdditionalCheckInterface iface;
    iface.m_memAlloc = Client_MemAlloc;
    iface.m_memFree = Client_MemFree;
    iface.m_loadLibrary = OsLoadLibrary;
    iface.m_getLibraryEntryPoint = OsGetLibraryEntryPoint;
    iface.m_freeLibrary = OsFreeLibrary;

    // A jump (0xE9) at the start of OsValidateFunctionPointer means it has been patched on x86; on
    // arm64 that byte never begins an instruction, so the check stays as the Windows client has it.
    if (*reinterpret_cast<const uint8_t*>(&OsValidateFunctionPointer) == 0xE9) {
        return;
    }

    if (AdditionalCheck1Scan(&iface, 0)) {
        SendBotDetected2(CMSG_BOT_DETECTED2, 0x60231D69);
    }
}

void InitTokenSeed(const uint8_t* key) {
}

int32_t Packet_SMSG_CHECK_FOR_BOTS(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    uint8_t key[16];
    msg->GetArray(key, sizeof(key));

    return 1;
}

int32_t GatherAdditionalChecks(uint8_t* check1, uint8_t* check2, uint8_t* check3) {
    *check1 = 0;
    *check2 = 0;
    *check3 = 0;

    return 0;
}

uint8_t AdditionalCheck1Scan(const AdditionalCheckInterface* iface, int32_t unused_a2) {
    return 0;
}

void LoadEncryptedModule(const uint8_t* key, uint32_t length, void (*callback)()) {
}
