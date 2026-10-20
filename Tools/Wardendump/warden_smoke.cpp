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
 *   WardenSmoke [options] <module.bin> <trace.log> <session-key.bin>
 *       replay a captured session exactly like the client runs it: the built-in module takes
 *       the trace's "SMSG <hex>" lines, and when it asks for the server module (ModuleUse) the
 *       module blob from that session's ModuleCache is decrypted and activated, picking up the
 *       built-in module's saved RC4 state. Every module output is compared with the trace's
 *       "CMSG" lines in order.
 *
 *       The replay mirrors the client's module cache, which is what makes the packets line up
 *       with the live session: a ModuleUse before the module has been cached in *this* session
 *       returns false (the built-in module answers "module missing" and keeps processing the
 *       503-byte module-transfer packets), exactly like a fresh client. <module.bin> is only a
 *       stand-in for a cache hit at session start (a login that reused an earlier download),
 *       selected with --cache-hit or WARDEN_REPLAY_CACHE_HIT=1.
 *
 *   Options:
 *     --session <n>         session to replay, 1-based; negative counts from the end
 *                           (default: the last session that has packets)
 *     --until <n>           feed server packets 0..n, then stop (0-based packet index)
 *     --packet <n>          alias for --until <n>: run exactly the check in that SMSG entry
 *     --list                list the trace's sessions and exit
 *     --list-packets        with --session: list the session's packets/outputs and exit
 *     --trailing <n>        Process frames after the last packet (file checks read files and
 *                           answer on these polls; default 0 with --until, 3000 otherwise)
 *     --dump-plain <dir>    write each server-module packet's decrypted bytes to <dir>
 *     --feed <hex|@file>    after the replay, feed a plaintext Warden check payload to the
 *                           module (encrypted with its current inbound RC4 stream) and print
 *                           the CMSG bytes it produces
 *     --account <name>      account name handed to the module's .zdata checks
 *     --cache-hit           let <module.bin> satisfy a ModuleUse (old behavior)
 *
 *   WardenSmoke --snapshot <warden-snapshot.bin>
 *       rerun the entry call a live session failed in, from the machine state captured just
 *       before it; the full branch trace lands in Logs/warden.log (or ./warden.log).
 *
 * Typical offline iteration on one check (the key and module image are per session; the key is
 * written to Logs/warden-session-key.bin by the login, so copy it before the next login):
 *
 *   WardenSmoke --list Logs/warden-traffic.log
 *   WardenSmoke --session 70 --list-packets Logs/warden-traffic.log
 *   WardenSmoke --session 70 --until 61 --dump-plain /tmp/warden-plain \
 *       Logs/warden-module-image.bin Logs/warden-traffic.log Logs/warden-session-key.bin
 *   WardenSmoke --session 70 --until 60 --feed @/tmp/warden-plain/packet-061.plain \
 *       Logs/warden-module-image.bin Logs/warden-traffic.log Logs/warden-session-key.bin
 *
 * WARDEN_CLIENT_EXE=<path to Wow.exe> gives the module the client executable, as the client
 * does from its own directory; replays need it to match a live session.
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <zlib.h>
#include <CommonCrypto/CommonDigest.h>

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
    bool compareOutputs = true;

    // The client's g_wardenCache, keyed by the 16-byte module id. A ModuleUse only succeeds
    // after this session's ModuleCache filled the entry, unless a preloaded image is allowed to
    // stand in for a cache hit.
    std::map<std::string, std::vector<uint8_t>> cachedModules;
    std::vector<uint8_t> fallbackImage;
    bool preloadImage = false;
    bool moduleRequested = false;
    std::vector<uint8_t> pendingImage;

    // Labels the outputs with the packet that produced them; a module Data call can emit more
    // than one CMSG (the zdata bridge emits the account reply before the normal answer).
    std::string activeLabel = "built-in";
    uint32_t currentPacket = 0;

    // --dump-plain: decrypt the delivered packet with a copy of the module's inbound RC4 state.
    // Only the server module's object layout is known (the 24576-byte built-in module differs),
    // so the replay skips the built-in module's packets.
    std::string dumpPlainDir;

    // OurLibrary::StateSave / StateLoad, exactly like the client's single saved-state slot.
    std::vector<uint8_t> savedState;
    bool hasSavedState = false;

    void ModuleOutput(const uint8_t* data, uint32_t size) override {
        std::string hex = ToHex(data, size);
        uint32_t index = outputCount++;

        std::printf("MODULE OUTPUT #%u (%u bytes, %s module, after packet %u): %s", index, size,
            activeLabel.c_str(), currentPacket, hex.c_str());

        if (compareOutputs && index < expectedOutputs.size()) {
            bool same = expectedOutputs[index] == hex;
            outputMismatches += same ? 0 : 1;
            std::printf("  [%s]\n", same ? "matches the live session" : "DIFFERS from the live session");

            if (!same) {
                std::printf("    live session sent: %s\n", expectedOutputs[index].c_str());
            }
        } else {
            std::printf("\n");
        }
    }

    bool ModuleUse(const uint8_t* id, const uint8_t* key) override {
        std::string hex = ToHex(id, 16);
        auto cached = cachedModules.find(hex);

        if (cached != cachedModules.end() && cached->second.size() > 4 + 260) {
            // The client's LoadModule: RC4 with the key the built-in module passed, then inflate.
            std::vector<uint8_t> blob = cached->second;
            Rc4Crypt(key, 16, blob.data(), (uint32_t)blob.size());

            uint32_t capacity = 0;
            std::memcpy(&capacity, blob.data(), sizeof(capacity));
            std::vector<uint8_t> image(capacity);
            uLongf imageSize = capacity;

            if (uncompress(image.data(), &imageSize, &blob[4], blob.size() - 4) != Z_OK) {
                std::printf("MODULE USE id=%s -> cached module does not inflate\n", hex.c_str());
                return false;
            }

            image.resize((size_t)imageSize);
            std::printf("MODULE USE id=%s -> cached module (%u byte blob -> %u byte image)\n",
                hex.c_str(), (uint32_t)cached->second.size(), (uint32_t)imageSize);
            pendingImage = std::move(image);
            moduleRequested = true;
            return true;
        }

        if (preloadImage && !fallbackImage.empty()) {
            std::printf("MODULE USE id=%s -> preloaded image (cache hit emulation)\n", hex.c_str());
            pendingImage = fallbackImage;
            moduleRequested = true;
            return true;
        }

        // Exactly the live client on a fresh cache: the module answers "module missing" and the
        // server sends the module (the 503-byte transfer packets) followed by a ModuleCache.
        std::printf("MODULE USE id=%s -> cache miss\n", hex.c_str());
        return false;
    }

    void ModuleCache(const uint8_t* id, const void* data, uint32_t size) override {
        std::string hex = ToHex(id, 16);
        std::printf("MODULE CACHE id=%s size=%u\n", hex.c_str(), size);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        cachedModules[hex].assign(bytes, bytes + size);
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

// ---------------------------------------------------------------------
// Trace sessions
// ---------------------------------------------------------------------

struct TraceSession {
    uint32_t firstLine = 0;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<std::string> outputs;
};

struct ReplayOptions {
    // 1-based; negative counts from the end. Zero means "the last session with packets".
    int32_t session = 0;
    // Feed server packets 0..until, then stop instead of polling the trailing frames. -1 = all.
    int32_t until = -1;
    // Process frames after the last fed packet (file checks finish their reads on these polls).
    // -1 = automatic: 0 with --until, 3000 in a full replay.
    int32_t trailing = -1;
    std::string dumpPlainDir;
    std::string feedPlain;
    std::string account;
};

static bool ParseHexBytes(const std::string& text, std::vector<uint8_t>& out) {
    std::string digits;

    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ':' && c != 'x' && c != 'X') {
            return false;
        }
    }

    if (digits.empty() || digits.size() % 2 != 0) {
        return false;
    }

    out.clear();

    for (size_t i = 0; i + 1 < digits.size(); i += 2) {
        char byteText[3] = { digits[i], digits[i + 1], 0 };
        out.push_back((uint8_t)std::strtoul(byteText, nullptr, 16));
    }

    return true;
}

