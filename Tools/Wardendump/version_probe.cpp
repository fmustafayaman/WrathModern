/*
 * Component-by-component probe for the version-hash pipeline: for each entry the login
 * proof uses, print whether it hashed and the digest it contributed (Tools diag).
 * The working directory changes to the client directory first, matching the game.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Os/OsFile.h"
#include "WoW/Source/WowServices/HMAC.h"
#include "Platform/Mac/Paths.h"

#include "WoW/Source/WowSvcs/WowSvcsClient/ChecksumExecutables.h"

static void PrintDigest(const char* label, SHA1::Digest& digest) {
    std::printf("%-16s ", label);
    for (uint32_t i = 0; i < sizeof(digest); i++) {
        std::printf("%02x ", reinterpret_cast<uint8_t*>(&digest)[i]);
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    const char* directory = argc > 1 ? argv[1] : ".";

    if (!OsSetCurrentDirectory(directory)) {
        std::printf("FAIL: chdir %s\n", directory);
        return 1;
    }

    SHA1::Digest digest;

    // Each file hashed alone with a plain SHA1 stream, exactly what the proof's HMAC wraps.
    struct Entry { const char* label; const char* path; int32_t loaded; };

    Entry entries[4] = {
        { "Wow.exe", nullptr, 1 },
        { "DivxDecoder.dll", ".\\DivxDecoder.dll", 1 },
        { "unicows.dll", ".\\unicows.dll", 0 },
    };

    for (uint32_t i = 0; i < 3; i++) {
        SHA1 sha;
        sha.Prepare();

        IOsVersionHashCallback2<SHA1> callback(&sha);
        IOsVersionChecksum(callback, entries[i].path, entries[i].loaded);
        sha.Finish(digest);
        PrintDigest(entries[i].label, digest);
    }

    // Where is Wow.exe and what does the client directory look like?
    char path[4096];
    bool found = MacPaths::FindInClientDirectory("Wow.exe", path, sizeof(path));
    std::printf("find Wow.exe: %d -> %s\n", found, found ? path : "(probe binary is not a bundle; expected)");

    // The full proof, like ChecksumExecutables computes it.
    uint8_t challenge[16];

    for (uint32_t i = 0; i < sizeof(challenge); i++) {
        challenge[i] = static_cast<uint8_t>(0x10 + i);
    }

    uint8_t proof[20];
    memset(proof, 0xCC, sizeof(proof));
    ChecksumExecutables(challenge, sizeof(challenge), proof);

    std::printf("proof: ");

    for (uint32_t i = 0; i < sizeof(proof); i++) {
        std::printf("%02x ", proof[i]);
    }

    std::printf("\n");
    return 0;
}
