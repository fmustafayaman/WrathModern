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
 *  - The handler buffer the realm was given as token, with the delivered code copied to
 *    token + 0x154 exactly where the official handler copies it.
 *  - A guest CDataStore carrying the message (the delivered code reads it through ESI).
 *  - A guest WDataStore the delivered code fills through EDI.
 */

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
};

class BuckCodeEmu {
public:
    explicit BuckCodeEmu(BuckCodeServices* services);
    ~BuckCodeEmu();

    BuckCodeEmu(const BuckCodeEmu&) = delete;
    BuckCodeEmu& operator=(const BuckCodeEmu&) = delete;

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