static bool ReadFeedPlain(const std::string& argument, std::vector<uint8_t>& plain) {
    if (!argument.empty() && argument[0] == '@') {
        return ReadFile(argument.c_str() + 1, plain);
    }

    return ParseHexBytes(argument, plain);
}

static std::vector<TraceSession> ReadTrace(const char* tracePath) {
    std::vector<TraceSession> sessions;
    FILE* trace = std::fopen(tracePath, "r");

    if (!trace) {
        return sessions;
    }

    // A captured 0x41D payload is 10838 bytes, so one line can carry ~22 KB of hex.
    const size_t lineSize = 1 << 20;
    std::vector<char> line(lineSize);
    uint32_t lineNumber = 0;

    while (std::fgets(line.data(), (int)lineSize, trace)) {
        lineNumber++;

        if (std::strncmp(line.data(), "SESSION", 7) == 0) {
            sessions.push_back(TraceSession());
            sessions.back().firstLine = lineNumber;
            continue;
        }

        if (sessions.empty()) {
            continue;
        }

        bool server = std::strncmp(line.data(), "SMSG ", 5) == 0;
        bool client = std::strncmp(line.data(), "CMSG ", 5) == 0;

        if (!server && !client) {
            continue;
        }

        std::string hex(line.data() + 5);

        while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
            hex.pop_back();
        }

        if (client) {
            sessions.back().outputs.push_back(hex);
            continue;
        }

        std::vector<uint8_t> packet;

        if (!ParseHexBytes(hex, packet)) {
            std::printf("WARN: unparsable SMSG line %u\n", lineNumber);
            continue;
        }

        sessions.back().packets.push_back(packet);
    }

    std::fclose(trace);
    return sessions;
}

