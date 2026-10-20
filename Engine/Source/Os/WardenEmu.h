#ifndef ENGINE_SOURCE_OS_WARDENEMU_H
#define ENGINE_SOURCE_OS_WARDENEMU_H

#include <cstddef>
#include <cstdint>

/*
 * Emulated BLL loader and Warden module runner for hosts that cannot execute the Warden
 * modules natively (for example an arm64 macOS build running 32-bit Windows module images).
 *
 * The 3.3.5a Warden protocol lives inside the module image the server hands the client: the
 * client-side code is only a small bridge that feeds server data into the module and sends the
 * module's packets back out. This loader runs the module's real code in a 32-bit x86 machine
 * (Unicorn) and bridges the module's calls back out through WardenEmuServices, so the protocol
 * is executed as authored.
 *
 * Everything here is emulation of the genuine client behaviour: nothing generates or alters
 * check results, the module's own code decides what to send.
 */

// The client-side half: called back by the emulator when the module does its erarbeitungs.
class WardenEmuServices {
public:
    // The module produced a packet to send to the server (payload of CMSG_WARDEN_DATA).
    virtual void ModuleOutput(const uint8_t* data, uint32_t size) = 0;

    // The module asks to switch to a cached module by its 16-byte id and 16-byte key.
    // Return true when a module is waiting to be activated.
    virtual bool ModuleUse(const uint8_t* id, const uint8_t* key) = 0;

    // The module asks to store a module image for its 16-byte id.
    virtual void ModuleCache(const uint8_t* id, const void* data, uint32_t size) = 0;

    // The module wants to persist / restore its state across module swaps.
    // StateLoad fills "data" with at most size bytes and reports the number restored.
    virtual void StateSave(const void* data, uint32_t size) = 0;
    virtual bool StateLoad(void* data, uint32_t& size) = 0;

    // The client's own executable, laid out the way the Windows loader maps it, and its base.
    // On Windows the module runs inside the client process: GetModuleHandle(NULL) names this
    // image and server requests resolve client functions as offsets into it. The data must stay
    // valid for the services' lifetime. False when no Windows executable is available.
    virtual bool ClientImage(const uint8_t*& data, uint32_t& size, uint32_t& base) = 0;

    // OsGetAsyncTimeMs: the client clock, which modules call directly inside the client image.
    virtual uint32_t ClientTimeMs() = 0;

    // FrameScript::Execute: the client's Lua entry point, which Warden modules call for Lua
    // checks. The module hands over a script in the guest; the host runs it in the game's Lua
    // state. The script itself sends the check answer (for example through SendAddonMessage),
    // exactly as it does in the Windows client.
    virtual bool ExecuteLua(const char* source, const char* filename, const char* taint) = 0;

    // CGBattlefieldInfo::InitializeGame: Warmane's custom module calls the client's battlefield
    // initializer before it reads the handlers it installs. The host runs the client's own
    // implementation; the module then finds the table it expects.
    virtual void InitializeBattlefield() = 0;

    // The logged-in account name, exactly as the client sends it (uppercased). Warmane's module
    // asks the client's .zdata stub (client RVA 0x9D1000) for it as part of its account checks;
    // the official client's runtime-generated stub returns this same string.
    virtual const char* AccountName() = 0;

    // SFile2: the module reads game files through the client's own Storm file API and hashes
    // their bytes (the live 364-byte challenge checks a file this way). The handle is opaque to
    // the module, so the host may return its own SFile object. FileRead returns the bytes read;
    // zero means failure, exactly as SFileReadFile reports it.
    virtual void* FileOpen(const char* name) = 0;
    virtual uint32_t FileSize(void* file) = 0;
    virtual uint32_t FileRead(void* file, void* buffer, uint32_t count) = 0;
    virtual void FileClose(void* file) = 0;

    virtual ~WardenEmuServices() {}
};

// One emulated module: the BLL image and its loaded guest state.
class WardenEmu {
public:
    explicit WardenEmu(WardenEmuServices* services);
    ~WardenEmu();

    WardenEmu(const WardenEmu&) = delete;
    WardenEmu& operator=(const WardenEmu&) = delete;

    // Load a BLL module image (the zlib-expanded form the client's real loader receives).
    bool Load(const void* data, uint32_t size);

    // Run the module's export (ordinal 1), the module object constructor.
    // OurLibrary is handed over as a guest object hooked through the services above.
    bool CreateModule();

    // WardenModule entry points, in vtable order Initialize / Shutdown / Data / Process.
    bool Initialize(const uint8_t* sessionKey, uint32_t keyLength);
    void Shutdown();
    void Data(const void* data, uint32_t size, uint32_t& consumed);
    void Process(uint32_t elapsedMs);

    bool ImageLoaded() const;
    bool ModuleCreated() const;

    // Diagnostics: when an entry call fails, the machine state captured just before it is
    // written to Logs/warden-snapshot.bin. This reruns that exact call on a fresh machine, so
    // a failure seen once in a live session can be studied offline as often as needed.
    bool ReplaySnapshot(const char* path);

    // Diagnostics: copy the module's inbound RC4 stream (256-byte S-box followed by i and j).
    // The offset is the server modules' object layout (+0x122); the 24576-byte built-in module
    // uses a different one. WardenSmoke uses it to decrypt a captured packet for inspection and
    // to encrypt a plaintext check before Data(), so it can iterate on one check offline.
    bool CopyInboundRc4(uint8_t state[0x102]) const;

private:
    struct Impl;
    Impl* m_impl;
};

#endif
