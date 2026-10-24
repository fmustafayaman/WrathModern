#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "WoW/Source/Net/NetClient/BuckData.h"
#include "WoW/Source/Net/NetClient/BuckDatabases.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Os/Mac/BuckCodeEmu.h"
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Platform/Mac/Paths.h"
#include "WoW/Source/Net/NetClient/NetClient.h"
#include "WoW/Source/Net/Types.h"

/*
 * Warmane's delivered bootstrap, installer and challenge handlers run in one persistent
 * machine. They choose their addresses, request values, keys and replies. Native integration
 * only transports those packets and translates real client functions/data across the ABI.
 */

namespace {

constexpr uint32_t BUCK_INSTALL = 0x4B8;
constexpr uint32_t BUCK_LUA = 0x4B9;
constexpr uint32_t BUCK_CHALLENGE = 0x41B;
constexpr uint32_t BUCK_CLEAR = 0x41C;
constexpr uint32_t BUCK_DATA = 0x41D;
bool s_frameworkInstalled = false;

void BuckLog(const char* format, ...) {
    FILE* log = fopen("Logs/buck-data.log", "a");
    if (!log) return;
    fprintf(log, "%u ", OsGetAsyncTimeMs());
    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fputc('\n', log);
    fclose(log);
}

std::string ToHex(const uint8_t* data, uint32_t size, uint32_t limit = 64) {
    static const char digits[] = "0123456789abcdef";
    std::string text;

    for (uint32_t i = 0; i < size && i < limit; i++) {
        text += digits[data[i] >> 4];
        text += digits[data[i] & 0xF];
        text += ' ';
    }

    if (size > limit) text += "...";
    return text;
}

class ClientBuckServices : public BuckCodeServices {
public:
    bool ClientImage(const uint8_t*& data, uint32_t& size, uint32_t& base) override {
        static std::vector<uint8_t> image;
        static uint32_t preferredBase = 0;
        static bool attempted = false;

        if (!attempted) {
            attempted = true;
            char path[4096];

            if (MacPaths::FindInClientDirectory("Wow.exe", path, sizeof(path))
                && OsMacMapPeImage(path, image, preferredBase)) {
                BuckLog("client image: %s mapped (%u bytes at %08x)", path,
                    static_cast<uint32_t>(image.size()), preferredBase);
            } else {
                BuckLog("client image: no usable Wow.exe in the client directory");
                image.clear();
            }
        }

        if (image.empty()) return false;
        data = image.data();
        size = static_cast<uint32_t>(image.size());
        base = preferredBase;
        return true;
    }

    void Log(const char* line) override {
        BuckLog("%s", line ? line : "");
    }

    void RunLua(const char* source, const char* filename, const char* taint) override {
        FrameScript_Execute(source, filename, taint);
    }

    bool ClientDatabase(uint32_t address, BuckDatabase& database) override {
        return BuckReadNativeDatabase(address, database);
    }

    bool CommitClientDatabase(uint32_t address, const BuckDatabase& database) override {
        return BuckCommitNativeDatabase(address, database);
    }

    int32_t IsTrial() override {
        return SFile::IsTrial();
    }