static int PrintSessions(const std::vector<TraceSession>& sessions) {
    if (sessions.empty()) {
        std::printf("no SESSION markers in the trace\n");
        return 1;
    }

    for (size_t i = 0; i < sessions.size(); i++) {
        const TraceSession& session = sessions[i];
        uint32_t firstPacket = session.packets.empty() ? 0 : (uint32_t)session.packets[0].size();
        std::printf("session %u (trace line %u): %u server packets, %u client outputs, first SMSG %u bytes\n",
            (uint32_t)(i + 1), session.firstLine, (uint32_t)session.packets.size(),
            (uint32_t)session.outputs.size(), firstPacket);
    }

    return 0;
}

static int PrintSessionPackets(const std::vector<TraceSession>& sessions, int32_t index) {
    if (sessions.empty()) {
        std::printf("no SESSION markers in the trace\n");
        return 1;
    }

    if (index == 0) {
        index = (int32_t)sessions.size();
    }

    int32_t selected = index > 0 ? index - 1 : (int32_t)sessions.size() + index;

    if (selected < 0 || selected >= (int32_t)sessions.size()) {
        std::printf("session %d does not exist (%u sessions)\n", index, (uint32_t)sessions.size());
        return 1;
    }

    const TraceSession& session = sessions[(size_t)selected];
    std::printf("session %d (trace line %u): %u server packets, %u client outputs\n", selected + 1,
        session.firstLine, (uint32_t)session.packets.size(), (uint32_t)session.outputs.size());

    for (size_t i = 0; i < session.packets.size(); i++) {
        std::printf("  packet[%u] SMSG %u bytes\n", (uint32_t)i, (uint32_t)session.packets[i].size());
    }

    for (size_t i = 0; i < session.outputs.size(); i++) {
        std::printf("  output[%u] CMSG %u bytes: %s\n", (uint32_t)i,
            (uint32_t)(session.outputs[i].size() / 2),
            session.outputs[i].substr(0, 80).c_str());
    }

    return 0;
}

// ---------------------------------------------------------------------
// RC4 stepping over a copy of the module's state
// ---------------------------------------------------------------------

// The module keeps the inbound RC4 stream as S[256] followed by i and j (object +0x122), the
// same layout WardenEmu's WARDEN_TRACE_PACKETS diagnostics use.
static uint8_t Rc4StreamByte(uint8_t* state) {
    uint8_t& a = state[0x100];
    uint8_t& b = state[0x101];
    a = static_cast<uint8_t>(a + 1);
    b = static_cast<uint8_t>(b + state[a]);
    uint8_t t = state[a];
    state[a] = state[b];
    state[b] = t;
    return state[static_cast<uint8_t>(state[b] + state[a])];
}

static void Rc4StreamApply(uint8_t* state, uint8_t* data, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        data[i] ^= Rc4StreamByte(state);
    }
}

static std::string ToHexPreview(const uint8_t* data, uint32_t size, uint32_t limit = 64) {
    return ToHex(data, size < limit ? size : limit);
}

