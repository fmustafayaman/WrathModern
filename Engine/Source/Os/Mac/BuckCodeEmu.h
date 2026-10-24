#ifndef ENGINE_SOURCE_OS_MAC_BUCKCODEEMU_H
#define ENGINE_SOURCE_OS_MAC_BUCKCODEEMU_H

#include <cstdint>
#include <string>
#include <vector>

/*
 * Warmane's buck-data code stream (opcodes 0x2E9 / 0x4B8 / 0x4B9 and the challenge opcode
 * 0x41B) delivers x86 code from the server. The official 3.3.5a client executes it in its
 * own address space, calling client functions by their fixed Wow.exe addresses; the replies
 * it builds are the CMSG 0x41B / 0x41D / 0x41C packets.
 *
 * This is a 32-bit x86 machine (Unicorn) that runs that delivered code the way the official
 * client does: the client's executable image is mapped at its real base, so the absolute
 * addresses the code carries resolve, and the few client functions the code calls are
 * bridged to this client's implementations. Nothing here invents a result: the delivered
 * code itself decides what to write, and the bytes it writes are what goes on the wire.
 *
 * Built for macOS/arm64, where the x86 code cannot run natively.
 *
 * Layout, mirroring the official process:
 *  - Wow.exe mapped at 0x00400000 (the framework's globals live far into its address space,
 *    e.g. 0x00DD0Fxx, and are writable).
 *  - VirtualAlloc regions from 0x03CD0000 with the Windows 64 KiB allocation granularity: the
 *    framework buffer, then every handler buffer the install code allocates (the 0x41B one is
 *    the token the realm is told).
 *  - The process heap (SMem, operator new) from 0x04000000.
 *  - The machine's own ClientServices handler table: what the delivered code registered with
 *    SetMessageHandler, so each message reaches the handler and param the framework chose.
 */

// A native client database serialized into the original 32-bit record layout. String fields
// carry offsets into `strings` (UINT32_MAX means nullptr), so host pointers never enter the
// guest. The delivered parser edits real rows; updated rows are translated back for native use.
struct BuckDatabase {
    bool rle = false;
    int32_t numRecords = 0;
    int32_t minID = 0;
    int32_t maxID = 0;
    std::vector<int32_t> ids;
    uint32_t recordSize = 0;
    std::vector<uint8_t> records;
    std::vector<uint8_t> strings;
    std::vector<uint32_t> stringFields;
};

class BuckCodeServices {
public:
    virtual ~BuckCodeServices() {}

    // The client's own executable, laid out the way the Windows loader maps it, and its base.
    // The delivered code reads framework globals inside it (0x00DD0Fxx) and the loader-resolved
    // import slots (0x009DFxxx); without it the machine still runs but those reads see zeroes.
    // False when no executable is available.
    virtual bool ClientImage(const uint8_t*& data, uint32_t& size, uint32_t& base) = 0;

    // Diagnostics line.
    virtual void Log(const char* line) = 0;

    // The client executable's path as the Windows client sees it (the module scan reports it).
    virtual const char* ClientPath() = 0;

    // FrameScript::Execute: the client's Lua entry point. Delivered code can reach it through
    // the client image the same way Warden modules do.
    virtual void RunLua(const char* source, const char* filename, const char* taint) = 0;

    // The client database object the official client keeps at `address` (a WowClientDB global
    // in Wow.exe's data section, e.g. 0x00AD4160 g_mapDB), as this client has it loaded. The
    // content Warmane delivers with SMSG 0x41D edits these databases by address. False when
    // the address is not a database this client knows.
    virtual bool ClientDatabase(uint32_t address, BuckDatabase& database) {
        (void)address;
        (void)database;
        return false;
    }

    // Applies the rows the delivered content parser actually changed to the native database.
    // Unknown databases or layouts fail explicitly; never substitute a successful check result.
    virtual bool CommitClientDatabase(uint32_t address, const BuckDatabase& database) {
        (void)address;
        (void)database;
        return false;
    }

    // SFile::IsTrial (0x00422140): the call the 0x41D content hook wraps at the start of
    // ClientInitializeGame.
    virtual int32_t IsTrial() {
        return 0;
    }
};

class BuckCodeEmu {
public:
    // A client call site the delivered code can redirect: the official code at these addresses
    // is `call rel32`, and the install segments rewrite the displacement to reach their own
    // hooks. The client runs its native implementation; it asks the machine at the same point
    // whether the delivered code took the call over.
    static constexpr uint32_t CALL_CLIENT_INITIALIZE_GAME = 0x00405543; // call SFile::IsTrial
    static constexpr uint32_t CALL_FRAMESCRIPT_EXECUTE_FILE = 0x008190A4; // call FrameScript_ExecuteBuffer

    explicit BuckCodeEmu(BuckCodeServices* services);
    ~BuckCodeEmu();

    BuckCodeEmu(const BuckCodeEmu&) = delete;
    BuckCodeEmu& operator=(const BuckCodeEmu&) = delete;

