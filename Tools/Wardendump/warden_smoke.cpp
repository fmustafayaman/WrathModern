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
 * Modes:
 *   WardenSmoke                          built-in module lifecycle with a dummy key
 *   WardenSmoke <module.bin>             load a dumped server module image instead
 *   WardenSmoke <module.bin> <trace.log> <session-key.bin>
 *       replay a captured session exactly like the client runs it: the built-in module takes
 *       the trace's "SMSG <hex>" lines, and when it asks for the server module (ModuleUse) the
 *       image from <module.bin> is activated, picking up the built-in module's saved state.
 *       Every module output is compared with the trace's "CMSG" lines in order. The trace file
 *       collects every login; the packets after its last "SESSION" line are replayed.
 *   WardenSmoke --snapshot <warden-snapshot.bin>
 *       rerun the entry call a live session failed in, from the machine state captured just
 *       before it; the full branch trace lands in Logs/warden.log (or ./warden.log).
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

static std::string ToHex(const uint8_t* data, uint32_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;

    for (uint32_t i = 0; i < size; i++) {
        text += digits[data[i] >> 4];
        text += digits[data[i] & 0xF];
    }

    return text;
}

static bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* file = std::fopen(path, "rb");

    if (!file) {
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        std::fclose(file);
        return false;
    }

    out.resize((size_t)size);
    bool ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

class SmokeServices : public WardenEmuServices {
public:
    // The trace's CMSG lines, in order; every module output is checked against the next one.
    std::vector<std::string> expectedOutputs;
    uint32_t outputCount = 0;
    uint32_t outputMismatches = 0;

    // Set when ModuleUse can hand over a server module (a dumped image was given).
    bool canUseModule = false;
    bool moduleRequested = false;

    // OurLibrary::StateSave / StateLoad, exactly like the client's single saved-state slot.
    std::vector<uint8_t> savedState;
    bool hasSavedState = false;

    void ModuleOutput(const uint8_t* data, uint32_t size) override {
        std::string hex = ToHex(data, size);
        uint32_t index = outputCount++;

        if (index < expectedOutputs.size()) {
            bool same = expectedOutputs[index] == hex;
            outputMismatches += same ? 0 : 1;
            std::printf("MODULE OUTPUT #%u (%u bytes): %s  [%s]\n", index, size, hex.c_str(),
                same ? "matches the live session" : "DIFFERS from the live session");

            if (!same) {
                std::printf("    live session sent: %s\n", expectedOutputs[index].c_str());
            }
        } else {
            std::printf("MODULE OUTPUT #%u (%u bytes): %s\n", index, size, hex.c_str());
        }
    }

    bool ModuleUse(const uint8_t* id, const uint8_t* key) override {
        (void)key;
        std::printf("MODULE USE id=%s -> %s\n", ToHex(id, 16).c_str(),
            canUseModule ? "server module image" : "no image");
        moduleRequested = canUseModule;
        return canUseModule;
    }

    void ModuleCache(const uint8_t* id, const void* data, uint32_t size) override {
        (void)data;
        std::printf("MODULE CACHE id=%s size=%u\n", ToHex(id, 16).c_str(), size);
    }

    void StateSave(const void* data, uint32_t size) override {
        std::printf("STATE SAVE size=%u\n", size);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        savedState.assign(bytes, bytes + size);
        hasSavedState = true;
    }

    bool StateLoad(void* data, uint32_t& size) override {
        std::printf("STATE LOAD size=%u (%s)\n", size, hasSavedState ? "restored" : "nothing saved");

        if (!hasSavedState) {
            return false;
        }

        if (size > savedState.size()) {
            size = (uint32_t)savedState.size();
        }

        std::memcpy(data, savedState.data(), size);
        savedState.clear();
        hasSavedState = false;
        return true;
    }
};

// The built-in module image: RC4 with the embedded key, then zlib with the capacity prefix.
static bool BuiltinImage(std::vector<uint8_t>& image) {
    std::vector<uint8_t> blob(s_builtinModule, s_builtinModule + sizeof(s_builtinModule));
    Rc4Crypt(s_builtinModuleKey, sizeof(s_builtinModuleKey), blob.data(), static_cast<uint32_t>(blob.size()));

    uint32_t capacity = 0;
    std::memcpy(&capacity, blob.data(), sizeof(capacity));

    image.resize(capacity);
    uLongf imageSize = capacity;

    if (uncompress(image.data(), &imageSize, &blob[4], blob.size() - 4)) {
        return false;
    }

    image.resize((size_t)imageSize);
    return true;
}

static WardenEmu* StartModule(SmokeServices& services, const std::vector<uint8_t>& image,
    const uint8_t* sessionKey, const char* label) {
    WardenEmu* module = new WardenEmu(&services);

    if (!module->Load(image.data(), (uint32_t)image.size())) {
        std::printf("FAIL: %s Load\n", label);
        delete module;
        return nullptr;
    }

    if (!module->CreateModule()) {
        std::printf("FAIL: %s CreateModule\n", label);
        delete module;
        return nullptr;
    }

    if (!module->Initialize(sessionKey, 40)) {
        std::printf("FAIL: %s Initialize\n", label);
        delete module;
        return nullptr;
    }

    std::printf(".%s loaded, created and initialized\n", label);
    return module;
}

static int ReplaySnapshot(const char* path) {
    SmokeServices services;
    WardenEmu module(&services);
    bool ok = module.ReplaySnapshot(path);
    std::printf(".snapshot replay %s (see the trace lines in warden.log)\n", ok ? "completed" : "failed");
    return ok ? 0 : 2;
}