// The client flow for a captured session: built-in module first, the server module once the
// built-in one asks for it, activation after every Data/Process like ActivateNextModule.
static int ReplaySession(const char* imagePath, const char* tracePath, const char* keyPath,
    const ReplayOptions& options) {
    std::vector<uint8_t> serverImage;
    std::vector<uint8_t> builtinImage;
    std::vector<uint8_t> key;

    // "-" means no preloaded image: a cold session gets the module from the trace's ModuleCache.
    if (std::strcmp(imagePath, "-") != 0) {
        if (!ReadFile(imagePath, serverImage)) {
            std::printf("FAIL: cannot read module image %s (pass - on a cold session; the module\n"
                        "      then comes from the trace's ModuleCache)\n", imagePath);
            return 1;
        }
    } else {
        std::printf(".no preloaded module image\n");
    }

    if (!ReadFile(keyPath, key) || key.size() != 40) {
        std::printf("FAIL: session key %s must be 40 bytes\n", keyPath);
        return 1;
    }

    if (!BuiltinImage(builtinImage)) {
        std::printf("FAIL: built-in module inflate\n");
        return 1;
    }

    std::vector<TraceSession> sessions = ReadTrace(tracePath);

    if (sessions.empty()) {
        std::printf("FAIL: no SESSION markers in %s\n", tracePath);
        return 1;
    }

    // --session <n>: 1-based, negative counts from the end. Zero picks the last session that
    // actually carried Warden packets (a login can end before the server sends any).
    int32_t selected = options.session;

    if (selected == 0) {
        selected = (int32_t)sessions.size();

        while (selected > 0 && sessions[(size_t)(selected - 1)].packets.empty()) {
            selected--;
        }
    } else if (selected < 0) {
        selected = (int32_t)sessions.size() + selected + 1;
    }

    if (selected < 1 || selected > (int32_t)sessions.size()) {
        std::printf("FAIL: session %d does not exist (%u sessions; use --list)\n", options.session,
            (uint32_t)sessions.size());
        return 1;
    }

    const TraceSession& session = sessions[(size_t)(selected - 1)];
    std::printf(".trace: session %d of %u (trace line %u): %u server packets, %u client packets\n",
        selected, (uint32_t)sessions.size(), session.firstLine, (uint32_t)session.packets.size(),
        (uint32_t)session.outputs.size());

    if (session.packets.empty()) {
        std::printf("FAIL: session %d has no server packets\n", selected);
        return 1;
    }

    uint32_t lastPacket = (uint32_t)session.packets.size();

    if (options.until >= 0) {
        if ((uint32_t)options.until >= lastPacket) {
            std::printf("FAIL: --until %d is past the session's last packet (%u)\n", options.until,
                lastPacket - 1);
            return 1;
        }

        lastPacket = (uint32_t)options.until + 1;
    }

    SmokeServices services;
    services.expectedOutputs = session.outputs;
    services.fallbackImage = serverImage;
    services.dumpPlainDir = options.dumpPlainDir;

    if (!options.account.empty()) {
        services.accountName = options.account;
    } else if (const char* account = getenv("WARDEN_ACCOUNT")) {
        services.accountName = account;
    }

    // The client's cache survives logins, but a single captured session normally starts with the
    // built-in module and a cache miss; --cache-hit is for sessions whose module came from an
    // earlier login (the module image then has to be given on the command line).
    const char* cacheHit = std::getenv("WARDEN_REPLAY_CACHE_HIT");
    const char* cold = std::getenv("WARDEN_REPLAY_COLD");
    services.preloadImage = (cacheHit && cacheHit[0] == '1') || (cold && cold[0] == '0');

    if (services.preloadImage) {
        std::printf(".module cache: preloaded image %s stands in for a cache hit\n", imagePath);
    } else {
        std::printf(".module cache: empty at session start (a ModuleUse before ModuleCache is a miss)\n");
    }

    if (!options.dumpPlainDir.empty()) {
        mkdir(options.dumpPlainDir.c_str(), 0755);
    }

    WardenEmu* active = StartModule(services, builtinImage, key.data(), "built-in module");

    if (!active) {
        return 1;
    }

    // ActivateNextModule: the old module shuts down (saving its state) before the new one is
    // created and initialized (loading it). The pending image comes from ModuleCache, exactly as
    // the client's g_wardenCache does.
    auto activateNext = [&]() -> bool {
        if (!services.moduleRequested) {
            return true;
        }

        services.moduleRequested = false;
        active->Shutdown();
        delete active;
        services.activeLabel = "server";
        active = StartModule(services, services.pendingImage, key.data(), "server module");
        services.pendingImage.clear();
        return active != nullptr;
    };

    const uint32_t framesPerPacket = 3;

    for (uint32_t index = 0; index < lastPacket; index++) {
        services.currentPacket = index;
        const std::vector<uint8_t>& packet = session.packets[index];

        // --dump-plain: decrypt a copy with the module's live inbound state. This is the check
        // payload the module is about to see, which can be edited and fed back with --feed.
        if (!services.dumpPlainDir.empty() && services.activeLabel == "server") {
            uint8_t state[0x102];

            if (active->CopyInboundRc4(state)) {
                std::vector<uint8_t> plain(packet);
                Rc4StreamApply(state, plain.data(), (uint32_t)plain.size());
                char name[512];
                std::snprintf(name, sizeof(name), "%s/packet-%03u.plain", services.dumpPlainDir.c_str(),
                    index);

                FILE* file = std::fopen(name, "wb");

                if (file) {
                    std::fwrite(plain.data(), 1, plain.size(), file);
                    std::fclose(file);
                }

                std::printf("PLAIN #%u (%u bytes): %s%s\n", index, (uint32_t)plain.size(),
                    ToHexPreview(plain.data(), (uint32_t)plain.size()).c_str(),
                    plain.size() > 64 ? "..." : "");

                // Warden opcodes are small; a random-looking first byte means the module keeps
                // its RC4 stream at another object offset (a layout this tool does not know).
                if (!plain.empty() && plain[0] > 0x0F) {
                    std::printf("WARN: PLAIN #%u starts with %02x, not a Warden opcode; the module's\n"
                                "      RC4 stream layout may differ from the one this tool assumes.\n",
                        index, plain[0]);
                }
            }
        }

        uint32_t consumed = 0;
        active->Data(packet.data(), (uint32_t)packet.size(), consumed);
        std::printf(".packet[%u] %u bytes, consumed %u\n", index, (uint32_t)packet.size(), consumed);

        if (index == 0 && consumed == 0 && services.activeLabel == "built-in") {
            std::printf("NOTE: the first packet was not consumed, so the module could not decrypt it.\n"
                        "      The 40-byte session key belongs to exactly one login; the client writes it\n"
                        "      to Logs/warden-session-key.bin on every ActivateNextModule and overwrites\n"
                        "      it on the next login. Copy the key next to the trace before logging in again.\n");
        }

        if (!activateNext()) {
            return 1;
        }

        // The client polls every frame between packets.
        for (uint32_t frame = 0; frame < framesPerPacket; frame++) {
            active->Process(16);

            if (!activateNext()) {
                return 1;
            }
        }
    }

    // The client keeps polling after the last packet (the character screen sits there); periodic
    // work such as memory scans and the file checks' reads run on these frames. A full replay
    // mirrors the ~48 seconds (3000 x 16 ms) the session had; a targeted check can ask for just
    // enough frames with --trailing.
    int32_t trailing = options.trailing;

    if (trailing < 0) {
        trailing = options.until < 0 ? 3000 : 0;
    }

    for (int32_t frame = 0; frame < trailing; frame++) {
        active->Process(16);

        if (!activateNext()) {
            return 1;
        }
    }

    if (trailing > 0) {
        std::printf(".%d trailing frames processed\n", trailing);
    }

    // --feed: hand the module one plaintext check payload at the current RC4 position. The
    // module decrypts what Data receives, so encrypt the payload with a copy of its inbound
    // stream; the module then sees exactly the plaintext, as if the server had sent it now.
    int32_t feedResult = 0;

    if (!options.feedPlain.empty()) {
        std::vector<uint8_t> plain;

        if (!ReadFeedPlain(options.feedPlain, plain) || plain.empty()) {
            std::printf("FAIL: --feed needs hex bytes or @file\n");
            feedResult = 1;
        } else {
            uint8_t state[0x102];

            if (!active->CopyInboundRc4(state)) {
                std::printf("FAIL: cannot read the module's inbound RC4 state\n");
                feedResult = 1;
            } else {
                if (services.activeLabel != "server") {
                    std::printf("NOTE: the built-in module keeps its RC4 stream at a different object\n"
                                "      offset; plaintext feeding is calibrated for the server modules.\n");
                }

                std::printf(".feeding %u plaintext bytes (module %s, after packet %u)\n",
                    (uint32_t)plain.size(), services.activeLabel.c_str(), services.currentPacket);
                std::printf(".plaintext: %s%s\n", ToHexPreview(plain.data(), (uint32_t)plain.size()).c_str(),
                    plain.size() > 64 ? "..." : "");

                std::vector<uint8_t> cipher(plain);
                Rc4StreamApply(state, cipher.data(), (uint32_t)cipher.size());

                services.compareOutputs = false;
                uint32_t consumed = 0;
                active->Data(cipher.data(), (uint32_t)cipher.size(), consumed);
                std::printf(".feed consumed %u of %u bytes\n", consumed, (uint32_t)cipher.size());

                // Some checks finish on the next poll rather than inside Data (the periodic
                // answer is packed there, and file checks read on the following frames), so give
                // the module the frames the client would have run.
                uint32_t feedFrames = options.trailing > 0 ? (uint32_t)options.trailing : framesPerPacket;

                for (uint32_t frame = 0; frame < feedFrames; frame++) {
                    active->Process(16);

                    if (!activateNext()) {
                        feedResult = 1;
                        break;
                    }
                }
            }
        }
    }

    active->Shutdown();
    delete active;

    std::printf(".replay done: %u outputs, %u of them differ from the live session\n", services.outputCount,
        services.outputMismatches);

    if (feedResult) {
        return feedResult;
    }

    return services.outputMismatches ? 2 : 0;
}

