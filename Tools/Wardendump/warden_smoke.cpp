/*
 * Standalone smoke test for the emulated Warden loader (Tools diag, not part of the game).
 *
 * It runs the same pipeline as the client's LoadModule without the RSA signature check
 * (the signature material and its verification are part of the shipped client, not needed
 * to exercise the emulator): RC4-decrypt the built-in module, zlib-inflate the image and
 * feed it to WardenEmu, then drive the module lifecycle with a dummy session key.
 *
 * Expected output: the module's handshake generation calls back through OurLibrary::Data
 * with bytes, which this test prints as a hex dump. Those bytes are what the real client
 * would put into CMSG_WARDEN_DATA.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <zlib.h>

#include "Engine/Source/Os/WardenEmu.h"
#include "WoW/Source/Game/GameClient/WardenModuleData.h"

static void Rc4Crypt(const uint8_t* key, uint32_t keyLength, uint8_t* buffer, uint32_t size) {
    uint8_t state[256];

    for (uint32_t i = 0; i < 256; i++) {
        state[i] = static_cast<uint8_t>(i);
    }

    uint32_t j = 0;

    for (uint32_t i = 0; i < 256; i++) {
        j = (j + state[i] + key[i % keyLength]) & 0xFF;
        uint8_t t = state[i];
        state[i] = state[j];
        state[j] = t;
    }

    uint32_t a = 0;
    j = 0;

    for (uint32_t i = 0; i < size; i++) {
        a = (a + 1) & 0xFF;
        j = (j + state[a]) & 0xFF;
        uint8_t t = state[a];
        state[a] = state[j];
        state[j] = t;
        buffer[i] ^= state[(state[a] + state[j]) & 0xFF];
    }
}

class SmokeServices : public WardenEmuServices {
public:
    void ModuleOutput(const uint8_t* data, uint32_t size) override {
        std::printf("MODULE OUTPUT (%u bytes): ", size);

        for (uint32_t i = 0; i < size; i++) {
            std::printf("%02x ", data[i]);
        }

        std::printf("\n");
    }

    bool ModuleUse(const uint8_t* id, const uint8_t* key) override {
        std::printf("MODULE USE id=");
        for (int i = 0; i < 16; i++) {
            std::printf("%02x", id[i]);
        }
        std::printf("\n");
        return false;
    }

    void ModuleCache(const uint8_t* id, const void* data, uint32_t size) override {
        std::printf("MODULE CACHE size=%u\n", size);
    }

    void StateSave(const void* data, uint32_t size) override {
        std::printf("STATE SAVE size=%u\n", size);
    }

    bool StateLoad(void* data, uint32_t& size) override {
        std::printf("STATE LOAD size=%u\n", size);
        return false;
    }
};

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // 1. Decrypt the built-in module blob.
    std::vector<uint8_t> blob(s_builtinModule, s_builtinModule + sizeof(s_builtinModule));
    Rc4Crypt(s_builtinModuleKey, sizeof(s_builtinModuleKey), blob.data(),
        static_cast<uint32_t>(blob.size()));

    // 2. Inflate. The first four bytes hold the image capacity.
    uint32_t capacity = 0;
    std::memcpy(&capacity, blob.data(), sizeof(capacity));

    std::vector<uint8_t> image(capacity);
    uLongf imageSize = capacity;

    if (uncompress(image.data(), &imageSize, &blob[4], blob.size() - 4)) {
        std::printf("FAIL: uncompress\n");
        return 1;
    }

    std::printf(".inflate -> %u bytes (capacity %u)\n", (uint32_t)imageSize, capacity);

    // 3. Load into the emulator.
    SmokeServices services;
    WardenEmu module(&services);

    if (!module.Load(image.data(), (uint32_t)imageSize)) {
        std::printf("FAIL: Load\n");
        return 1;
    }

    std::printf(".load ok\n");

    // 4. Create the module object through export 1.
    if (!module.CreateModule()) {
        std::printf("FAIL: CreateModule\n");
        return 1;
    }

    std::printf(".create ok\n");

    // 5. Initialize with a dummy session key (40 bytes, like the login packet).
    uint8_t sessionKey[40];
    for (uint32_t i = 0; i < sizeof(sessionKey); i++) {
        sessionKey[i] = static_cast<uint8_t>(i * 7 + 0x21);
    }

    if (!module.Initialize(sessionKey, sizeof(sessionKey))) {
        std::printf("FAIL: Initialize\n");
        return 1;
    }

    std::printf(".initialize ok\n");

    // 6. Periodic processing: the handshake usually flows out through OurLibrary::Data here
    // (or during the Data call below).
    module.Process(50);
    module.Process(100);
    std::printf(".processed\n");

    // 7. Feed a dummy server payload so the module's Data path also runs.
    uint8_t payload[16];
    for (uint32_t i = 0; i < sizeof(payload); i++) {
        payload[i] = static_cast<uint8_t>(0xA0 + i);
    }

    uint32_t consumed = 0;
    module.Data(payload, sizeof(payload), consumed);
    std::printf(".data delivered, consumed=%u\n", consumed);

    // 8. Poll a few more frames.
    for (uint32_t ms = 0; ms < 6; ms++) {
        module.Process(500);
    }
    std::printf(".done\n");

    module.Shutdown();
    std::printf(".shutdown ok\n");
    return 0;
}