    // SMSG 0x2E9: the framework the server plants before the first 0x4B8. On the official client
    // the message overruns the battleground position arrays (Packet_MSG_BATTLEGROUND_PLAYER_-
    // POSITIONS, 0x0054B3F0) into a stub that allocates the framework buffer, records its base
    // at 0x00DD0FFC and registers the 0x4B8 dispatcher at base + 0x20 (param base + 0x20). The
    // machine reproduces those GUID/XY writes only in guest memory, then executes the actual
    // bootstrap and dispatcher. The delivered code chooses the buffers, globals and handlers.
    // Idempotent: a later 0x2E9 keeps the existing framework.
    bool InstallFramework(const uint8_t* payload, uint32_t size);

    // Delivers one message to the handler the delivered code registered for `opcode`, exactly
    // as the client's message dispatch calls it: handler(param, opcode, time, message) with the
    // message positioned past its opcode and EAX holding the handler address. Every packet the
    // handler sends through ClientServices::Send2 is returned in order (opcode first). False when
    // nothing is registered for the opcode or the handler faults.
    bool Dispatch(uint32_t opcode, uint32_t time, const uint8_t* payload, uint32_t size,
        std::vector<std::vector<uint8_t>>& sent);

    // Whether the delivered code registered a handler for the opcode, and with which param.
    bool HandlerFor(uint32_t opcode, uint32_t& handler, uint32_t& param) const;

    // Runs the client call at `site` the way the official client would after the delivered code
    // rewrote it: when the call still reaches its original target this returns false and the
    // caller continues natively; otherwise the delivered hook runs with `arguments` on the stack
    // and `result` receives EAX. A hook that ends by jumping into the original function's body
    // (`resume`, past its prologue) stops there, and the caller runs the original itself.
    bool IsCallHooked(uint32_t site);
    bool PatchScriptBuffer(void* buffer, uint32_t bytes, const char* chunkname);
    bool RunHookedCall(uint32_t site, const uint32_t* arguments, uint32_t argumentCount, uint32_t resume,
        uint32_t& result);

    // Copies bytes between this process and the machine, for hooked calls whose arguments point
    // at client data (a script buffer the hook edits in place). AllocateGuest returns a scratch
    // area in the machine's heap.
    uint32_t AllocateGuest(uint32_t size);
    bool FreeGuest(uint32_t address);
    bool WriteGuest(uint32_t address, const void* data, uint32_t size);
    bool ReadGuest(uint32_t address, void* data, uint32_t size);

    // Decodes one SMSG 0x41B challenge (the payload the realm sends, without the opcode) with
    // the token this client reported in its CMSG 0x41B request, executes the delivered code as
    // the official handler does, and returns the reply the client would send (the CMSG 0x41B
    // payload, opcode first). False when the challenge does not decode or the code faults.
    bool RunChallenge(const uint8_t* payload, uint32_t size, uint32_t token, uint32_t key,
        std::vector<uint8_t>& response, uint32_t& nextKey);

    // Answers a challenge by running the server's own 0x41B handler (the segment the 0x4B8
    // install code registered), exactly as the client's message dispatch calls it: the handler
    // sits at the token the server knows, its key argument is that token, and the message store
    // is positioned past the opcode and the challenge header. The handler decodes the payload,
    // runs the delivered code and sends its reply through the bridged Send2, which is captured
    // as the response. This is what the official client does; no framing is reconstructed here.
    // nextKey receives the param the handler registered for the following message, when it did.
    bool RunHandlerChallenge(const uint8_t* handler, uint32_t handlerSize, const uint8_t* payload,
        uint32_t size, uint32_t token, uint32_t key, std::vector<uint8_t>& response, uint32_t& nextKey);

    // Runs a delivered data handler (the 0x41C/0x41D segments) with a message positioned like
    // the client's dispatch: the handler may patch or write client globals. No reply is expected.
    // The 0x41D handler also installs a hook into the client image (a relative branch at
    // 0x00405544); hookOffset runs that hook afterwards, which is what parses the decompressed
    // table the 84-byte challenge reads. Zero skips it.
    bool RunDataHandler(const uint8_t* handler, uint32_t handlerSize, const uint8_t* payload,
        uint32_t size, uint32_t token, uint32_t hookOffset = 0);

    // The ten dwords the framework keeps at 0x00DD0FD8 (its buffers, handler addresses and the
    // content state); the 84-byte challenge copies them into its reply. Diagnostics only.
    bool ReadFrameworkState(uint32_t values[10]);

    // Decodes a challenge without running it: header, code length and the decoded message.
    // Used by diagnostics to compare against the official client's traffic.
    static bool DecodeChallenge(const uint8_t* payload, uint32_t size, uint32_t token, std::vector<uint8_t>& decoded);

    // 0x4B9 is [seed][NUL-terminated Lua chunks], not one string. The delivered handler keeps
    // its rolling key across chunk terminators and executes each chunk in order. Reject a
    // truncated final chunk before executing any of the packet.
    static bool DecodeLuaScripts(const uint8_t* payload, uint32_t size, std::vector<std::string>& scripts);

private:
    struct Impl;
    Impl* m_impl;
};

#endif
