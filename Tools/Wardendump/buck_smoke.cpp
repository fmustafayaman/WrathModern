/*
 * Standalone smoke test for the server-delivered x86 code runner (BuckCodeEmu), a Tools diag,
 * not part of the game.
 *
 * It takes one held-back Warmane challenge exactly as the realm sends it (SMSG 0x41B minus the
 * opcode), the token this client reported in its CMSG 0x41B request, and runs the delivered
 * code the way the official client's handler does: decode, run with the message in ESI and a
 * reply store in EDI, then pad and encode the reply. What it prints is the CMSG 0x41B payload
 * this client would send.
 *
 *   BuckSmoke <challenge.bin> <token-hex> [Wow.exe]
 *
 * WARDEN_CLIENT_EXE names the client executable when not given as the third argument. The
 * captured challenges under Tools/Wardendump/captures/ exercise it:
 *   BuckSmoke Tools/Wardendump/captures/challenge-40-a.bin 46c7000
 *   BuckSmoke Tools/Wardendump/captures/challenge-40-b.bin 46d6000
 * Both are the simple "answer with the message size" check; the reply must be 12 bytes whose
 * last dword decodes back to the size.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Source/Os/Mac/BuckCodeEmu.h"
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "WoW/Source/Net/NetClient/BuckDatabases.h"
#include "WoW/Source/DB/DBClient/DBClient.h"

namespace {

bool InitializeGameFiles() {
    static bool ready = false;

    if (ready) return true;
    const char* exe = getenv("WARDEN_CLIENT_EXE");
    if (!exe || !exe[0]) return false;
    namespace fs = std::filesystem;
    fs::path directory = fs::absolute(exe).parent_path();
    fs::path previous = fs::current_path();
    fs::current_path(directory);
    StormInitialize();
    int32_t priority = 1;
    const char* archives[] = { "Data/common.MPQ", "Data/common-2.MPQ", "Data/expansion.MPQ",
        "Data/lichking.MPQ", "Data/enUS/locale-enUS.MPQ", "Data/enUS/expansion-locale-enUS.MPQ",
        "Data/enUS/lichking-locale-enUS.MPQ", "Data/patch.MPQ", "Data/patch-2.MPQ", "Data/patch-3.MPQ",
        "Data/enUS/patch-enUS.MPQ", "Data/enUS/patch-enUS-2.MPQ", "Data/enUS/patch-enUS-3.MPQ" };

    for (const char* name : archives) {
        if (!fs::exists(name)) continue;
        SArchive* archive = nullptr;

        if (!SFile::OpenArchive((directory / name).c_str(), priority++, 0xC00, &archive)) {
            fs::current_path(previous);
            return false;
        }
    }

    fs::current_path(previous);
    ready = true;
    return true;
}

class SmokeServices : public BuckCodeServices {
public:
    bool ClientImage(const uint8_t*& data, uint32_t& size, uint32_t& base) override {
        static std::vector<uint8_t> s_image;
        static uint32_t s_base = 0;

        if (!s_image.empty()) {
            data = s_image.data();
            size = static_cast<uint32_t>(s_image.size());
            base = s_base;
            return true;
        }

        const char* path = getenv("WARDEN_CLIENT_EXE");

        if (!path || !path[0]) {
            return false;
        }

        std::vector<uint8_t> image;
        uint32_t preferredBase = 0;

        if (!OsMacMapPeImage(path, image, preferredBase)) {
            fprintf(stderr, "client image: %s could not be mapped\n", path);
            return false;
        }

        s_image = std::move(image);
        s_base = preferredBase;
        printf("client image: %s mapped (%u bytes at %08x)\n", path, static_cast<uint32_t>(s_image.size()), s_base);
        data = s_image.data();
        size = static_cast<uint32_t>(s_image.size());
        base = s_base;
        return true;
    }

    bool ClientDatabase(uint32_t address, BuckDatabase& database) override {
        return InitializeGameFiles() && BuckReadNativeDatabase(address, database);
    }

    bool CommitClientDatabase(uint32_t address, const BuckDatabase& database) override {
        return BuckCommitNativeDatabase(address, database);
    }

    int32_t IsTrial() override {
        return SFile::IsTrial();
    }

    void Log(const char* line) override {
        printf("%s\n", line);
        fflush(stdout);
    }

    void RunLua(const char* source, const char* filename, const char* taint) override {
        (void)filename;
        (void)taint;
        printf("lua: %.200s\n", source ? source : "");
        fflush(stdout);
    }

    const char* ClientPath() override {
        static std::string s_path;

        if (s_path.empty()) {
            const char* path = getenv("WARDEN_CLIENT_EXE");

            if (path && path[0]) {
                s_path = "Z:";

                for (const char* c = path; *c; c++) {
                    s_path += (*c == '/') ? '\\' : *c;
                }
            }
        }

        return s_path.c_str();
    }
};

bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* file = fopen(path, "rb");

    if (!file) {
        return false;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        return false;
    }

    out.resize(static_cast<size_t>(size));
    bool ok = fread(out.data(), 1, out.size(), file) == out.size();
    fclose(file);
    return ok;
}

void PrintHex(const char* label, const std::vector<uint8_t>& bytes) {
    printf("%s (%u):", label, static_cast<uint32_t>(bytes.size()));

    for (uint8_t byte : bytes) {
        printf(" %02x", byte);
    }

    printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && strcmp(argv[1], "--stream") == 0) {
        // Each fixture is [uint16 opcode][payload]. Keep one machine for the complete
        // install/data/challenge stream, exactly as the live client must.
        setenv("WARDEN_CLIENT_EXE", argv[2], 1);
        SmokeServices services;
        BuckCodeEmu emulator(&services);

        for (int i = 3; i < argc; i++) {
            uint32_t opcode = 0xFFFF;
            std::vector<std::vector<uint8_t>> sent;
            bool ran = false;

            if (strcmp(argv[i], "--initialize-game") == 0) {
                uint32_t result = 0;
                ran = emulator.RunHookedCall(BuckCodeEmu::CALL_CLIENT_INITIALIZE_GAME, nullptr, 0, 0, result);
            } else if (strncmp(argv[i], "--patch-script=", 15) == 0) {
                const char* filename = argv[i] + 15;
                void* buffer = nullptr;
                uint32_t bytes = 0;

                if (!InitializeGameFiles() || !SFile::ReadFile(nullptr, filename, &buffer, &bytes, 0, 1, nullptr)) {
                    fprintf(stderr, "script file could not be read: %s\n", filename);
                    return 2;
                }

                std::vector<uint8_t> original(static_cast<uint8_t*>(buffer), static_cast<uint8_t*>(buffer) + bytes);
                std::string chunkname = std::string("@") + filename;
                opcode = 0xFFFE;
                ran = emulator.PatchScriptBuffer(buffer, bytes, chunkname.c_str());
                printf("patched script '%s': changed=%u\n", filename,
                    memcmp(original.data(), buffer, bytes) != 0 ? 1 : 0);
                SFile::Unload(buffer);
            } else {
                std::vector<uint8_t> packet;

                if (!ReadFile(argv[i], packet) || packet.size() < 2) {
                    fprintf(stderr, "stream packet could not be read: %s\n", argv[i]);
                    return 2;
                }

                opcode = packet[0] | (static_cast<uint32_t>(packet[1]) << 8);
                const uint8_t* payload = packet.data() + 2;
                uint32_t size = static_cast<uint32_t>(packet.size() - 2);
                ran = opcode == 0x2E9 ? emulator.InstallFramework(payload, size)
                    : emulator.Dispatch(opcode, 0, payload, size, sent);
            }

            if (!ran) {
                fprintf(stderr, "stream opcode %04x failed\n", opcode);
                return 1;
            }

            printf("stream opcode %04x: %u sent\n", opcode, static_cast<uint32_t>(sent.size()));

            for (const auto& response : sent) {
                PrintHex("sent", response);
            }

            uint32_t state[10] = {};

            if (!emulator.ReadFrameworkState(state)) {
                return 1;
            }

            printf("framework state:");
            for (uint32_t value : state) printf(" %08x", value);
            printf("\n");

            if (opcode == 0xFFFF) {
                MapRec* map = g_mapDB.GetRecord(37);
                SpellRec spell;

                if (!map || !g_spellDB.GetRecord(300057, &spell)) {
                    fprintf(stderr, "delivered native map/spell records are missing\n");
                    return 1;
                }

                printf("native content: map %d directory '%s', spell %d\n", map->m_id, map->m_directory, spell.m_id);
            }
        }

        return 0;
    }

    if (argc >= 6 && strcmp(argv[1], "--data") == 0) {
        std::vector<uint8_t> handler;
        std::vector<uint8_t> payload;

        if (!ReadFile(argv[2], handler) || !ReadFile(argv[3], payload)) {
            fprintf(stderr, "handler or payload could not be read\n");
            return 2;
        }

        if (argc >= 6) {
            setenv("WARDEN_CLIENT_EXE", argv[5], 1);
        }

        SmokeServices services;
        BuckCodeEmu emulator(&services);
        uint32_t token = static_cast<uint32_t>(strtoul(argv[4], nullptr, 16));
        uint32_t hookOffset = argc >= 7 ? static_cast<uint32_t>(strtoul(argv[6], nullptr, 16)) : 0;

        if (!emulator.RunDataHandler(handler.data(), static_cast<uint32_t>(handler.size()),
                payload.data(), static_cast<uint32_t>(payload.size()), token, hookOffset)) {
            fprintf(stderr, "data handler failed\n");
            return 1;
        }

        return 0;
    }

    if (argc >= 7 && strcmp(argv[1], "--seq") == 0) {
        std::vector<uint8_t> handler;
        std::vector<uint8_t> first;
        std::vector<uint8_t> second;

        if (!ReadFile(argv[2], handler) || !ReadFile(argv[3], first) || !ReadFile(argv[4], second)) {
            fprintf(stderr, "handler or challenges could not be read\n");
            return 2;
        }

        setenv("WARDEN_CLIENT_EXE", argv[6], 1);

        SmokeServices services;
        BuckCodeEmu emulator(&services);
        uint32_t token = static_cast<uint32_t>(strtoul(argv[5], nullptr, 16));
        uint32_t key = token;
        std::vector<uint8_t> response;
        uint32_t nextKey = 0;

        if (!emulator.RunHandlerChallenge(handler.data(), static_cast<uint32_t>(handler.size()),
                first.data(), static_cast<uint32_t>(first.size()), token, key, response, nextKey)) {
            fprintf(stderr, "first challenge failed\n");
            return 1;
        }

        PrintHex("first response", response);
        printf("first next key: %08x\n", nextKey);
        key = nextKey;

        if (!emulator.RunHandlerChallenge(handler.data(), static_cast<uint32_t>(handler.size()),
                second.data(), static_cast<uint32_t>(second.size()), token, key, response, nextKey)) {
            fprintf(stderr, "second challenge failed\n");
            return 1;
        }

        PrintHex("second response", response);
        printf("second next key: %08x\n", nextKey);
        return 0;
    }

    if (argc >= 5 && strcmp(argv[1], "--handler") == 0) {
        std::vector<uint8_t> handler;
        std::vector<uint8_t> challenge;

        if (!ReadFile(argv[2], handler) || !ReadFile(argv[3], challenge)) {
            fprintf(stderr, "handler or challenge could not be read\n");
            return 2;
        }

        if (argc >= 6) {
            setenv("WARDEN_CLIENT_EXE", argv[5], 1);
        }

        SmokeServices services;
        BuckCodeEmu emulator(&services);
        uint32_t token = static_cast<uint32_t>(strtoul(argv[4], nullptr, 16));
        uint32_t key = argc >= 7 ? static_cast<uint32_t>(strtoul(argv[6], nullptr, 16)) : token;
        std::vector<uint8_t> response;
        uint32_t nextKey = 0;

        if (!emulator.RunHandlerChallenge(handler.data(), static_cast<uint32_t>(handler.size()),
                challenge.data(), static_cast<uint32_t>(challenge.size()), token, key, response, nextKey)) {
            fprintf(stderr, "challenge handler failed\n");
            return 1;
        }

        PrintHex("response", response);
        printf("next key: %08x\n", nextKey);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--lua") == 0) {
        std::vector<uint8_t> blob;

        if (!ReadFile(argv[2], blob)) {
            fprintf(stderr, "script: %s could not be read\n", argv[2]);
            return 2;
        }

        std::vector<std::string> scripts;

        if (!BuckCodeEmu::DecodeLuaScripts(blob.data(), static_cast<uint32_t>(blob.size()), scripts)) {
            fprintf(stderr, "Lua payload is missing a seed or has an unterminated chunk\n");
            return 1;
        }

        printf("scripts (%u):\n", static_cast<uint32_t>(scripts.size()));

        for (uint32_t i = 0; i < scripts.size(); i++) {
            printf("script %u (%u bytes):\n%s\n", i + 1, static_cast<uint32_t>(scripts[i].size()),
                scripts[i].c_str());
        }
        return 0;
    }

    if (argc < 3) {
        fprintf(stderr, "usage: %s <challenge.bin> <token-hex> [Wow.exe]\n", argv[0]);
        fprintf(stderr, "       %s --handler <handler.bin> <challenge.bin> <token-hex> [Wow.exe] [key-hex]\n", argv[0]);
        fprintf(stderr, "       %s --lua <4b9-payload.bin>\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> challenge;

    if (!ReadFile(argv[1], challenge)) {
        fprintf(stderr, "challenge: %s could not be read\n", argv[1]);
        return 2;
    }

    if (argc >= 4) {
        setenv("WARDEN_CLIENT_EXE", argv[3], 1);
    }

    uint32_t token = static_cast<uint32_t>(strtoul(argv[2], nullptr, 16));
    SmokeServices services;
    BuckCodeEmu emulator(&services);

    std::vector<uint8_t> decoded;

    if (!BuckCodeEmu::DecodeChallenge(challenge.data(), static_cast<uint32_t>(challenge.size()), token, decoded)) {
        fprintf(stderr, "challenge does not decode\n");
        return 1;
    }

    PrintHex("decoded", decoded);

    uint32_t codeLength = 0;
    memcpy(&codeLength, decoded.data() + 4, sizeof(codeLength));
    printf("code %u bytes\n", codeLength);

    std::vector<uint8_t> response;
    uint32_t nextKey = 0;

    if (!emulator.RunChallenge(challenge.data(), static_cast<uint32_t>(challenge.size()), token, token,
            response, nextKey)) {
        fprintf(stderr, "challenge code failed\n");
        return 1;
    }

    PrintHex("response", response);
    printf("next key: %08x\n", nextKey);
    return 0;
}
