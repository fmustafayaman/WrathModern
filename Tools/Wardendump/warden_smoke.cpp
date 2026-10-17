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
 *
 * Two optional arguments:
 *   WardenSmoke <module.bin>            load a dumped server module instead of the built-in one
 *   WardenSmoke <module.bin> <trace.log>  replay the trace's "SMSG <hex>" lines through Data()
 *   WardenSmoke <module.bin> <trace.log> <session-key.bin>  also use the captured session key
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    std::vector<uint8_t> image;

    if (argc >= 2) {
        // A dumped server module: the blob MacWardenServices::ModuleCache writes is the BLL
        // image the emulator loads directly, with no RC4 or inflate step.
        FILE* file = std::fopen(argv[1], "rb");

        if (!file) {
            std::printf("FAIL: cannot open %s\n", argv[1]);
            return 1;
        }

        std::fseek(file, 0, SEEK_END);
        long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);

        if (size <= 0) {
            std::fclose(file);
            std::printf("FAIL: %s is empty\n", argv[1]);
            return 1;
        }

        image.resize((size_t)size);

        if (std::fread(image.data(), 1, image.size(), file) != image.size()) {
            std::fclose(file);
            std::printf("FAIL: short read on %s\n", argv[1]);
            return 1;
        }

        std::fclose(file);
        std::printf(".module file %s -> %u bytes\n", argv[1], (uint32_t)image.size());
    } else {
        // 1. Decrypt the built-in module blob.
        std::vector<uint8_t> blob(s_builtinModule, s_builtinModule + sizeof(s_builtinModule));
        Rc4Crypt(s_builtinModuleKey, sizeof(s_builtinModuleKey), blob.data(),
            static_cast<uint32_t>(blob.size()));

        // 2. Inflate. The first four bytes hold the image capacity.
        uint32_t capacity = 0;
        std::memcpy(&capacity, blob.data(), sizeof(capacity));

        image.resize(capacity);
        uLongf imageSize = capacity;

        if (uncompress(image.data(), &imageSize, &blob[4], blob.size() - 4)) {
            std::printf("FAIL: uncompress\n");
            return 1;
        }

        image.resize((size_t)imageSize);
        std::printf(".inflate -> %u bytes (capacity %u)\n", (uint32_t)imageSize, capacity);
    }

    // 3. Load into the emulator.
    SmokeServices services;
    WardenEmu module(&services);

    if (!module.Load(image.data(), (uint32_t)image.size())) {
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

    // 5. Initialize with the session key. The module RC4-decrypts every server packet with this
    // key, so a replayed trace only makes progress with the real one: pass it as the third
    // argument (Logs/warden-session-key.bin). Without it a dummy key keeps the lifecycle running.
    uint8_t sessionKey[40];

    if (argc >= 4) {
        FILE* keyFile = std::fopen(argv[3], "rb");

        if (!keyFile) {
            std::printf("FAIL: cannot open session key %s\n", argv[3]);
            return 1;
        }

        size_t read = std::fread(sessionKey, 1, sizeof(sessionKey), keyFile);
        std::fclose(keyFile);

        if (read != sizeof(sessionKey)) {
            std::printf("FAIL: session key is %u bytes, expected %u\n", (uint32_t)read,
                (uint32_t)sizeof(sessionKey));
            return 1;
        }

        std::printf(".session key %s\n", argv[3]);
    } else {
        for (uint32_t i = 0; i < sizeof(sessionKey); i++) {
            sessionKey[i] = static_cast<uint8_t>(i * 7 + 0x21);
        }
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

    if (argc >= 3) {
        // Replay a captured conversation: every "SMSG <hex>" line goes through Data() in order,
        // exactly as the client delivered it. The MODULE OUTPUT lines this prints are the
        // module's real replies, comparable with the trace's "CMSG" lines.
        FILE* trace = std::fopen(argv[2], "r");

        if (!trace) {
            std::printf("FAIL: cannot open trace %s\n", argv[2]);
            return 1;
        }

        char line[8192];
        uint32_t index = 0;

        while (std::fgets(line, sizeof(line), trace)) {
            if (std::strncmp(line, "SMSG ", 5) != 0) {
                continue;
            }

            const char* hex = line + 5;
            std::vector<uint8_t> packet;
            size_t length = std::strlen(hex);

            for (size_t i = 0; i + 1 < length; i += 2) {
                if (hex[i] == '\n' || hex[i] == '\r') {
                    break;
                }

                char byteText[3] = { hex[i], hex[i + 1], 0 };
                packet.push_back((uint8_t)std::strtoul(byteText, nullptr, 16));
            }

            std::printf(".replay[%u] %u bytes\n", index++, (uint32_t)packet.size());

            uint32_t replayConsumed = 0;
            module.Data(packet.data(), (uint32_t)packet.size(), replayConsumed);
            std::printf(".replay[%u] consumed=%u\n", index - 1, replayConsumed);

            for (uint32_t ms = 0; ms < 3; ms++) {
                module.Process(500);
            }
        }

        std::fclose(trace);
        std::printf(".replay done (%u packets)\n", index);

        module.Shutdown();
        std::printf(".shutdown ok\n");
        return 0;
    }

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
