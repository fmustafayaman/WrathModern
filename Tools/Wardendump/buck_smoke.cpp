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
#include <string>
#include <vector>

#include "Engine/Source/Os/Mac/BuckCodeEmu.h"
#include "Engine/Source/Os/Mac/MacOs.h"

namespace {

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

    void Log(const char* line) override {
        printf("%s\n", line);
        fflush(stdout);
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

    if (size <= 0) {
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
    if (argc < 3) {
        fprintf(stderr, "usage: %s <challenge.bin> <token-hex> [Wow.exe]\n", argv[0]);
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

    if (!emulator.RunChallenge(challenge.data(), static_cast<uint32_t>(challenge.size()), token, response)) {
        fprintf(stderr, "challenge code failed\n");
        return 1;
    }

    PrintHex("response", response);
    return 0;
}