// The client flow for a captured session: built-in module first, the server module once the
// built-in one asks for it, activation after every Data/Process like ActivateNextModule.
static int ReplaySession(const char* imagePath, const char* tracePath, const char* keyPath) {
    std::vector<uint8_t> serverImage;
    std::vector<uint8_t> builtinImage;
    std::vector<uint8_t> key;

    if (!ReadFile(imagePath, serverImage)) {
        std::printf("FAIL: cannot read module image %s\n", imagePath);
        return 1;
    }

    if (!ReadFile(keyPath, key) || key.size() != 40) {
        std::printf("FAIL: session key %s must be 40 bytes\n", keyPath);
        return 1;
    }

    if (!BuiltinImage(builtinImage)) {
        std::printf("FAIL: built-in module inflate\n");
        return 1;
    }

    FILE* trace = std::fopen(tracePath, "r");

    if (!trace) {
        std::printf("FAIL: cannot open trace %s\n", tracePath);
        return 1;
    }

    std::vector<std::vector<uint8_t>> packets;
    SmokeServices services;
    services.canUseModule = true;
    char line[16384];

    while (std::fgets(line, sizeof(line), trace)) {
        // The client marks every login; only the last session goes with the captured key.
        if (std::strncmp(line, "SESSION", 7) == 0) {
            packets.clear();
            services.expectedOutputs.clear();
            continue;
        }

        bool server = std::strncmp(line, "SMSG ", 5) == 0;
        bool client = std::strncmp(line, "CMSG ", 5) == 0;

        if (!server && !client) {
            continue;
        }

        std::string hex(line + 5);

        while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
            hex.pop_back();
        }

        if (client) {
            services.expectedOutputs.push_back(hex);
            continue;
        }

        std::vector<uint8_t> packet;

        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            char byteText[3] = { hex[i], hex[i + 1], 0 };
            packet.push_back((uint8_t)std::strtoul(byteText, nullptr, 16));
        }

        packets.push_back(packet);
    }

    std::fclose(trace);
    std::printf(".trace: %u server packets, %u client packets\n", (uint32_t)packets.size(),
        (uint32_t)services.expectedOutputs.size());

    WardenEmu* active = StartModule(services, builtinImage, key.data(), "built-in module");

    if (!active) {
        return 1;
    }

    // ActivateNextModule: the old module shuts down (saving its state) before the new one is
    // created and initialized (loading it).
    auto activateNext = [&]() -> bool {
        if (!services.moduleRequested) {
            return true;
        }

        services.moduleRequested = false;
        active->Shutdown();
        delete active;
        active = StartModule(services, serverImage, key.data(), "server module");
        return active != nullptr;
    };

    for (uint32_t index = 0; index < packets.size(); index++) {
        uint32_t consumed = 0;
        active->Data(packets[index].data(), (uint32_t)packets[index].size(), consumed);
        std::printf(".packet[%u] %u bytes, consumed %u\n", index, (uint32_t)packets[index].size(), consumed);

        if (!activateNext()) {
            return 1;
        }

        // The client polls every frame between packets.
        for (uint32_t frame = 0; frame < 3; frame++) {
            active->Process(16);

            if (!activateNext()) {
                return 1;
            }
        }
    }

    active->Shutdown();
    delete active;

    std::printf(".replay done: %u outputs, %u of them differ from the live session\n", services.outputCount,
        services.outputMismatches);
    return services.outputMismatches ? 2 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--snapshot") == 0) {
        return ReplaySnapshot(argv[2]);
    }

    if (argc >= 4) {
        return ReplaySession(argv[1], argv[2], argv[3]);
    }

    std::vector<uint8_t> image;

    if (argc >= 2) {
        // A dumped server module image (Logs/warden-module-image.bin): the emulator loads it
        // directly, with no RC4 or inflate step.
        if (!ReadFile(argv[1], image)) {
            std::printf("FAIL: cannot read %s\n", argv[1]);
            return 1;
        }

        std::printf(".module file %s -> %u bytes\n", argv[1], (uint32_t)image.size());
    } else {
        if (!BuiltinImage(image)) {
            std::printf("FAIL: uncompress\n");
            return 1;
        }

        std::printf(".inflate -> %u bytes\n", (uint32_t)image.size());
    }

    // A dummy session key keeps the lifecycle running; the module only answers real traffic
    // with the real key (see the session replay mode).
    uint8_t sessionKey[40];

    for (uint32_t i = 0; i < sizeof(sessionKey); i++) {
        sessionKey[i] = static_cast<uint8_t>(i * 7 + 0x21);
    }

    SmokeServices services;
    WardenEmu* module = StartModule(services, image, sessionKey, "module");

    if (!module) {
        return 1;
    }

    // Periodic processing: the handshake usually flows out through OurLibrary::Data here
    // (or during the Data call below).
    module->Process(50);
    module->Process(100);
    std::printf(".processed\n");

    // Feed a dummy server payload so the module's Data path also runs.
    uint8_t payload[16];

    for (uint32_t i = 0; i < sizeof(payload); i++) {
        payload[i] = static_cast<uint8_t>(0xA0 + i);
    }

    uint32_t consumed = 0;
    module->Data(payload, sizeof(payload), consumed);
    std::printf(".data delivered, consumed=%u\n", consumed);

    // Poll a few more frames.
    for (uint32_t ms = 0; ms < 6; ms++) {
        module->Process(500);
    }

    std::printf(".done\n");

    module->Shutdown();
    delete module;
    std::printf(".shutdown ok\n");
    return 0;
}
