#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "WoW/Source/Net/NetClient/BuckData.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/FrameScript/FrameScript.h"
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

// The most recent token reported on any connection. The world server can challenge before it
// re-delivers the code on the world connection; the token the process reported to the realm is
// the one such a challenge would be encoded with.
uint32_t s_lastBuckToken;

// The key the framework currently holds for the 0x41B handler. The install code registers the
// handler with the token; the handler itself re-registers with the size of the message it just
// consumed, and the framework passes that param to the next dispatch. Carrying it forward keeps
// the decode correct whichever of the two the server used for a given challenge.
uint32_t s_challengeKey;

// The 0x41B handler segment delivered by the code blob. The official client installs this at the
// token address and its message dispatch calls it for every challenge; running it (rather than
// reconstructing the framing) is what produces the reply on the wire.
std::vector<uint8_t> s_challengeHandler;

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

    void RunLua(const char* source, const char* filename, const char* taint) override {
        BuckLog("FrameScript::Execute from delivered code: %.200s", source ? source : "");
        FrameScript_Execute(source, filename, taint);
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

// A code blob is [uint32 length][code] repeated. The first segment is the install code, the last
// is the handler it registers; the official framework hands each segment to the client as a
// CDataStore. For 0x41B the handler is what decodes and answers the challenges.
bool ExtractChallengeHandler(const uint8_t* data, uint32_t size, std::vector<uint8_t>& handler) {
    const uint8_t* last = nullptr;
    uint32_t lastSize = 0;
    uint32_t position = 0;

    while (position + 4 <= size) {
        uint32_t length = static_cast<uint32_t>(data[position]) | (static_cast<uint32_t>(data[position + 1]) << 8)
            | (static_cast<uint32_t>(data[position + 2]) << 16) | (static_cast<uint32_t>(data[position + 3]) << 24);
        position += 4;

        if (length > size - position) {
            break;
        }

        last = data + position;
        lastSize = length;
        position += length;
    }

    if (!last || !lastSize) {
        return false;
    }

    handler.assign(last, last + lastSize);
    return true;
}

int32_t HandleLuaScript(const uint8_t* data, uint32_t size) {
    // The delivered 0x4B9 handler first reads one byte with CDataStore::GetByte: that byte is
    // the seed for the rolling XOR, not part of the script. It then decodes the rest of the
    // message in place, each byte offset by a key that advances by the decoded byte plus one,
    // and hands the NUL-terminated result to luaL_loadbuffer/lua_pcall.
    if (size < 2) {
        return 1;
    }

    std::string script;
    uint8_t key = data[0];

    for (uint32_t i = 1; i < size; i++) {
        uint8_t decoded = data[i] ^ key;
        key = static_cast<uint8_t>(key + decoded + 1);

        if (!decoded) {
            break;
        }

        script += static_cast<char>(decoded);
    }

    BuckLog("delivered Lua script (%u bytes, seed %02x): %.200s", static_cast<uint32_t>(script.size()),
        data[0], script.c_str());

    if (!script.empty()) {
        FrameScript_Execute(script.c_str(), nullptr, nullptr);
    }

    return 1;
}

bool LooksLikeDeliveredCode(uint8_t byte) {
    switch (byte) {
        case 0x55: // push ebp
        case 0x60: // pushal
        case 0x53: // push ebx
        case 0x56: // push esi
        case 0x57: // push edi
        case 0x8B: // mov
        case 0xB8: // mov eax, imm32
        case 0x68: // push imm32
        case 0xE8: // call rel32
        case 0x31: // xor
        case 0x83: // sub/add esp, imm8
        case 0x6A: // push imm8
        case 0x50: // push eax
            return true;
        default:
            return false;
    }
}

// Picks the key a challenge was encoded with: the framework's current param, the install token,
// or the size the handler registers after consuming a message. A candidate must decode to a
// sane header (code length inside the payload) and to plausible code that returns.
uint32_t ChooseChallengeKey(const uint8_t* data, uint32_t size) {
    uint32_t candidates[4] = { s_challengeKey, s_lastBuckToken, size >= 8 ? size - 8 : 0, size };
    std::vector<uint8_t> decoded;

    for (uint32_t candidate : candidates) {
        if (!candidate || !BuckCodeEmu::DecodeChallenge(data, size, candidate, decoded)) {
            continue;
        }

        if (decoded.size() < 9) {
            continue;
        }

        uint32_t codeLength = 0;
        memcpy(&codeLength, decoded.data() + 4, sizeof(codeLength));

        if (!codeLength || codeLength > size - 8 || !LooksLikeDeliveredCode(decoded[8])) {
            continue;
        }

        bool returns = false;

        for (uint32_t i = 8; i < 8 + codeLength && i < decoded.size(); i++) {
            if (decoded[i] == 0xC3) {
                returns = true;
                break;
            }
        }

        if (returns) {
            return candidate;
        }
    }

    return s_challengeKey ? s_challengeKey : s_lastBuckToken;
}

int32_t HandleChallenge(NetClient* connection, const uint8_t* data, uint32_t size) {
    std::vector<uint8_t> response;
    uint32_t token = s_lastBuckToken ? s_lastBuckToken : connection->m_buckToken;
    uint32_t key = ChooseChallengeKey(data, size);

    // Run the server's own handler when the code blob delivered one; it decodes the payload,
    // runs the delivered code and sends the reply exactly as the official client would.
    bool answered = false;
    uint32_t nextKey = 0;

    if (!s_challengeHandler.empty()) {
        answered = Emulator().RunHandlerChallenge(s_challengeHandler.data(),
            static_cast<uint32_t>(s_challengeHandler.size()), data, size, token, key, response, nextKey);
    }

    if (!answered) {
        answered = Emulator().RunChallenge(data, size, token, key, response, nextKey);
    }

    if (!answered) {
        BuckLog("challenge of %u bytes with token %08x key %08x could not be answered", size, token, key);
        return 1;
    }

    if (nextKey) {
        s_challengeKey = nextKey;
    }

    BuckLog("challenge %u bytes, token %08x, key %08x, next key %08x, reply %u bytes: %s", size, token, key,
        nextKey, static_cast<uint32_t>(response.size()), ToHex(response.data(), static_cast<uint32_t>(response.size())).c_str());

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
        // challenges with it. The delivered install code keeps the buffer in a client global and
        // reuses it when the server delivers the blob again, so the token is stable for the
        // process and shared by both connections. Generate it once and remember it.
        if (!s_lastBuckToken) {
            s_lastBuckToken = 0x04000000u | (OsGetAsyncTimeMs() & 0x00FFF000u);
        }

        value = s_lastBuckToken;
        connection->m_buckToken = value;
        s_challengeKey = value;

        std::vector<uint8_t> handler;

        if (ExtractChallengeHandler(data, size, handler)) {
            s_challengeHandler = handler;
            BuckLog("installed the delivered 0x41B handler (%u bytes) at token %08x",
                static_cast<uint32_t>(handler.size()), value);
        }
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

    if (isRealm && (msgId == BUCK_OPCODE_INSTALL || msgId == MSG_BATTLEGROUND_PLAYER_POSITIONS)) {
        return 0;
    }

    uint32_t size = msg->m_size > msg->m_read ? msg->m_size - msg->m_read : 0;
    const uint8_t* data = msg->m_data + msg->m_read;
    int32_t result = 0;

    switch (msgId) {
        case BUCK_OPCODE_INSTALL:
            result = BuckDataHandleCustomCode(connection, data, size);
            break;

        case BUCK_OPCODE_CHALLENGE:
            result = HandleChallenge(connection, data, size);
            break;

        case CMSG_UNUSED6: // 0x4B9: a delivered Lua script, decoded and run like the handler does
            result = HandleLuaScript(data, size);
            break;

        case BUCK_OPCODE_CLEAR: // 0x41C: data for the delivered handlers
        case BUCK_OPCODE_DATA: // 0x41D: data for the delivered handlers
            BuckLog("SMSG %03x (%u bytes) left to the client: %s", msgId, size, ToHex(data, size).c_str());
            result = 1;
            break;

        default:
            return 0;
    }

    // What these handlers read, they read whole; mark the store consumed for the caller.
    msg->m_read = msg->m_size;
    return result;
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
