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

private:
    struct Impl;
    Impl* m_impl;
};

#endif