// Compare a captured Warmane file check with the bytes Storm returns for that path.
// A single-file check is [02][pathLen][path][00][20-byte SHA1]. The hash is of the file the
// official client's SFile handed the module; matching it means our read is the same file.
int HashCapturedChecks(const char* logPath) {
    setenv("WARDEN_CLIENT_EXE", "/Volumes/Yaman SSD/wow3.3.5a/Wow.exe", 0);

    SmokeServices services;
    FILE* log = std::fopen(logPath, "r");

    if (!log) {
        std::printf("cannot read %s\n", logPath);
        return 1;
    }

    char line[4096];
    int32_t checked = 0;
    int32_t matched = 0;
    int32_t mismatched = 0;

    while (std::fgets(line, sizeof(line), log)) {
        const char* hex = std::strstr(line, " map ");

        if (!hex) {
            continue;
        }

        hex += 5;

        uint8_t raw[512];
        int32_t rawLen = 0;

        while (hex[0] && hex[1] && rawLen < (int32_t)sizeof(raw)) {
            if (hex[0] == '\n' || hex[0] == ' ') {
                break;
            }

            unsigned value = 0;

            if (std::sscanf(hex, "%2x", &value) != 1) {
                break;
            }

            raw[rawLen++] = (uint8_t)value;
            hex += 2;
        }

        if (rawLen < 4 || raw[0] != 0x02) {
            continue;
        }

        uint8_t pathLen = raw[1];

        if (pathLen < 8 || 2 + pathLen + 21 > rawLen || raw[2 + pathLen] != 0x00) {
            continue;
        }

        char path[256];

        if (pathLen >= sizeof(path)) {
            continue;
        }

        std::memcpy(path, raw + 2, pathLen);
        path[pathLen] = '\0';

        const uint8_t* expected = raw + 2 + pathLen + 1;
        void* file = services.FileOpen(path);

        if (!file) {
            std::printf("MISS %s\n", path);
            continue;
        }

        CC_SHA1_CTX sha1;
        CC_SHA1_Init(&sha1);
        uint8_t chunk[16384];
        uint32_t total = 0;

        // The Warden module hashes a prefix of the file (it stops after 72 reads of 16 KiB,
        // 0x120000 bytes). WARDEN_SHA1_LIMIT tests that window against the check data.
        const char* limitText = std::getenv("WARDEN_SHA1_LIMIT");
        uint32_t limit = limitText ? static_cast<uint32_t>(std::strtoul(limitText, nullptr, 0)) : 0;

        for (;;) {
            uint32_t read = 0;
            int32_t ok = SFileReadFile(static_cast<SFileImpl*>(file), chunk, sizeof(chunk), &read, nullptr, 0);

            if (!read) {
                break;
            }

            if (limit && total + read > limit) {
                read = limit - total;
            }

            CC_SHA1_Update(&sha1, chunk, read);
            total += read;

            if ((limit && total >= limit) || !ok || read < sizeof(chunk)) {
                break;
            }
        }

        services.FileClose(file);

        uint8_t digest[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1_Final(digest, &sha1);
        bool same = std::memcmp(digest, expected, CC_SHA1_DIGEST_LENGTH) == 0;
        checked++;

        if (same) {
            matched++;
        } else {
            mismatched++;
        }

        if (checked <= 8 || !same) {
            std::printf("%s %s size=%u\n  expected ", same ? "MATCH" : "DIFF", path, total);

            for (int32_t i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) {
                std::printf("%02x", expected[i]);
            }

            std::printf("\n  ours     ");

            for (uint8_t byte : digest) {
                std::printf("%02x", byte);
            }

            std::printf("\n");
        }
    }

    std::fclose(log);
    std::printf("checked %d matched %d mismatched %d\n", checked, matched, mismatched);
    return mismatched ? 1 : 0;
}

static void PrintUsage() {
    std::printf(
        "usage: WardenSmoke [options] <module.bin|-> <trace.log> <session-key.bin>\n"
        "       WardenSmoke --list <trace.log>\n"
        "       WardenSmoke --list-packets [--session <n>] <trace.log>\n"
        "       WardenSmoke --snapshot <warden-snapshot.bin>\n"
        "       WardenSmoke --sha1 <warden.log>\n"
        "options: --session <n> --until <n>|--packet <n> --trailing <n> --dump-plain <dir>\n"
        "         --feed <hex|@file> --account <name> --cache-hit\n");
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--sha1") == 0) {
        return HashCapturedChecks(argv[2]);
    }

    if (argc >= 3 && std::strcmp(argv[1], "--snapshot") == 0) {
        return ReplaySnapshot(argv[2]);
    }

    ReplayOptions options;
    bool list = false;
    bool listPackets = false;
    std::vector<const char*> positional;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        bool hasValue = i + 1 < argc;

        if (std::strcmp(arg, "--session") == 0 && hasValue) {
            options.session = std::atoi(argv[++i]);
        } else if ((std::strcmp(arg, "--until") == 0 || std::strcmp(arg, "--packet") == 0) && hasValue) {
            // --packet N is the same as --until N: replay up to and including that server packet.
            options.until = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--trailing") == 0 && hasValue) {
            options.trailing = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--dump-plain") == 0 && hasValue) {
            options.dumpPlainDir = argv[++i];
        } else if (std::strcmp(arg, "--feed") == 0 && hasValue) {
            options.feedPlain = argv[++i];
        } else if (std::strcmp(arg, "--account") == 0 && hasValue) {
            options.account = argv[++i];
        } else if (std::strcmp(arg, "--cache-hit") == 0) {
            setenv("WARDEN_REPLAY_CACHE_HIT", "1", 1);
        } else if (std::strcmp(arg, "--list") == 0) {
            list = true;
        } else if (std::strcmp(arg, "--list-packets") == 0) {
            listPackets = true;
        } else if (arg[0] == '-' && arg[1] != '\0' && std::strcmp(arg, "-") != 0) {
            std::printf("unknown option %s\n", arg);
            PrintUsage();
            return 1;
        } else {
            positional.push_back(arg);
        }
    }

    if (list || listPackets) {
        if (positional.size() != 1) {
            PrintUsage();
            return 1;
        }

        std::vector<TraceSession> sessions = ReadTrace(positional[0]);

        if (list) {
            return PrintSessions(sessions);
        }

        return PrintSessionPackets(sessions, options.session);
    }

    if (positional.size() == 3) {
        return ReplaySession(positional[0], positional[1], positional[2], options);
    }

    if (positional.size() > 3 || positional.size() == 2) {
        PrintUsage();
        return 1;
    }

    std::vector<uint8_t> image;

    if (positional.size() == 1) {
        // A dumped server module image (Logs/warden-module-image.bin): the emulator loads it
        // directly, with no RC4 or inflate step.
        if (!ReadFile(positional[0], image)) {
            std::printf("FAIL: cannot read %s\n", positional[0]);
            return 1;
        }

        std::printf(".module file %s -> %u bytes\n", positional[0], (uint32_t)image.size());
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
