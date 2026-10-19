#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "WoW/Source/Net/NetClient/BuckData.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Os/Mac/BuckCodeEmu.h"
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Platform/Mac/Paths.h"
#include "WoW/Source/Net/NetClient/NetClient.h"
#include "WoW/Source/Net/Types.h"

/*
 * The client half of Warmane's buck-data stream. Every answer here is the delivered code's
 * own: the challenge is decoded with the token this client reported, the code runs in the
 * BuckCodeEmu machine against a guest CDataStore, and the bytes it builds are sent back.
 *
 * Diagnostics land in Logs/buck-data.log: the decoded challenge, every client call the
 * delivered code made, and the reply, byte for byte.
 */

namespace {

constexpr uint32_t BUCK_OPCODE_QUERY = 0x41D; // CMSG_QUERY_SERVER_BUCK_DATA
constexpr uint32_t BUCK_OPCODE_CLEAR = 0x41C; // CMSG_CLEAR_SERVER_BUCK_DATA
constexpr uint32_t BUCK_OPCODE_CHALLENGE = 0x41B;
constexpr uint32_t BUCK_OPCODE_DATA = 0x41D; // SMSG 0x41D carries the requested data
constexpr uint32_t BUCK_OPCODE_INSTALL = 0x4B8;

void BuckLog(const char* format, ...) {
    FILE* log = fopen("Logs/buck-data.log", "a");

    if (!log) {
        return;
    }

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

    if (size > limit) {
        text += "...";
    }

    return text;
}

// The delivered code's view of this client: its executable image for the framework globals
// (0x00DD0Fxx) and import slots, and a log line for diagnostics.
class ClientBuckServices : public BuckCodeServices {
public:
    bool ClientImage(const uint8_t*& data, uint32_t& size, uint32_t& base) override {
        static std::vector<uint8_t> s_image;
        static uint32_t s_base = 0;
        static bool s_attempted = false;

        if (!s_attempted) {
            s_attempted = true;
            char path[4096];

            if (MacPaths::FindInClientDirectory("Wow.exe", path, sizeof(path))
                && OsMacMapPeImage(path, s_image, s_base)) {
                BuckLog("client image: %s mapped (%u bytes at %08x)", path,
                    static_cast<uint32_t>(s_image.size()), s_base);
            } else {
                BuckLog("client image: no Wow.exe in the client directory");
                s_image.clear();
            }
        }

        if (s_image.empty()) {
            return false;
        }

        data = s_image.data();
        size = static_cast<uint32_t>(s_image.size());
        base = s_base;
        return true;
    }

    void Log(const char* line) override {
        BuckLog("%s", line ? line : "");
    }
};

BuckCodeEmu& Emulator() {
    static ClientBuckServices services;
    static BuckCodeEmu emulator(&services);
    return emulator;
}

// The cheapest read of the blob that finds the buck request its install code will send: the
// opcode sits in a `push <imm32>` right before the message builder call (see the official
// client's traffic). The 0x41B request carries a client-side buffer address as its token, and
// the challenge that follows is encoded with exactly that value.
bool FindRequestOpcode(const uint8_t* data, uint32_t size, uint32_t& request) {
    for (uint32_t i = 0; i + 5 <= size; i++) {
        if (data[i] != 0x68) { // push imm32
            continue;
        }

        uint32_t value = static_cast<uint32_t>(data[i + 1]) | (static_cast<uint32_t>(data[i + 2]) << 8)
            | (static_cast<uint32_t>(data[i + 3]) << 16) | (static_cast<uint32_t>(data[i + 4]) << 24);

        if (value == BUCK_OPCODE_QUERY || value == BUCK_OPCODE_CLEAR || value == BUCK_OPCODE_CHALLENGE) {
            request = value;
            return true;
        }
    }

    return false;
}

int32_t HandleChallenge(NetClient* connection, const uint8_t* data, uint32_t size) {
    std::vector<uint8_t> response;

    if (!Emulator().RunChallenge(data, size, connection->m_buckToken, response)) {
        BuckLog("challenge of %u bytes with token %08x could not be answered", size, connection->m_buckToken);
        return 1;
    }

    BuckLog("challenge %u bytes, token %08x, reply %u bytes: %s", size, connection->m_buckToken,
        static_cast<uint32_t>(response.size()), ToHex(response.data(), static_cast<uint32_t>(response.size())).c_str());

    CDataStore reply;
    reply.PutArray(response.data(), static_cast<uint32_t>(response.size()));
    reply.Finalize();
    connection->Send(&reply);
    return 1;
}

}  // namespace

int32_t BuckDataHandleCustomCode(NetClient* connection, const uint8_t* data, uint32_t size) {
    uint32_t request = 0;

    if (!FindRequestOpcode(data, size, request)) {
        BuckLog("code blob of %u bytes has no buck request", size);
        return 1;
    }

    uint32_t value = 0;

    if (request == BUCK_OPCODE_CHALLENGE) {
        // The official client asks with the address of the executable buffer the blob installs
        // for the handler; the realm treats it as the session's token and encodes the follow-up
        // challenges with it. Report a plausible client-side address and remember the value.
        value = 0x04000000u | (OsGetAsyncTimeMs() & 0x00FFF000u);
        connection->m_buckToken = value;
    }

    CDataStore reply;
    reply.Put(request);
    reply.Put(value);
    reply.Finalize();
    connection->Send(&reply);

    BuckLog("code blob of %u bytes: sent request %03x value %08x", size, request, value);
    return 1;
}

int32_t BuckDataProcessMessage(NetClient* connection, uint32_t msgId, CDataStore* msg) {
    // The realm connection answers the code-delivery opcodes through RealmConnection, which
    // shares BuckDataHandleCustomCode below; 0x41B challenges arrive on both connections.
    bool isRealm = connection == NetClient::s_realmConnection;

    if (isRealm && (msgId == BUCK_OPCODE_INSTALL || msgId == MSG_BATTLEGROUND_PLAYER_POSITIONS
        || msgId == CMSG_UNUSED6)) {
        return 0;
    }

    uint32_t size = msg->m_size > msg->m_read ? msg->m_size - msg->m_read : 0;
    const uint8_t* data = msg->m_data + msg->m_read;

    switch (msgId) {
        case BUCK_OPCODE_INSTALL:
            return BuckDataHandleCustomCode(connection, data, size);

        case BUCK_OPCODE_CHALLENGE:
            return HandleChallenge(connection, data, size);

        case CMSG_UNUSED6: // 0x4B9: a delivered Lua script; the handler decrypts and runs it
        case BUCK_OPCODE_CLEAR: // 0x41C: data for the delivered handlers
        case BUCK_OPCODE_DATA: // 0x41D: data for the delivered handlers
            BuckLog("SMSG %03x (%u bytes) left to the client: %s", msgId, size, ToHex(data, size).c_str());
            return 1;

        default:
            return 0;
    }
}

#else

#include "WoW/Source/Net/NetClient/BuckData.h"

int32_t BuckDataProcessMessage(NetClient* connection, uint32_t msgId, CDataStore* msg) {
    (void)connection;
    (void)msgId;
    (void)msg;
    return 0;
}

int32_t BuckDataHandleCustomCode(NetClient* connection, const uint8_t* data, uint32_t size) {
    (void)connection;
    (void)data;
    (void)size;
    return 0;
}

#endif
