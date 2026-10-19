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
 *
 * WARDEN_REPLAY_COLD=1 replays a login whose client downloaded the module (a cache miss in
 * the live log): the module comes from the trace's transfer instead of <module.bin>.
 *
 * WARDEN_CLIENT_EXE=<path to Wow.exe> gives the module the client executable, as the client
 * does from its own directory; replays need it to match a live session.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include <zlib.h>

#include "Engine/Source/Os/WardenEmu.h"
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
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

    // Cold-cache replays: the module arrives through the trace (ModuleCache) and ModuleUse loads
    // it like the client's LoadModule does (RC4 with the given key, then inflate).
    std::string cachedId;
    std::vector<uint8_t> cachedBlob;
    std::vector<uint8_t> downloadedImage;

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
        if (!cachedBlob.empty() && cachedId == ToHex(id, 16) && cachedBlob.size() > 4 + 260) {
            std::vector<uint8_t> blob = cachedBlob;
            Rc4Crypt(key, 16, blob.data(), (uint32_t)blob.size());

            uint32_t capacity = 0;
            std::memcpy(&capacity, blob.data(), sizeof(capacity));
            downloadedImage.resize(capacity);
            uLongf imageSize = capacity;

            if (uncompress(downloadedImage.data(), &imageSize, &blob[4], blob.size() - 4) != Z_OK) {
                std::printf("MODULE USE id=%s -> downloaded module does not inflate\n", cachedId.c_str());
                downloadedImage.clear();
                return false;
            }

            downloadedImage.resize((size_t)imageSize);
            std::printf("MODULE USE id=%s -> downloaded module (%u bytes image)\n", cachedId.c_str(),
                (uint32_t)imageSize);
            moduleRequested = true;
            return true;
        }

        std::printf("MODULE USE id=%s -> %s\n", ToHex(id, 16).c_str(),
            canUseModule ? "server module image" : "cache miss");
        moduleRequested = canUseModule;
        return canUseModule;
    }

    void ModuleCache(const uint8_t* id, const void* data, uint32_t size) override {
        std::printf("MODULE CACHE id=%s size=%u\n", ToHex(id, 16).c_str(), size);
        cachedId = ToHex(id, 16);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        cachedBlob.assign(bytes, bytes + size);
    }

    void StateSave(const void* data, uint32_t size) override {
        std::printf("STATE SAVE size=%u\n", size);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        savedState.assign(bytes, bytes + size);
        hasSavedState = true;
    }

    bool ClientImage(const uint8_t*& data, uint32_t& size, uint32_t& base) override {
        static std::vector<uint8_t> s_image;
        static uint32_t s_base;
        static bool s_attempted;

        if (!s_attempted) {
            s_attempted = true;
            const char* path = std::getenv("WARDEN_CLIENT_EXE");

            if (!path || !OsMacMapPeImage(path, s_image, s_base)) {
                std::printf("NOTE: no client executable (set WARDEN_CLIENT_EXE to the game's Wow.exe)\n");
                s_image.clear();
            } else {
                std::printf(".client executable %s: %u bytes at %08x\n", path, (uint32_t)s_image.size(), s_base);
            }
        }

        if (s_image.empty()) {
            return false;
        }

        data = s_image.data();
        size = (uint32_t)s_image.size();
        base = s_base;
        return true;
    }

    uint32_t ClientTimeMs() override {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint32_t)(now.tv_nsec / 1000000u));
    }

    bool ExecuteLua(const char* source, const char* filename, const char* taint) override {
        (void)filename;
        (void)taint;
        std::printf("LUA CHECK: %s\n", source ? source : "(null)");
        return true;
    }

    void InitializeBattlefield() override {
        std::printf("BATTLEFIELD INIT\n");
    }

    // The account name handed to the module's .zdata stub checks. A live session's login name
    // is not in the trace, so the replay takes it from WARDEN_ACCOUNT (uppercased here).
    std::string accountName = "WMTEST";

    const char* AccountName() override {
        return accountName.c_str();
    }

    // File checks: the live client answers through SFile2 (MPQ-aware). The offline replay does
    // the same: Storm's archives are opened from the game directory, so the module reads the
    // very bytes the real client would hand it. Without a usable archive set it falls back to
    // plain files (loose-file paths only) and reports which name it could not open.
    std::string gameDir;
    bool m_stormAttempted = false;
    bool m_stormReady = false;
    std::map<void*, bool> m_stormFiles;

    const std::string& GameDirectory() {
        if (!gameDir.empty()) {
            return gameDir;
        }

        const char* exe = std::getenv("WARDEN_CLIENT_EXE");
        gameDir = ".";

        if (exe && exe[0]) {
            std::string path = exe;
            size_t slash = path.find_last_of('/');

            if (slash != std::string::npos) {
                gameDir = path.substr(0, slash);
            }
        }

        return gameDir;
    }

    static bool FileExists(const std::string& path) {
        FILE* file = std::fopen(path.c_str(), "rb");

        if (!file) {
            return false;
        }

        std::fclose(file);
        return true;
    }

    void OpenArchive(const std::string& name, int32_t priority, bool optional) {
        std::string path = GameDirectory() + "/" + name;

        if (!FileExists(path)) {
            if (!optional) {
                std::printf("STORM archive missing: %s\n", name.c_str());
            }

            return;
        }

        SArchive* archive = nullptr;

        if (SFile::OpenArchive(path.c_str(), priority, 0xC00, &archive)) {
            std::printf("STORM archive %s at priority %d\n", name.c_str(), priority);
        } else {
            std::printf("STORM archive %s failed\n", name.c_str());
        }
    }

    void InitializeStorm() {
        if (m_stormAttempted) {
            return;
        }

        m_stormAttempted = true;

        char previous[4096];

        if (!getcwd(previous, sizeof(previous))) {
            previous[0] = '\0';
        }

        // SFileOpenArchive resolves relative paths against the process directory, exactly as the
        // game does once it chdirs into the client directory.
        if (chdir(GameDirectory().c_str()) != 0) {
            std::printf("STORM cannot enter %s\n", GameDirectory().c_str());
            return;
        }

        StormInitialize();

        int32_t priority = 1;
        OpenArchive("Data/common.mpq", priority++, false);
        OpenArchive("Data/common-2.mpq", priority++, true);
        OpenArchive("Data/expansion.mpq", priority++, true);
        OpenArchive("Data/lichking.mpq", priority++, true);
        OpenArchive("Data/enUS/base-enUS.mpq", priority++, true);
        OpenArchive("Data/enUS/locale-enUS.mpq", priority++, true);
        OpenArchive("Data/enUS/expansion-locale-enUS.mpq", priority++, true);
        OpenArchive("Data/enUS/lichking-locale-enUS.mpq", priority++, true);
        OpenArchive("Data/enUS/backup-enUS.mpq", priority++, true);
        OpenArchive("Data/enUS/speech-enUS.mpq", priority++, true);

        // Patch archives override the base data; the client opens every patch it finds, the
        // later ones above the earlier. Alphabetical order is enough here: the maps under test
        // are original data, and this keeps the replay's bytes independent of that detail.
        const char* patches[] = {
            "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ", "patch-5.MPQ", "patch-6.MPQ",
            "patch-7.MPQ", "patch-8.MPQ", "patch-a.MPQ", "patch-b.MPQ", "patch-c.MPQ",
            "patch-e.MPQ", "patch-f.MPQ", "patch-i.MPQ", "patch-j.MPQ", "patch-s.MPQ",
            "patch-t.MPQ", "patch-u.MPQ", "patch-w.MPQ", "patch-x.MPQ", "patch-y.MPQ",
            "patch-z.MPQ",
        };

        for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
            OpenArchive(std::string("Data/") + patches[i], priority++, true);
        }

        const char* localePatches[] = {
            "patch-enUS.MPQ", "patch-enUS-2.MPQ", "patch-enUS-3.MPQ", "patch-enUS-9.MPQ",
            "patch-enUS-a.MPQ", "patch-enUS-c.MPQ", "patch-enUS-d.MPQ", "patch-enUS-f.MPQ",
            "patch-enUS-j.MPQ", "patch-enUS-m.MPQ", "patch-enUS-n.MPQ", "patch-enUS-q.MPQ",
            "patch-enUS-s.MPQ", "patch-enUS-u.MPQ", "patch-enUS-w.MPQ", "patch-enUS-x.MPQ",
        };

        for (size_t i = 0; i < sizeof(localePatches) / sizeof(localePatches[0]); i++) {
            OpenArchive(std::string("Data/enUS/") + localePatches[i], priority++, true);
        }

        if (previous[0]) {
            chdir(previous);
        }

        m_stormReady = true;
    }

    void* FileOpen(const char* name) override {
        InitializeStorm();

        if (m_stormReady && name && name[0]) {
            SFileImpl* file = nullptr;
            int32_t opened = SFileOpenFile(name, &file);

            if (opened && file) {
                std::printf("SFILE OPEN '%s' -> opened (Storm)\n", name);
                m_stormFiles[file] = true;
                return file;
            }
        }

        std::string path = GameDirectory() + "/" + (name ? name : "");
        FILE* file = std::fopen(path.c_str(), "rb");
        std::printf("SFILE OPEN '%s' -> %s\n", name ? name : "(null)", file ? path.c_str() : "failed");

        if (file) {
            m_stormFiles[file] = false;
        }

        return file;
    }

    uint32_t FileSize(void* file) override {
        if (!file) {
            return 0;
        }

        if (m_stormFiles[file]) {
            uint32_t size = SFileGetFileSize(static_cast<SFileImpl*>(file), nullptr);
            std::printf("SFILE SIZE -> %u (Storm)\n", size);
            return size;
        }

        FILE* stream = static_cast<FILE*>(file);
        long here = std::ftell(stream);
        std::fseek(stream, 0, SEEK_END);
        long size = std::ftell(stream);
        std::fseek(stream, here, SEEK_SET);
        std::printf("SFILE SIZE -> %ld\n", size);
        return size > 0 ? (uint32_t)size : 0;
    }

    uint32_t FileRead(void* file, void* buffer, uint32_t count) override {
        if (!file) {
            return 0;
        }

        if (m_stormFiles[file]) {
            uint32_t read = 0;

            if (!SFileReadFile(static_cast<SFileImpl*>(file), buffer, count, &read, nullptr, 0)) {
                std::printf("SFILE READ %u -> failed (Storm)\n", count);
                return 0;
            }

            std::printf("SFILE READ %u -> %u (Storm)\n", count, read);
            return read;
        }

        uint32_t read = (uint32_t)std::fread(buffer, 1, count, static_cast<FILE*>(file));
        std::printf("SFILE READ %u -> %u\n", count, read);
        return read;
    }

    void FileClose(void* file) override {
        if (!file) {
            return;
        }

        if (m_stormFiles[file]) {
            SFileCloseFile(static_cast<SFileImpl*>(file));
        } else {
            std::fclose(static_cast<FILE*>(file));
        }

        m_stormFiles.erase(file);
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

    if (const char* account = getenv("WARDEN_ACCOUNT")) {
        services.accountName = account;
    }

    // WARDEN_REPLAY_COLD=1 replays a session whose client had no cached module: the module is
    // downloaded through the trace instead of taken from <module.bin>.
    const char* cold = std::getenv("WARDEN_REPLAY_COLD");
    services.canUseModule = !(cold && cold[0] == '1');
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
        active = StartModule(services, services.downloadedImage.empty() ? serverImage : services.downloadedImage,
            key.data(), "server module");
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

    // The client keeps polling after the last packet (the character screen sits there); periodic
    // work such as memory scans runs on these frames. 3000 frames of 16 ms is about 48 seconds.
    for (uint32_t frame = 0; frame < 3000; frame++) {
        active->Process(16);

        if (!activateNext()) {
            return 1;
        }
    }

    std::printf(".3000 trailing frames processed\n");

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