    const char* ClientPath() override {
        static std::string result;

        if (result.empty()) {
            char path[4096];

            if (MacPaths::FindInClientDirectory("Wow.exe", path, sizeof(path))) {
                result = "Z:";
                for (const char* c = path; *c; c++) result += *c == '/' ? '\\' : *c;
            }
        }

        return result.c_str();
    }
};

BuckCodeEmu& Emulator() {
    static ClientBuckServices services;
    static BuckCodeEmu emulator(&services);
    return emulator;
}

void LogFrameworkState() {
    uint32_t state[10];

    if (Emulator().ReadFrameworkState(state)) {
        BuckLog("framework state: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
            state[0], state[1], state[2], state[3], state[4], state[5], state[6], state[7], state[8], state[9]);
    }
}

bool PatchScriptBuffer(void* buffer, uint32_t bytes, const char* chunkname) {
    return Emulator().PatchScriptBuffer(buffer, bytes, chunkname);
}

int32_t DispatchDelivered(NetClient* connection, uint32_t opcode, const uint8_t* data, uint32_t size) {
    if (!s_frameworkInstalled) {
        BuckLog("SMSG %04x (%u bytes): no delivered framework", opcode, size);
        return 1;
    }

    uint32_t handler = 0;
    uint32_t key = 0;
    Emulator().HandlerFor(BUCK_CHALLENGE, handler, key);
    std::vector<std::vector<uint8_t>> sent;

    if (!Emulator().Dispatch(opcode, OsGetAsyncTimeMs(), data, size, sent)) {
        BuckLog("SMSG %04x (%u bytes): delivered handler failed", opcode, size);
        return 1;
    }

    uint32_t nextHandler = 0;
    uint32_t nextKey = 0;

    if (Emulator().HandlerFor(BUCK_CHALLENGE, nextHandler, nextKey)) {
        connection->m_buckToken = nextHandler;
    }

    for (const auto& packet : sent) {
        BuckLog("SMSG %04x: forwarding %u bytes from delivered Send2: %s", opcode,
            static_cast<uint32_t>(packet.size()), ToHex(packet.data(), static_cast<uint32_t>(packet.size())).c_str());
        CDataStore reply;
        reply.PutArray(packet.data(), static_cast<uint32_t>(packet.size()));
        reply.Finalize();
        connection->Send(&reply);
    }

    if (opcode == BUCK_CHALLENGE) {
        BuckLog("challenge %u bytes, token %08x, key %08x, next key %08x, reply %u bytes", size,
            handler, key, nextKey, sent.empty() ? 0 : static_cast<uint32_t>(sent.front().size()));
    } else {
        LogFrameworkState();
    }

    return 1;
}

int32_t HandleLuaScript(const uint8_t* data, uint32_t size) {
    uint32_t handler = 0;
    uint32_t param = 0;
    std::vector<std::string> scripts;

    if (!s_frameworkInstalled || !Emulator().HandlerFor(BUCK_LUA, handler, param)) {
        BuckLog("SMSG 4B9 (%u bytes): no delivered Lua handler", size);
        return 1;
    }

    if (!BuckCodeEmu::DecodeLuaScripts(data, size, scripts)) {
        BuckLog("SMSG 4B9 (%u bytes): missing seed or unterminated Lua chunk; not executed", size);
        return 1;
    }

    BuckLog("delivered Lua packet (%u bytes, seed %02x): %u chunks", size, data[0],
        static_cast<uint32_t>(scripts.size()));

    for (uint32_t i = 0; i < scripts.size(); i++) {
        BuckLog("delivered Lua chunk %u/%u (%u bytes): %.200s", i + 1,
            static_cast<uint32_t>(scripts.size()), static_cast<uint32_t>(scripts[i].size()), scripts[i].c_str());
        FrameScript_Execute(scripts[i].c_str(), nullptr, nullptr);
    }

    return 1;
}

} // namespace

bool BuckDataApplyContent() {
    if (!s_frameworkInstalled || !Emulator().IsCallHooked(BuckCodeEmu::CALL_CLIENT_INITIALIZE_GAME)) return true;
    uint32_t result = 0;
    bool ok = Emulator().RunHookedCall(BuckCodeEmu::CALL_CLIENT_INITIALIZE_GAME, nullptr, 0, 0, result);
    BuckLog("delivered native content hook: %s", ok ? "ok" : "failed");
    LogFrameworkState();
    return ok;
}

int32_t BuckDataHandleCustomCode(NetClient* connection, const uint8_t* data, uint32_t size) {
    return DispatchDelivered(connection, BUCK_INSTALL, data, size);
}

int32_t BuckDataProcessMessage(NetClient* connection, uint32_t msgId, CDataStore* msg) {
    if (connection == NetClient::s_realmConnection && msgId == BUCK_INSTALL) return 0;
    uint32_t size = msg->m_size > msg->m_read ? msg->m_size - msg->m_read : 0;
    const uint8_t* data = msg->m_data + msg->m_read;
    int32_t result = 0;

    switch (msgId) {
        case MSG_BATTLEGROUND_PLAYER_POSITIONS:
            if (!Emulator().InstallFramework(data, size)) return 0;
            s_frameworkInstalled = true;
            FrameScript_SetExecuteFileHook(PatchScriptBuffer);
            BuckLog("delivered framework bootstrap ran (%u bytes)", size);
            LogFrameworkState();
            result = 1;
            break;
        case BUCK_INSTALL:
        case BUCK_CHALLENGE:
        case BUCK_CLEAR:
        case BUCK_DATA:
            result = DispatchDelivered(connection, msgId, data, size);
            break;
        case BUCK_LUA:
            result = HandleLuaScript(data, size);
            break;
        default:
            return 0;
    }

    msg->m_read = msg->m_size;
    return result;
}

#else

#include "WoW/Source/Net/NetClient/BuckData.h"

bool BuckDataApplyContent() { return true; }
int32_t BuckDataProcessMessage(NetClient*, uint32_t, CDataStore*) { return 0; }
int32_t BuckDataHandleCustomCode(NetClient*, const uint8_t*, uint32_t) { return 0; }

#endif
