#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "Engine/Source/Os/Mac/BuckCodeEmu.h"
#include "Engine/Source/Os/Mac/ClientImageRuntime.h"

#include <unicorn/unicorn.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <sys/statvfs.h>

/*
 * Warmane's server-delivered x86 code runner. See BuckCodeEmu.h for what this is and why.
 *
 * The delivered code is 32-bit x86 that calls the 12340 client by absolute address. The client
 * functions it calls are emulated here with this client's own semantics (the decompiled
 * implementations), against guest objects; everything else (the code's own arithmetic, the
 * values it chooses to send) executes as delivered.
 *
 * The memory layout mirrors the official process closely enough that the addresses the code
 * carries mean the same thing:
 *  0x00400000 + imageSize  the mapped Wow.exe
 *  0x03CD0000              VirtualAlloc regions, 64 KiB granular like Windows: the framework
 *                          buffer first, then the handler buffers the install code allocates
 *  0x04000000              guest heap (SMem, operator new); large enough for the client
 *                          databases the 0x41D content edits (Spell alone is ~34 MiB)
 *  0x0F000000              guest stack
 *  0x0F800000              the message and the reply store objects
 *  0x0FFFFFF0              stop sentinel
 * The handler buffer the realm knows as the token is used directly (the official handler
 * copies the delivered code to token + 0x154 and calls it there).
 */

namespace {

constexpr uint32_t CLIENT_BASE = 0x00400000;
constexpr uint32_t MACHINE_BASE = 0x00400000;
constexpr uint32_t MACHINE_SIZE = 0x10000000; // 256 MiB, holds the image and every guest area

// The official process's framework buffer sits at 0x03CD0000 and the handler buffers follow it
// in 64 KiB steps (0x03D00000, 0x03D10000, 0x03D20000); Windows hands VirtualAlloc(NULL, ...)
// the next free allocation-granularity block.
constexpr uint32_t VIRTUAL_BASE = 0x03CD0000;
constexpr uint32_t VIRTUAL_LIMIT = 0x04000000;
constexpr uint32_t VIRTUAL_GRANULARITY = 0x10000;

constexpr uint32_t HEAP_BASE = 0x04000000;
constexpr uint32_t HEAP_LIMIT = 0x0F000000;

constexpr uint32_t STACK_BASE = 0x0F000000;
constexpr uint32_t STACK_SIZE = 0x00080000;
constexpr uint32_t STACK_TOP = STACK_BASE + STACK_SIZE - 0x40;

constexpr uint32_t MESSAGE_OBJECT = 0x0F800000;
constexpr uint32_t REPLY_OBJECT = 0x0F800100;
constexpr uint32_t CODE_FALLBACK = 0x0F900000;
constexpr uint32_t STOP_ADDRESS = 0x0FFFFFF0;

// The loader-resolved import slots inside the client image. The installed slots are made to
// point at stubs inside the machine; the code's `call dword ptr [slot]` then lands here.
constexpr uint32_t IMPORT_SLOT_VIRTUAL_ALLOC = 0x009DF238;
constexpr uint32_t IMPORT_SLOT_VIRTUAL_PROTECT = 0x009DF28C;
constexpr uint32_t STUB_VIRTUAL_ALLOC = 0x0FE00000;
constexpr uint32_t STUB_VIRTUAL_PROTECT = 0x0FE00010;
constexpr uint32_t STUB_LOAD_LIBRARY = 0x0FE00020;
constexpr uint32_t STUB_FREE_LIBRARY = 0x0FE00030;
constexpr uint32_t STUB_REG_OPEN_KEY = 0x0FE00040;
constexpr uint32_t STUB_REG_QUERY_VALUE = 0x0FE00050;
constexpr uint32_t STUB_REG_CLOSE_KEY = 0x0FE00060;
constexpr uint32_t STUB_DISK_FREE_SPACE = 0x0FE00070;
constexpr uint32_t STUB_GET_PROC_ADDRESS = 0x0FE00080;
constexpr uint32_t STUB_GET_CURRENT_HW_PROFILE = 0x0FE00090;
constexpr uint32_t STUB_IS_DEBUGGER_PRESENT = 0x0FE000A0;
constexpr uint32_t STUB_GET_CURRENT_PROCESS = 0x0FE000B0;
constexpr uint32_t STUB_FLUSH_INSTRUCTION_CACHE = 0x0FE000C0;
constexpr uint32_t STUB_CLOSE_HANDLE = 0x0FE000D0;
constexpr uint32_t STUB_GET_CURRENT_THREAD_ID = 0x0FE000E0;
constexpr uint32_t STUB_OPEN_THREAD = 0x0FE000F0;
constexpr uint32_t STUB_CREATE_THREAD = 0x0FE00100;
constexpr uint32_t STUB_WAIT_FOR_SINGLE_OBJECT = 0x0FE00110;
constexpr uint32_t STUB_GET_THREAD_CONTEXT = 0x0FE00120;

constexpr uint32_t IAT_LOAD_LIBRARY = 0x009DF248;
constexpr uint32_t IAT_FREE_LIBRARY = 0x009DF250;
constexpr uint32_t IAT_REG_OPEN_KEY = 0x009DF00C;
constexpr uint32_t IAT_REG_QUERY_VALUE = 0x009DF010;
constexpr uint32_t IAT_REG_CLOSE_KEY = 0x009DF008;
constexpr uint32_t IAT_DISK_FREE_SPACE = 0x009DF388;
constexpr uint32_t CLIENT_GLOBAL_GET_PROC_ADDRESS = 0x00B2ED98;
constexpr uint32_t IAT_CLOSE_HANDLE = 0x009DF124;
// The 288-byte capture resolves these slots by XOR. Their raw file values are import-name
// RVAs, not callable client addresses (0x6B3856 is GetCurrentThreadId's unresolved name RVA).
constexpr uint32_t IAT_GET_CURRENT_THREAD_ID = 0x009DF218;
constexpr uint32_t IAT_OPEN_THREAD = 0x009DF2B4;
constexpr uint32_t IAT_CREATE_THREAD = 0x009DF338;
constexpr uint32_t IAT_WAIT_FOR_SINGLE_OBJECT = 0x009DF2E0;
constexpr uint32_t IAT_GET_THREAD_CONTEXT = 0x009DF2BC;

// The client functions the delivered code calls, by their 12340 addresses.
constexpr uint32_t CLIENT_STORE_GET_INT = 0x00401170;
constexpr uint32_t CLIENT_OS_GET_PHYSICAL_MEMORY = 0x0086B4C0;
constexpr uint32_t CLIENT_OS_GET_NUMBER_OF_PROCESSORS = 0x0086B240;
constexpr uint32_t CLIENT_OS_GET_COMPUTER_NAME = 0x0086B480;
constexpr uint32_t CLIENT_OS_GET_USER_NAME = 0x0086B4A0;

// OsFindTextSectionBounds (0x0086B510) caches the .text range it reads from the PE header in
// these two globals; OsValidateFunctionPointer (0x0086B5A0) compares every function pointer
// against them and re-derives them only while they are still zero. The official client runs
// that during startup. This machine never executes Wow.exe's own code, so the same section
// table is parsed here and the same cached values are stored (see SeedClientGlobals).
constexpr uint32_t CLIENT_TEXT_START_GLOBAL = 0x00D415B8;
constexpr uint32_t CLIENT_TEXT_END_GLOBAL = 0x00D415BC;
constexpr uint32_t CLIENT_OS_FIND_TEXT_SECTION_BOUNDS = 0x0086B510;
constexpr uint32_t CLIENT_OS_VALIDATE_FUNCTION_POINTER = 0x0086B5A0;
constexpr uint32_t CLIENT_FIND_DISPLAY_DEVICE = 0x00689EA0;
constexpr uint32_t CLIENT_TOOLHELP_SNAPSHOT = 0x008A131C;
constexpr uint32_t CLIENT_MODULE32_FIRST = 0x008A1310;
constexpr uint32_t CLIENT_MODULE32_NEXT = 0x008A1316;
constexpr uint32_t CLIENT_WDATASTORE_CTOR = 0x00403EC0;
constexpr uint32_t CLIENT_WDATASTORE_DTOR = 0x00403F10;
constexpr uint32_t CLIENT_STORE_PUT8 = 0x0047AFE0;
constexpr uint32_t CLIENT_STORE_PUT32 = 0x0047B0A0;
constexpr uint32_t CLIENT_STORE_PUT64 = 0x0047B100;
constexpr uint32_t CLIENT_STORE_PUT_ARRAY = 0x0047B1C0;
constexpr uint32_t CLIENT_STORE_PUT_STRING = 0x0047B300;
constexpr uint32_t CLIENT_STORE_GET_ARRAY = 0x0047B560;
constexpr uint32_t CLIENT_STORE_GET_IN_SITU = 0x0047B6B0;
constexpr uint32_t CLIENT_STORE_GET_BYTE = 0x00464560;
constexpr uint32_t CLIENT_MEMSET = 0x0040BB80;
constexpr uint32_t CLIENT_MEMCPY = 0x00414760;
constexpr uint32_t CLIENT_STRLEN = 0x00412770;
constexpr uint32_t CLIENT_FRAME_SCRIPT_EXECUTE = 0x00819210;
constexpr uint32_t CLIENT_OPERATOR_NEW = 0x00415074;
constexpr uint32_t CLIENT_OPERATOR_DELETE = 0x00412FC7;
constexpr uint32_t CLIENT_SMEM_ALLOC = 0x0076E540;
constexpr uint32_t CLIENT_SMEM_REALLOC = 0x0076E5E0;
constexpr uint32_t CLIENT_ZLIB_UNCOMPRESS = 0x00778180;
constexpr uint32_t CLIENT_SEND2 = 0x006B0B50;
constexpr uint32_t CLIENT_SET_MESSAGE_HANDLER = 0x006B0B80;
constexpr uint32_t CLIENT_STORE_GET32 = 0x0047B3C0;  // CDataStore::Get(uint32_t&)
constexpr uint32_t CLIENT_SMEM_FREE = 0x0076E5A0;
constexpr uint32_t CLIENT_SFILE_IS_TRIAL = 0x00422140;
constexpr uint32_t CLIENT_STRCMP = 0x0040F9D0;      // CRT strcmp
constexpr uint32_t CLIENT_STRSTR = 0x0088BCA0;      // CRT strstr
constexpr uint32_t CLIENT_MEMMOVE = 0x0040CB10;     // CRT memcpy/memmove (overlap-aware)

// The framework's state, ten dwords the install segments and handlers keep in Wow.exe's data
// section (identified from the delivered code; the official values are from its 84-byte reply):
//  +0x00 0x00DD0FD8  the 0x4B9 install's FrameXML patch list (operator new)
//  +0x04 0x00DD0FDC  the 0x41B handler buffer: the token the realm is told
//  +0x08 0x00DD0FE0  the 0x41C data version, sent back in the next 0x41C request
//  +0x0C 0x00DD0FE4  the 0x4B9 handler buffer
//  +0x10 0x00DD0FE8  the decompressed 0x41D content, freed once ClientInitializeGame parsed it
//  +0x14 0x00DD0FEC  the content's string pool (SMemAlloc)
//  +0x18 0x00DD0FF0  the content version, sent back in the next 0x41D request
//  +0x1C 0x00DD0FF4  the 0x41D handler buffer
//  +0x20 0x00DD0FF8  the 0x41C handler buffer
//  +0x24 0x00DD0FFC  the framework buffer the 0x2E9 stub allocated
constexpr uint32_t FRAMEWORK_STATE = 0x00DD0FD8;
constexpr uint32_t FRAMEWORK_CONTENT = 0x00DD0FE8;
constexpr uint32_t FRAMEWORK_BASE_GLOBAL = 0x00DD0FFC;
constexpr uint32_t FRAMEWORK_OPCODE = 0x4B8;
constexpr uint32_t FRAMEWORK_DISPATCHER_OFFSET = 0x20;
// The dispatcher copies each segment to its param + 0x3E (base + 0x5E) and calls it there.
constexpr uint32_t FRAMEWORK_SEGMENT_OFFSET = 0x3E;

// The content stream the 0x41D hook parses (decoded from the delivered parser at handler +
// 0x16C): [version] then commands until the end:
//  0 [size][bytes]                       the string pool
//  1 [count][database]*count             reset these databases when the version changed
//  2 [database][recordSize][count][maxID][minID][records]   replace a database
//  3 [database][recordSize][count][stringFields][fieldOffset]*stringFields
//    ([id][recordSize - 4 bytes])*count  insert or overwrite records by ID
//  4 [database][size][bytes]             overwrite the record block
constexpr uint32_t CONTENT_STRINGS = 0;
constexpr uint32_t CONTENT_RESET = 1;
constexpr uint32_t CONTENT_REPLACE = 2;
constexpr uint32_t CONTENT_UPSERT = 3;
constexpr uint32_t CONTENT_OVERWRITE = 4;

// WowClientDB fields (WoW/Source/DB/WowClientDB.h, 32-bit layout). WowClientDB<T> keeps its
// records at +0x1C and the by-ID index at +0x20; WowClientDB_RLE<T> keeps the row data at
// +0x18, the record pointers at +0x1C and the by-ID index right after them (+0x20).
constexpr uint32_t DB_LOADED = 0x04;
constexpr uint32_t DB_NUM_RECORDS = 0x08;
constexpr uint32_t DB_MAX_ID = 0x0C;
constexpr uint32_t DB_MIN_ID = 0x10;
constexpr uint32_t DB_RLE_DATA = 0x18;
constexpr uint32_t DB_RECORDS = 0x1C;
constexpr uint32_t DB_RECORDS_BY_ID = 0x20;

// The reply store's fixed preamble: the handler builds CMSG 0x41B with this id and constant
// before the delivered code adds its answer (see the decompiled 0x05660000 handler).
constexpr uint32_t CHALLENGE_OPCODE = 0x41B;
constexpr uint32_t CHALLENGE_MAGIC = 0xD0BE63D1;

// CDataStore field offsets (Engine/Source/Base/CDataStore.h).
constexpr uint32_t STORE_DATA = 0x04;
constexpr uint32_t STORE_BASE = 0x08;
constexpr uint32_t STORE_ALLOC = 0x0C;
constexpr uint32_t STORE_SIZE = 0x10;
constexpr uint32_t STORE_READ = 0x14;
// WDataStore adds these (WoW/Source/WowServices/WDataStore.h).
constexpr uint32_t STORE_BUFFER_OBJECT = 0x18;
constexpr uint32_t STORE_CHUNK_SIZE = 0x20;

uint32_t RotateRight(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
}

// Toolhelp reports only PE images actually mapped into this 32-bit guest. Native Mach-O
// libraries are outside that address space; historical Wine DLL inventories are not evidence
// that those DLLs are loaded here. See FillModuleEntry.

}  // namespace

struct BuckCodeEmu::Impl {
    BuckCodeServices* services = nullptr;
    uc_engine* machine = nullptr;
    uint32_t heapNext = HEAP_BASE;
    uint32_t clientBase = CLIENT_BASE;
    uint32_t clientSize = 0;
    bool faulted = false;
    std::string faultReason;

    // The most recent reply store the delivered code built, as it will go on the wire.
    std::vector<uint8_t> replyBuffer;

    // The last packet the delivered code sent through ClientServices::Send2, if any.
    std::vector<uint8_t> sentPacket;
    bool sentPacketValid = false;

    // Every packet sent through Send2 during the current run, in order.
    std::vector<std::vector<uint8_t>> sentPackets;

    // The last SetMessageHandler registration the delivered code made, so the caller can carry
    // the handler's param (the next message's key) forward exactly as the framework does.
    bool lastRegistrationValid = false;
    uint32_t lastRegistrationOpcode = 0;
    uint32_t lastRegistrationParam = 0;

    // The machine's ClientServices handler table: opcode -> handler and param, as the delivered
    // code registered them (the 0x41B handler re-registers itself with each challenge's nonce).
    struct Registration {
        uint32_t handler;
        uint32_t param;
    };
    std::map<uint32_t, Registration> handlers;

    // Both the bootstrap and dispatcher execute the delivered x86. Only the reconstructed
    // bootstrap may execute in the client's data section; normal client code needs a bridge.
    uint32_t frameworkBase = 0;
    bool bootstrapActive = false;

    uint32_t virtualNext = VIRTUAL_BASE;

    // SMem/operator new blocks: requested size and capacity. Large blocks keep headroom, so a
    // record-by-record SMemReAlloc of a 34 MiB database does not copy it every time.
    struct Allocation {
        uint32_t size;
        uint32_t capacity;
    };
    std::map<uint32_t, Allocation> allocations;
    std::multimap<uint32_t, uint32_t> freeBlocks;

    // Logical Windows threads execute on one Unicorn CPU, with separate saved register
    // contexts/stacks and shared guest memory. No native thread or captured CONTEXT is used.
    using ThreadClock = std::chrono::steady_clock;
    struct GuestThread {
        uc_context* context = nullptr;
        uint32_t start = 0;
        uint32_t parameter = 0;
        uint32_t stack = 0;
        uint32_t stackSize = 0;
        uint32_t waitFor = 0;
        uint32_t exitCode = 0;
        bool finished = false;
    };
    struct ThreadHandle {
        uint32_t id;
        uint32_t access;
    };
    std::map<uint32_t, GuestThread> threads;
    std::map<uint32_t, ThreadHandle> threadHandles;
    uc_context* initialThreadContext = nullptr;
    uint32_t currentThreadId = 1;
    uint32_t nextThreadId = 2;
    uint32_t nextThreadHandle = 0x03001000;
    bool threadYielded = false;

    bool BootThreads();
    void DestroyThreads();
    void ReapThreads();
    uint32_t OpenGuestThread(uint32_t access, uint32_t id);
    uint32_t CreateGuestThread(const uint32_t* arguments);
    bool ReadGuestThreadContext(uint32_t handle, uint32_t buffer);
    bool WaitForGuestThread(uint32_t handle, uint32_t timeout, uint32_t& result);
    uc_err RunGuest(uint32_t address, uint32_t stopAt = 0);

    // Real native DBC rows, translated to the guest's uncompressed 32-bit layout.
    std::set<uint32_t> mirroredDatabases;
    struct ContentDatabase {
        uint32_t stride = 0;
        std::vector<uint32_t> ids;
        std::vector<uint32_t> stringFields;
    };
    std::map<uint32_t, ContentDatabase> contentDatabases;

    bool Boot();
    void Teardown();
    void Fail(const char* reason);

    void Log(const char* format, ...) __attribute__((format(printf, 2, 3)));

    bool ReadMemory(uint32_t address, void* out, uint32_t size) const;
    bool WriteMemory(uint32_t address, const void* in, uint32_t size) const;
    bool ReadWord(uint32_t address, uint32_t& value) const;
    bool WriteWord(uint32_t address, uint32_t value) const;
    std::string ReadGuestString(uint32_t address, uint32_t capacity = 4096);

    uint32_t HeapAlloc(uint32_t size, uint32_t alignment = 16);
    uint32_t VirtualAllocate(uint32_t size);
    uint32_t TrackedAlloc(uint32_t size);
    bool TrackedFree(uint32_t block);
    uint32_t TrackedReAlloc(uint32_t block, uint32_t size);

    // CDataStore / WDataStore semantics on guest objects.
    void StoreConstruct(uint32_t store);
    uint32_t StoreGetInt(uint32_t store);
    void StorePutBytes(uint32_t store, const void* data, uint32_t size);
    uint32_t StorePut(uint32_t store, uint32_t value, uint32_t width);
    void StoreGetArray(uint32_t store, uint32_t destination, uint32_t count);

    // Calls guest code at `address` with `stackArguments` pushed right to left (cdecl order),
    // ECX and EAX as given, and a return address that stops the machine; `eax` receives the
    // result. `stopAt` (when non-zero) is a second address that ends the run, for hooks that
    // leave by jumping into the original function. False when the code faults.
    bool RunFunction(uint32_t address, const uint32_t* stackArguments, uint32_t argumentCount, uint32_t ecx, uint32_t& eax,
        uint32_t stopAt = 0);
    uint32_t extraStop = 0;

    // The message store as the client's dispatch hands it to a handler: opcode in front and
    // already read, the payload untouched.
    void BuildDispatchMessage(uint32_t opcode, const uint8_t* payload, uint32_t size);

    // Mirrors the client databases the 0x41D content is about to edit (see MirrorDatabase).
    bool PrepareContentDatabases();
    bool CommitContentDatabases();
    bool MirrorDatabase(uint32_t address, uint32_t recordSize);

    bool HandleClientCall(uint32_t address);
    bool FillModuleEntry(uint32_t entry, uint32_t index);
    void SeedClientGlobals(const uint8_t* image, uint32_t size);
    void HandleInvalidMemory(uc_mem_type type, uint64_t address, int size);

    // Module32First/Next walk position. CreateToolhelp32Snapshot resets it.
    uint32_t s_moduleIndex = 0;
    bool moduleSnapshotOpen = false;

    bool BuildMessage(const std::vector<uint8_t>& decoded, uint32_t readPosition);
    bool BuildReply(uint32_t& replySize);
};

// =====================================================================
// Boot / teardown
// =====================================================================

void BuckCodeEmu::Impl::Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (services) {
        services->Log(line);
    } else {
        fprintf(stderr, "%s\n", line);
    }
}

// The official client's startup path runs OsFindTextSectionBounds (0x0086B510), which reads the
// mapped PE header and caches the .text range in the two globals below. OsValidateFunctionPointer
// (0x0086B5A0) is the guard the server's later code probes: it reads those globals and only
// re-derives them when both are zero. The machine maps the client image but never executes the
// client's own startup, so do exactly what 0x0086B510 does, from the same section table.
void BuckCodeEmu::Impl::SeedClientGlobals(const uint8_t* image, uint32_t size) {
    uint32_t start = 0;
    uint32_t end = 0;

    if (!ClientImageRuntime::TextBounds(image, size, start, end)) {
        Log("client .text section could not be derived from this PE image");
        return;
    }

    WriteWord(CLIENT_TEXT_START_GLOBAL, start);
    WriteWord(CLIENT_TEXT_END_GLOBAL, end);
    Log("client .text %08x..%08x cached at %08x/%08x (OsFindTextSectionBounds %08x, "
        "OsValidateFunctionPointer %08x)", start, end, CLIENT_TEXT_START_GLOBAL, CLIENT_TEXT_END_GLOBAL,
        CLIENT_OS_FIND_TEXT_SECTION_BOUNDS, CLIENT_OS_VALIDATE_FUNCTION_POINTER);
}

bool BuckCodeEmu::Impl::Boot() {
    if (machine) {
        return true;
    }

    if (uc_open(UC_ARCH_X86, UC_MODE_32, &machine) != UC_ERR_OK) {
        Log("buck machine could not be created");
        return false;
    }

    if (uc_mem_map(machine, MACHINE_BASE, MACHINE_SIZE, UC_PROT_ALL) != UC_ERR_OK) {
        Log("buck machine memory could not be mapped");
        uc_close(machine);
        machine = nullptr;
        return false;
    }

    // The client executable, for the framework globals and import slots inside it. The code
    // the machine runs never executes these bytes; every client function it calls is bridged.
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t base = 0;

    if (services && services->ClientImage(data, size, base) && data && size && base == CLIENT_BASE
        && static_cast<uint64_t>(base) + size <= MACHINE_BASE + MACHINE_SIZE) {
        if (uc_mem_write(machine, base, data, size) == UC_ERR_OK) {
            clientBase = base;
            clientSize = size;
            Log("buck client image mapped at %08x (%u bytes)", base, size);
            SeedClientGlobals(data, size);
        } else {
            Log("buck client image could not be written at %08x", base);
        }
    } else {
        Log("buck client image unavailable; framework globals read as zero");
    }

    // Import slots point at the stubs, as the Windows loader resolves them in the real client.
    WriteWord(IMPORT_SLOT_VIRTUAL_ALLOC, STUB_VIRTUAL_ALLOC);
    WriteWord(IMPORT_SLOT_VIRTUAL_PROTECT, STUB_VIRTUAL_PROTECT);
    WriteWord(IAT_LOAD_LIBRARY, STUB_LOAD_LIBRARY);
    WriteWord(IAT_FREE_LIBRARY, STUB_FREE_LIBRARY);
    WriteWord(IAT_REG_OPEN_KEY, STUB_REG_OPEN_KEY);
    WriteWord(IAT_REG_QUERY_VALUE, STUB_REG_QUERY_VALUE);
    WriteWord(IAT_REG_CLOSE_KEY, STUB_REG_CLOSE_KEY);
    WriteWord(IAT_DISK_FREE_SPACE, STUB_DISK_FREE_SPACE);
    WriteWord(CLIENT_GLOBAL_GET_PROC_ADDRESS, STUB_GET_PROC_ADDRESS);
    WriteWord(IAT_CLOSE_HANDLE, STUB_CLOSE_HANDLE);
    WriteWord(0x009DF1B4, STUB_IS_DEBUGGER_PRESENT);
    WriteWord(0x009DF1C0, STUB_GET_CURRENT_PROCESS);
    WriteWord(0x009DF294, STUB_FLUSH_INSTRUCTION_CACHE);
    WriteWord(IAT_GET_CURRENT_THREAD_ID, STUB_GET_CURRENT_THREAD_ID);
    WriteWord(IAT_OPEN_THREAD, STUB_OPEN_THREAD);
    WriteWord(IAT_CREATE_THREAD, STUB_CREATE_THREAD);
    WriteWord(IAT_WAIT_FOR_SINGLE_OBJECT, STUB_WAIT_FOR_SINGLE_OBJECT);
    WriteWord(IAT_GET_THREAD_CONTEXT, STUB_GET_THREAD_CONTEXT);

    // Dispatch every instruction in the machine range through one code hook; the delivered code
    // executes few instructions, and the hook is what catches client calls and the sentinel.
    uc_hook codeHook;
    uc_hook_add(machine, &codeHook, UC_HOOK_CODE,
        reinterpret_cast<void*>(+[](uc_engine* machine, uint64_t address, uint32_t size, void* user) {
            (void)machine;
            (void)size;
            static_cast<BuckCodeEmu::Impl*>(user)->HandleClientCall(static_cast<uint32_t>(address));
        }),
        this, MACHINE_BASE, MACHINE_BASE + MACHINE_SIZE - 1);

    uc_hook memoryHook;
    uc_hook_add(machine, &memoryHook, UC_HOOK_MEM_INVALID,
        reinterpret_cast<void*>(+[](uc_engine* machine, uc_mem_type type, uint64_t address, int size, int64_t value, void* user) {
            (void)machine;
            (void)value;
            static_cast<BuckCodeEmu::Impl*>(user)->HandleInvalidMemory(type, address, size);
            return false;
        }),
        this, 1, 0);

    // CPUID: the delivered fingerprint code asks for the processor brand string. Wine on Apple
    // silicon reports the virtualised brand, and the official response length follows it; return
    // the same 48-byte padded brand instead of Unicorn's default Intel model.
    uc_hook cpuidHook;
    uc_hook_add(machine, &cpuidHook, UC_HOOK_INSN,
        reinterpret_cast<void*>(+[](uc_engine* machine, void* user) -> int {
            (void)user;
            uint32_t leaf = 0;
            uc_reg_read(machine, UC_X86_REG_EAX, &leaf);

            if (leaf == 0x80000000) {
                uint32_t maxLeaf = 0x80000004;
                uc_reg_write(machine, UC_X86_REG_EAX, &maxLeaf);
                return 1;
            }

            if (leaf >= 0x80000002 && leaf <= 0x80000004) {
                static const char brand[48] = "VirtualApple @ 2.50GHz ";
                uint32_t values[4] = {};
                memcpy(values, brand + (leaf - 0x80000002) * 16, sizeof(values));
                uc_reg_write(machine, UC_X86_REG_EAX, &values[0]);
                uc_reg_write(machine, UC_X86_REG_EBX, &values[1]);
                uc_reg_write(machine, UC_X86_REG_ECX, &values[2]);
                uc_reg_write(machine, UC_X86_REG_EDX, &values[3]);
                return 1;
            }

            return 0;
        }),
        this, 1, 0, UC_X86_INS_CPUID);

    if (!BootThreads()) {
        Log("buck guest thread contexts could not be initialized");
        Teardown();
        return false;
    }

    return true;
}

void BuckCodeEmu::Impl::Teardown() {
    DestroyThreads();
    if (machine) {
        uc_close(machine);
        machine = nullptr;
    }
}

// =====================================================================
// Memory helpers
// =====================================================================

bool BuckCodeEmu::Impl::ReadMemory(uint32_t address, void* out, uint32_t size) const {
    if (!size) {
        return true;
    }

    return uc_mem_read(machine, address, out, size) == UC_ERR_OK;
}

bool BuckCodeEmu::Impl::WriteMemory(uint32_t address, const void* in, uint32_t size) const {
    if (!size) {
        return true;
    }

    return uc_mem_write(machine, address, in, size) == UC_ERR_OK;
}

bool BuckCodeEmu::Impl::ReadWord(uint32_t address, uint32_t& value) const {
    return ReadMemory(address, &value, sizeof(value));
}

bool BuckCodeEmu::Impl::WriteWord(uint32_t address, uint32_t value) const {
    return WriteMemory(address, &value, sizeof(value));
}

std::string BuckCodeEmu::Impl::ReadGuestString(uint32_t address, uint32_t capacity) {
    std::string text;
    if (!address) return text;

    for (uint32_t i = 0; i < capacity; i++) {
        uint8_t byte = 0;

        if (!ReadMemory(address + i, &byte, 1)) {
            Fail("string read outside guest memory");
            return {};
        }

        if (!byte) return text;
        text += static_cast<char>(byte);
    }

    Fail("guest string exceeded the bounded read");
    return {};
}

uint32_t BuckCodeEmu::Impl::HeapAlloc(uint32_t size, uint32_t alignment) {
    uint64_t result = (static_cast<uint64_t>(heapNext) + alignment - 1) & ~uint64_t(alignment - 1);
    uint64_t end = (result + (size ? size : 1) + 15) & ~uint64_t(15);

    if (end > HEAP_LIMIT) {
        Fail("buck heap exhausted");
        return 0;
    }

    heapNext = static_cast<uint32_t>(end);
    return static_cast<uint32_t>(result);
}

uint32_t BuckCodeEmu::Impl::VirtualAllocate(uint32_t size) {
    uint64_t end = static_cast<uint64_t>(virtualNext)
        + ((static_cast<uint64_t>(size) + VIRTUAL_GRANULARITY - 1) & ~uint64_t(VIRTUAL_GRANULARITY - 1));

    if (!size || end > VIRTUAL_LIMIT) {
        Fail("buck VirtualAlloc region exhausted");
        return 0;
    }

    uint32_t result = virtualNext;
    virtualNext = static_cast<uint32_t>(end);
    return result;
}

// =====================================================================
// Guest CDataStore / WDataStore
// =====================================================================

void BuckCodeEmu::Impl::StoreConstruct(uint32_t store) {
    // The real WDataStore constructor initializes its own fields but leaves the unused dword at
    // +0x1C alone. The delivered 0x41B handler keeps its context pointer there across the
    // constructor call (it writes [ebp-8] before constructing the store at [ebp-0x24]), so
    // clearing that slot breaks the handler.
    WriteWord(store + 0x00, 0);
    WriteWord(store + STORE_DATA, 0);
    WriteWord(store + STORE_BASE, 0);
    WriteWord(store + STORE_ALLOC, 0);
    WriteWord(store + STORE_SIZE, 0);
    WriteWord(store + STORE_READ, 0);
    WriteWord(store + STORE_BUFFER_OBJECT, 0);
    WriteWord(store + STORE_CHUNK_SIZE, 256);
}

uint32_t BuckCodeEmu::Impl::StoreGetInt(uint32_t store) {
    uint32_t buffer = 0;
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t read = 0;
    ReadWord(store + STORE_DATA, buffer);
    ReadWord(store + STORE_BASE, base);
    ReadWord(store + STORE_SIZE, size);
    ReadWord(store + STORE_READ, read);

    uint32_t value = 0;

    // GetInt initializes its local result to zero; a failed FetchRead leaves it zero.
    // End-of-store is a normal CDataStore failure, not an invented successful read.
    if (read >= base && read <= size && size - read >= 4) {
        if (!buffer || !ReadMemory(buffer + read - base, &value, sizeof(value))) {
            Fail("CDataStore::GetInt source outside guest memory");
            return 0;
        }

        WriteWord(store + STORE_READ, read + 4);
    }

    static const bool trace = getenv("BUCK_TRACE") != nullptr;

    if (trace) {
        Log("buck GetInt store=%08x read=%u size=%u -> %08x", store, read, size, value);
    }

    return value;
}

void BuckCodeEmu::Impl::StorePutBytes(uint32_t store, const void* data, uint32_t size) {
    if (!size) {
        return;
    }

    uint32_t buffer = 0;
    uint32_t base = 0;
    uint32_t alloc = 0;
    uint32_t used = 0;
    ReadWord(store + STORE_DATA, buffer);
    ReadWord(store + STORE_BASE, base);
    ReadWord(store + STORE_ALLOC, alloc);
    ReadWord(store + STORE_SIZE, used);

    uint32_t needed = used - base + size;

    if (!buffer) {
        uint32_t newAlloc = needed > 256 ? (needed + 0xFF) & ~0xFFu : 256;
        buffer = HeapAlloc(newAlloc);
        base = 0;
        alloc = newAlloc;
        WriteWord(store + STORE_DATA, buffer);
        WriteWord(store + STORE_BASE, base);
        WriteWord(store + STORE_ALLOC, alloc);
    } else if (needed > alloc) {
        uint32_t newAlloc = (needed + 0xFF) & ~0xFFu;
        uint32_t newBuffer = HeapAlloc(newAlloc);
        std::vector<uint8_t> copy(used - base);
        ReadMemory(buffer, copy.data(), static_cast<uint32_t>(copy.size()));
        WriteMemory(newBuffer, copy.data(), static_cast<uint32_t>(copy.size()));
        buffer = newBuffer;
        alloc = newAlloc;
        WriteWord(store + STORE_DATA, buffer);
        WriteWord(store + STORE_ALLOC, alloc);
    }

    WriteMemory(buffer + used - base, data, size);
    WriteWord(store + STORE_SIZE, used + size);
}

uint32_t BuckCodeEmu::Impl::StorePut(uint32_t store, uint32_t value, uint32_t width) {
    uint8_t bytes[4];
    memcpy(bytes, &value, sizeof(bytes));
    StorePutBytes(store, bytes, width);
    return store;
}

void BuckCodeEmu::Impl::StoreGetArray(uint32_t store, uint32_t destination, uint32_t count) {
    uint32_t buffer = 0;
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t read = 0;
    ReadWord(store + STORE_DATA, buffer);
    ReadWord(store + STORE_BASE, base);
    ReadWord(store + STORE_SIZE, size);
    ReadWord(store + STORE_READ, read);

    uint32_t available = size > read ? size - read : 0;

    // The original GetArray streams until FetchRead fails and can consume fewer than count
    // bytes (CDataStore.cpp:399). The delivered 0x41B installer deliberately passes ECX's
    // stack-local pointer as count, so this is not a strict exact-length read.
    if (read < base || (count && (!buffer || !destination))) {
        Fail("CDataStore::GetArray has an invalid source/destination");
        return;
    }

    uint32_t bytes = count < available ? count : available;
    static const bool trace = getenv("BUCK_TRACE") != nullptr;

    if (trace) {
        Log("buck GetArray store=%08x dst=%08x count=%u read=%u size=%u -> %u bytes from %08x",
            store, destination, count, read, size, bytes, buffer + read - base);
    }

    if (bytes && buffer && destination) {
        std::vector<uint8_t> copy(bytes);
        if (!ReadMemory(buffer + read - base, copy.data(), bytes)
            || !WriteMemory(destination, copy.data(), bytes)) {
            Fail("CDataStore::GetArray transfer outside guest memory");
            return;
        }
        // The destination can be code the previous challenge already executed; Unicorn keeps
        // translations of guest writes made from a hook, so drop them or the old code runs.
        uc_ctl_remove_cache(machine, destination, destination + bytes);
    }

    WriteWord(store + STORE_READ, read + bytes);
}

// =====================================================================
// Client function calls
// =====================================================================

bool BuckCodeEmu::Impl::FillModuleEntry(uint32_t entry, uint32_t index) {
    if (index || !clientSize || !services) return false;
    const char* path = services->ClientPath();
    if (!path || !path[0]) return false;
    std::string fullPath(path);
    if (fullPath.size() > 259) fullPath.resize(259);
    const char* name = strrchr(fullPath.c_str(), '\\');
    name = name ? name + 1 : fullPath.c_str();
    WriteWord(entry + 0x14, clientBase);
    WriteWord(entry + 0x18, clientSize);
    WriteWord(entry + 0x1C, clientBase);
    WriteMemory(entry + 0x20, name, static_cast<uint32_t>(strlen(name)) + 1);
    WriteMemory(entry + 0x120, fullPath.c_str(), static_cast<uint32_t>(fullPath.size()) + 1);
    return true;
}

bool BuckCodeEmu::Impl::HandleClientCall(uint32_t address) {
    if (address == STOP_ADDRESS || (currentThreadId == 1 && extraStop && address == extraStop)) {
        uc_emu_stop(machine);
        return true;
    }

    uint32_t esp = 0;
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);

    // Return address plus up to eight stdcall arguments (CreateThread/RegQueryValueEx
    // each need six). A six-dword frame made arguments[5] read beyond the host array.
    uint32_t frame[9] = { 0 };
    ReadMemory(esp, frame, sizeof(frame));

    uint32_t ecx = 0;
    uc_reg_read(machine, UC_X86_REG_ECX, &ecx);

    uint32_t returnAddress = frame[0];
    uint32_t* arguments = &frame[1];
    uint32_t result = 0;

    static const bool traceCalls = getenv("BUCK_TRACE") != nullptr;

    if (traceCalls) {
        Log("client call %08x ecx=%08x args=%08x %08x %08x %08x", address, ecx, frame[1], frame[2], frame[3], frame[4]);
    }

    auto finish = [&](uint32_t extraStackBytes) {
        uint32_t newEsp = esp + 4 + extraStackBytes;
        uc_reg_write(machine, UC_X86_REG_EAX, &result);
        uc_reg_write(machine, UC_X86_REG_ESP, &newEsp);
        uc_reg_write(machine, UC_X86_REG_EIP, &returnAddress);
    };

    switch (address) {
        case 0x0054E220: { // CGBattlefieldInfo::ShutdownGame
            // This guest has no native battlefield score objects. Unregister the same stock
            // handlers as BattlefieldInfo.cpp; never run the native client's shutdown here.
            const uint32_t opcodes[] = { 0x23D, 0x2D4, 0x2E0, 0x2E8, 0x2E9, 0x2EC, 0x2ED,
                0x4C7, 0x14B, 0x4DE, 0x4E0, 0x4E4, 0x4E5, 0x4E6, 0x4E1, 0x4E8 };

            for (uint32_t opcode : opcodes) {
                handlers.erase(opcode);
            }

            finish(0);
            return true;
        }
        case CLIENT_STORE_GET32: {
            result = StoreGetInt(ecx);
            WriteWord(arguments[0], result);
            result = ecx;
            finish(4);
            return true;
        }
        case CLIENT_SFILE_IS_TRIAL: {
            result = services->IsTrial();
            finish(0);
            return true;
        }
        case CLIENT_STRCMP: {
            std::string first = ReadGuestString(arguments[0], 0x100000);
            std::string second = ReadGuestString(arguments[1], 0x100000);
            result = static_cast<uint32_t>(strcmp(first.c_str(), second.c_str()));
            finish(0);
            return true;
        }
        case CLIENT_STRSTR: {
            std::string text = ReadGuestString(arguments[0], 0x100000);
            std::string needle = ReadGuestString(arguments[1], 0x100000);
            size_t offset = text.find(needle);
            result = offset == std::string::npos ? 0 : arguments[0] + static_cast<uint32_t>(offset);
            finish(0);
            return true;
        }
        case CLIENT_STORE_GET_INT: {
            // CDataStore::GetInt: consumes the next dword and returns it. The 0x41B handler
            // reads the challenge nonce, then the code length, then the delivered code reads
            // the trailing answer dword with the same call.
            uint32_t read = 0;
            ReadWord(ecx + STORE_READ, read);
            result = StoreGetInt(ecx);
            // 0x401170 calls Get(uint32_t&) with its local at entry ESP-8. The latter leaves
            // ECX pointing there and EDX holding the pre-read cursor; preserve those observed
            // volatile registers as the delivered installer uses ECX immediately afterwards.
            uint32_t local = esp - 8;
            WriteWord(local, result);
            uc_reg_write(machine, UC_X86_REG_ECX, &local);
            uc_reg_write(machine, UC_X86_REG_EDX, &read);
            finish(0);
            return true;
        }
        case CLIENT_WDATASTORE_CTOR: {
            StoreConstruct(ecx);
            result = ecx;
            finish(0);
            return true;
        }
        case CLIENT_WDATASTORE_DTOR: {
            StoreConstruct(ecx);
            finish(0);
            return true;
        }
        case CLIENT_STORE_PUT8: {
            StorePut(ecx, arguments[0] & 0xFF, 1);
            result = ecx;
            finish(4); // __thiscall, the function is `ret 4`
            return true;
        }
        case CLIENT_STORE_PUT32: {
            StorePut(ecx, arguments[0], 4);
            result = ecx;
            finish(4);
            return true;
        }
        case CLIENT_STORE_PUT64: {
            StorePut(ecx, arguments[0], 4);
            StorePut(ecx, arguments[1], 4);
            result = ecx;
            finish(8);
            return true;
        }
        case CLIENT_STORE_PUT_ARRAY: {
            uint32_t data = arguments[0];
            uint32_t count = arguments[1];

            if (data && count) {
                std::vector<uint8_t> bytes(count);
                if (!ReadMemory(data, bytes.data(), count)) {
                    Fail("CDataStore::PutArray source outside guest memory");
                    return true;
                }

                StorePutBytes(ecx, bytes.data(), count);
            }

            result = ecx;
            finish(8);
            return true;
        }
        case CLIENT_STORE_PUT_STRING: {
            std::string text = ReadGuestString(arguments[0], 0x4000);
            StorePutBytes(ecx, text.c_str(), static_cast<uint32_t>(text.size()) + 1);
            result = ecx;
            finish(4);
            return true;
        }
        case CLIENT_STORE_GET_ARRAY: {
            StoreGetArray(ecx, arguments[0], arguments[1]);
            result = ecx;
            finish(8);
            return true;
        }
        case CLIENT_STORE_GET_BYTE: {
            // CDataStore::GetByte: consumes the next byte of the message and returns it. The
            // 0x4B9 handler uses the first payload byte as its rolling-XOR seed.
            uint32_t buffer = 0;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t read = 0;
            ReadWord(ecx + STORE_DATA, buffer);
            ReadWord(ecx + STORE_BASE, base);
            ReadWord(ecx + STORE_SIZE, size);
            ReadWord(ecx + STORE_READ, read);

            uint8_t byte = 0;

            if (read >= base && read < size) {
                if (!buffer || !ReadMemory(buffer + read - base, &byte, 1)) {
                    Fail("CDataStore::GetByte source outside guest memory");
                    return true;
                }

                WriteWord(ecx + STORE_READ, read + 1);
            }

            result = byte;
            finish(0);
            return true;
        }
        case CLIENT_STORE_GET_IN_SITU: {
            uint32_t buffer = 0;
            uint32_t base = 0;
            uint32_t read = 0;
            uint32_t size = 0;
            ReadWord(ecx + STORE_DATA, buffer);
            ReadWord(ecx + STORE_BASE, base);
            ReadWord(ecx + STORE_READ, read);
            ReadWord(ecx + STORE_SIZE, size);

            uint32_t available = size > read ? size - read : 0;

            if (!buffer || read < base || arguments[1] > available) {
                result = 0;
                finish(8);
                return true;
            }

            uint32_t bytes = arguments[1];

            if (arguments[0]) {
                WriteWord(arguments[0], buffer + read - base);
            }

            WriteWord(ecx + STORE_READ, read + bytes);
            result = 1;
            finish(8);
            return true;
        }
        case CLIENT_MEMSET: {
            uint32_t size = arguments[2];
            std::vector<uint8_t> bytes(size, static_cast<uint8_t>(arguments[1]));
            if (!WriteMemory(arguments[0], bytes.data(), size)) {
                Fail("memset destination outside guest memory");
                return true;
            }

            if (size) uc_ctl_remove_cache(machine, arguments[0], static_cast<uint64_t>(arguments[0]) + size);
            result = arguments[0];
            finish(0); // __cdecl, the caller cleans
            return true;
        }
        case CLIENT_MEMCPY:
        case CLIENT_MEMMOVE: {
            uint32_t size = arguments[2];
            std::vector<uint8_t> bytes(size);
            if (!ReadMemory(arguments[1], bytes.data(), size) || !WriteMemory(arguments[0], bytes.data(), size)) {
                Fail("memcpy/memmove transfer outside guest memory");
                return true;
            }

            if (size) uc_ctl_remove_cache(machine, arguments[0], static_cast<uint64_t>(arguments[0]) + size);
            result = arguments[0];
            finish(0);
            return true;
        }
        case CLIENT_STRLEN: {
            uint32_t length = 0;
            uint8_t byte = 0;

            while (ReadMemory(arguments[0] + length, &byte, 1) && byte && length < 0x10000) {
                length++;
            }

            result = length;
            finish(0);
            return true;
        }
        case CLIENT_OPERATOR_NEW: {
            result = TrackedAlloc(arguments[0]);
            finish(0);
            return true;
        }
        case CLIENT_OPERATOR_DELETE:
        case CLIENT_SMEM_FREE: {
            TrackedFree(arguments[0]);
            finish(address == CLIENT_SMEM_FREE ? 16 : 0);
            return true;
        }
        case CLIENT_SMEM_ALLOC: {
            result = TrackedAlloc(arguments[0]);
            finish(16); // STORMAPI (stdcall)
            return true;
        }
        case CLIENT_SMEM_REALLOC: {
            result = TrackedReAlloc(arguments[0], arguments[1]);
            finish(20); // STORMAPI (stdcall)
            return true;
        }
        case CLIENT_FRAME_SCRIPT_EXECUTE: {
            std::string source = ReadGuestString(arguments[0]);
            std::string filename = ReadGuestString(arguments[1], 256);
            std::string taint = ReadGuestString(arguments[2], 256);
            services->RunLua(source.c_str(), arguments[1] ? filename.c_str() : nullptr,
                arguments[2] ? taint.c_str() : nullptr);
            result = 0;
            finish(0); // __cdecl
            return true;
        }
        case CLIENT_ZLIB_UNCOMPRESS: {
            uint32_t destination = arguments[0];
            uint32_t destinationLengthAddress = arguments[1];
            uint32_t source = arguments[2];
            uint32_t sourceLength = arguments[3];

            uint32_t destinationLength = 0;
            ReadWord(destinationLengthAddress, destinationLength);

            std::vector<uint8_t> input(sourceLength);
            std::vector<uint8_t> output(destinationLength);
            ReadMemory(source, input.data(), sourceLength);

            uLongf outLength = destinationLength;
            int zresult = uncompress(output.data(), &outLength, input.data(), sourceLength);
            Log("zlib_uncompress dst=%08x dstLen=%u src=%08x srcLen=%u -> %d, out=%u", destination,
                destinationLength, source, sourceLength, zresult, static_cast<uint32_t>(outLength));

            if (zresult == Z_OK) {
                WriteMemory(destination, output.data(), static_cast<uint32_t>(outLength));
                WriteWord(destinationLengthAddress, static_cast<uint32_t>(outLength));

                // Diagnostic: the 0x41D payload is the realm's compressed data table. Keeping
                // the decompressed bytes lets the offline smoke test compare the handler's
                // output with the official client's 84-byte table check.
                FILE* dump = fopen("Logs/buck-41d-decompressed.bin", "wb");

                if (dump) {
                    fwrite(output.data(), 1, outLength, dump);
                    fclose(dump);
                }

                result = 0;
            } else {
                result = zresult;
            }

            finish(16); // STORMAPI (stdcall)
            return true;
        }
        case CLIENT_SEND2: {
            // The delivered install code builds packets and sends them; capture the payload so
            // diagnostics can show exactly what went out.
            uint32_t store = arguments[0];
            uint32_t size = 0;
            uint32_t read = 0;
            uint32_t buffer = 0;
            uint32_t base = 0;
            ReadWord(store + STORE_SIZE, size);
            ReadWord(store + STORE_READ, read);
            ReadWord(store + STORE_DATA, buffer);
            ReadWord(store + STORE_BASE, base);

            std::vector<uint8_t> packet(size - read);
            ReadMemory(buffer + read - base, packet.data(), static_cast<uint32_t>(packet.size()));

            std::string hex;

            for (uint32_t i = 0; i < packet.size() && i < 64; i++) {
                char byte[4];
                snprintf(byte, sizeof(byte), "%02x", packet[i]);
                hex += byte;
            }

            Log("delivered code sent a %u byte packet: %s%s", static_cast<uint32_t>(packet.size()), hex.c_str(),
                packet.size() > 64 ? "..." : "");
            sentPacket = packet;
            sentPacketValid = true;
            sentPackets.push_back(packet);
            result = 0;
            finish(0); // __cdecl, the caller pops the argument
            return true;
        }
        case CLIENT_SET_MESSAGE_HANDLER: {
            Log("delivered code registered a message handler: opcode %u handler %08x param %08x",
                arguments[0], arguments[1], arguments[2]);
            lastRegistrationValid = true;
            lastRegistrationOpcode = arguments[0];
            lastRegistrationParam = arguments[2];
            handlers[arguments[0]] = { arguments[1], arguments[2] };
            result = 0;
            finish(0); // __cdecl, the caller pops the arguments
            return true;
        }
        case STUB_IS_DEBUGGER_PRESENT: {
            // There is no guest Windows debugger attached to this Unicorn process.
            result = 0;
            finish(0);
            return true;
        }
        case STUB_GET_CURRENT_PROCESS: {
            result = 0xFFFFFFFF; // The Windows pseudo-handle for this guest process.
            finish(0);
            return true;
        }
        case STUB_FLUSH_INSTRUCTION_CACHE: {
            result = uc_ctl_remove_cache(machine, arguments[1],
                static_cast<uint64_t>(arguments[1]) + arguments[2]) == UC_ERR_OK;
            finish(12);
            return true;
        }
        case STUB_VIRTUAL_ALLOC: {
            uint32_t requested = arguments[0];
            uint32_t size = arguments[1];

            if (requested) {
                result = requested;
            } else {
                result = VirtualAllocate(size);
            }

            finish(16); // Windows API (stdcall)
            return true;
        }
        case STUB_VIRTUAL_PROTECT: {
            WriteWord(arguments[3], 0x40);
            result = 1;
            finish(16);
            return true;
        }
        case CLIENT_OS_GET_NUMBER_OF_PROCESSORS: {
            result = 10;
            finish(0);
            return true;
        }
        case CLIENT_OS_GET_PHYSICAL_MEMORY: {
            // 16 GiB, as the host reports; EDX:EAX carries the 64-bit value.
            uint64_t memory = 0x00000003FFFFF000ull;
            uint32_t edx = static_cast<uint32_t>(memory >> 32);
            result = static_cast<uint32_t>(memory);
            uc_reg_write(machine, UC_X86_REG_EDX, &edx);
            finish(0);
            return true;
        }
        case CLIENT_OS_GET_COMPUTER_NAME:
        case CLIENT_OS_GET_USER_NAME: {
            const char* name = "MAC";
            WriteMemory(arguments[0], name, 4);
            WriteWord(arguments[1], 4);
            result = 1;
            finish(0);
            return true;
        }
        case CLIENT_FIND_DISPLAY_DEVICE: {
            // DISPLAY_DEVICEA: cb, DeviceName[32], DeviceString[128], StateFlags, ...
            std::vector<uint8_t> device(0x1A8, 0);
            uint32_t cb = 0x1A8;
            memcpy(device.data(), &cb, 4);
            const char* displayName = "\\\\.\\DISPLAY1";
            memcpy(device.data() + 4, displayName, strlen(displayName) + 1);
            const char* adapter = "Apple M4";
            memcpy(device.data() + 0x24, adapter, strlen(adapter) + 1);
            uint32_t flags = 4;
            memcpy(device.data() + 0xA4, &flags, 4);
            WriteMemory(arguments[0], device.data(), static_cast<uint32_t>(device.size()));
            result = 1;
            finish(0);
            return true;
        }
        case STUB_LOAD_LIBRARY: {
            result = 0x03000000;
            finish(4);
            return true;
        }
        case STUB_FREE_LIBRARY: {
            result = 1;
            finish(4);
            return true;
        }
        case STUB_GET_PROC_ADDRESS: {
            std::string name = ReadGuestString(arguments[1], 128);
            result = name == "GetCurrentHwProfileA" ? STUB_GET_CURRENT_HW_PROFILE : 0;
            finish(8);
            return true;
        }
        case STUB_GET_CURRENT_HW_PROFILE: {
            // HW_PROFILE_INFOA: dwDockInfo, szHwProfileGuid[39], szHwProfileName[80].
            std::vector<uint8_t> profile(0x4 + 39 + 80, 0);
            const char* guid = "{12340001-1234-1234-1234-123456789ABC}";
            const char* profileName = "WrathModern";
            memcpy(profile.data() + 4, guid, strlen(guid) + 1);
            memcpy(profile.data() + 0x2B, profileName, strlen(profileName) + 1);
            WriteMemory(arguments[0], profile.data(), static_cast<uint32_t>(profile.size()));
            result = 0;
            finish(4);
            return true;
        }
        case STUB_REG_OPEN_KEY: {
            WriteWord(arguments[4], 0x03000100);
            result = 0;
            finish(20);
            return true;
        }
        case STUB_REG_QUERY_VALUE: {
            if (arguments[3]) {
                WriteWord(arguments[3], 1); // REG_SZ
            }
            if (arguments[4]) {
                WriteMemory(arguments[4], "2024", 4);
            }
            if (arguments[5]) {
                WriteWord(arguments[5], 4);
            }
            result = 0;
            finish(24);
            return true;
        }
        case STUB_REG_CLOSE_KEY: {
            result = 0;
            finish(4);
            return true;
        }
        case STUB_DISK_FREE_SPACE: {
            // Wow.exe IAT 0x9DF388 is GetDiskFreeSpaceExA, NOT GetSystemTimeAsFileTime.
            // The 660-byte code passes (NULL, NULL, &totalBytes, NULL), so a FILETIME here
            // reported an ever-changing ~134 PB disk. Query the real backing filesystem.
            std::string path = arguments[0] ? ReadGuestString(arguments[0]) : services->ClientPath();

            if (path.size() >= 2 && (path[0] == 'Z' || path[0] == 'z') && path[1] == ':') {
                path.erase(0, 2);
            }

            for (char& c : path) if (c == '\\') c = '/';
            struct statvfs info = {};
            result = !path.empty() && statvfs(path.c_str(), &info) == 0;

            if (result) {
                uint64_t unit = info.f_frsize ? info.f_frsize : info.f_bsize;
                uint64_t values[] = { info.f_bavail * unit, info.f_blocks * unit, info.f_bfree * unit };

                for (uint32_t i = 0; i < 3; i++) {
                    if (arguments[i + 1] && !WriteMemory(arguments[i + 1], &values[i], sizeof(uint64_t))) {
                        Fail("GetDiskFreeSpaceExA output is outside guest memory");
                        result = 0;
                        break;
                    }
                }

                Log("GetDiskFreeSpaceExA: total=%llu available=%llu free=%llu", values[1], values[0], values[2]);
            }

            finish(16);
            return true;
        }
        case STUB_GET_CURRENT_THREAD_ID: {
            result = currentThreadId;
            finish(0);
            return true;
        }
        case STUB_OPEN_THREAD: {
            result = OpenGuestThread(arguments[0], arguments[2]);
            finish(12);
            return true;
        }
        case STUB_CREATE_THREAD: {
            result = CreateGuestThread(arguments);
            finish(24);
            return true;
        }
        case STUB_GET_THREAD_CONTEXT: {
            result = ReadGuestThreadContext(arguments[0], arguments[1]);
            finish(8);
            return true;
        }
        case STUB_WAIT_FOR_SINGLE_OBJECT: {
            bool waiting = WaitForGuestThread(arguments[0], arguments[1], result);
            finish(8);

            if (waiting) {
                threadYielded = true;
                uc_emu_stop(machine);
            }

            return true;
        }
        case STUB_CLOSE_HANDLE: {
            auto thread = threadHandles.find(arguments[0]);

            if (thread != threadHandles.end()) {
                threadHandles.erase(thread);
                ReapThreads();
                result = 1;
            } else {
                result = moduleSnapshotOpen && arguments[0] == 0x03000300;
                if (result) moduleSnapshotOpen = false;
            }

            finish(4);
            return true;
        }
        case CLIENT_TOOLHELP_SNAPSHOT: {
            // These import thunks retain the Windows API's stdcall ABI. Enumerate the actual
            // guest image rather than a captured Wine module list.
            s_moduleIndex = 0;
            moduleSnapshotOpen = (arguments[0] & 0x18) && !arguments[1] && clientSize;
            result = moduleSnapshotOpen ? 0x03000300 : 0xFFFFFFFF;
            finish(8);
            return true;
        }
        case CLIENT_MODULE32_FIRST:
        case CLIENT_MODULE32_NEXT: {
            if (address == CLIENT_MODULE32_FIRST) s_moduleIndex = 0;
            result = moduleSnapshotOpen && arguments[0] == 0x03000300
                && FillModuleEntry(arguments[1], s_moduleIndex);
            if (result) s_moduleIndex++;
            finish(8);
            return true;
        }
        default:
            break;
    }

    if (bootstrapActive && address >= 0x00DD1000 && address < 0x00DD1080) {
        return true;
    }

    // Normal delivered-code instruction: only addresses inside the mapped client image are
    // supposed to be calls into the client, and those must have a bridge.
    if (address < clientBase || address >= clientBase + clientSize) {
        return true;
    }

    // A client address with no bridge: the machine stops and the call is logged, so bridges get
    // added by what real delivered code asks for.
    char reason[192];
    snprintf(reason, sizeof(reason), "delivered code called client %08x with no bridge; stack %08x %08x %08x %08x",
        address, frame[0], frame[1], frame[2], frame[3]);
    Log("%s", reason);
    faulted = true;
    faultReason = reason;
    uc_emu_stop(machine);
    return true;
}

void BuckCodeEmu::Impl::HandleInvalidMemory(uc_mem_type type, uint64_t address, int size) {
    if (faulted) {
        return;
    }

    uint32_t eip = 0;
    uint32_t esp = 0;
    uint32_t eax = 0;
    uc_reg_read(machine, UC_X86_REG_EIP, &eip);
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);
    uc_reg_read(machine, UC_X86_REG_EAX, &eax);

    char reason[256];
    snprintf(reason, sizeof(reason), "delivered code %s faulted at %08x (%d bytes), eip %08x esp %08x eax %08x",
        type == UC_MEM_WRITE ? "write" : (type == UC_MEM_READ ? "read" : "fetch"),
        static_cast<uint32_t>(address), size, eip, esp, eax);
    Log("%s", reason);
    faulted = true;
    faultReason = reason;
}

// =====================================================================
// Challenge execution
// =====================================================================

bool BuckCodeEmu::Impl::BuildMessage(const std::vector<uint8_t>& decoded, uint32_t readPosition) {
    StoreConstruct(MESSAGE_OBJECT);

    // The message as the official client has it when a 0x41B handler runs: the two-byte opcode
    // is in front and has already been read, while the challenge payload follows untouched.
    // The handler itself consumes the nonce, the code length and the code from this store.
    uint32_t buffer = HeapAlloc(static_cast<uint32_t>(decoded.size()) + 0x100);
    WriteWord(MESSAGE_OBJECT + STORE_DATA, buffer);
    WriteWord(MESSAGE_OBJECT + STORE_BASE, 0);
    WriteWord(MESSAGE_OBJECT + STORE_ALLOC, static_cast<uint32_t>(decoded.size()) + 0x100);

    uint8_t opcode[2] = { static_cast<uint8_t>(CHALLENGE_OPCODE & 0xFF), static_cast<uint8_t>((CHALLENGE_OPCODE >> 8) & 0xFF) };
    WriteMemory(buffer, opcode, sizeof(opcode));
    WriteMemory(buffer + 2, decoded.data(), static_cast<uint32_t>(decoded.size()));

    WriteWord(MESSAGE_OBJECT + STORE_SIZE, static_cast<uint32_t>(decoded.size()) + 2);
    WriteWord(MESSAGE_OBJECT + STORE_READ, readPosition);
    return true;
}

bool BuckCodeEmu::Impl::BuildReply(uint32_t& replySize) {
    StoreConstruct(REPLY_OBJECT);
    StorePut(REPLY_OBJECT, CHALLENGE_OPCODE, 4);
    StorePut(REPLY_OBJECT, CHALLENGE_MAGIC, 4);
    replySize = 0;
    return true;
}

// =====================================================================
// Public API
// =====================================================================

BuckCodeEmu::BuckCodeEmu(BuckCodeServices* services) : m_impl(new Impl) {
    m_impl->services = services;
}

BuckCodeEmu::~BuckCodeEmu() {
    m_impl->Teardown();
    delete m_impl;
}

bool BuckCodeEmu::DecodeChallenge(const uint8_t* payload, uint32_t size, uint32_t token, std::vector<uint8_t>& decoded) {
    if (!payload || size < 8) {
        return false;
    }

    decoded.assign(payload, payload + size);
    uint32_t key = token;

    for (uint32_t offset = 0; offset + 4 <= decoded.size(); offset += 4) {
        uint32_t word;
        memcpy(&word, decoded.data() + offset, sizeof(word));
        word ^= key;
        memcpy(decoded.data() + offset, &word, sizeof(word));
        key = RotateRight(key, 1);
    }

    return true;
}

bool BuckCodeEmu::DecodeLuaScripts(const uint8_t* payload, uint32_t size, std::vector<std::string>& scripts) {
    scripts.clear();

    if (!payload || size < 2) {
        return false;
    }

    // Captured 0x4B9 handler +0x32..+0xB7: BL advances by decoded + 1 even at NUL,
    // then ESI != EDI loops back for another chunk without re-reading the seed. The
    // captured 1808-byte packet contains five chunks (799/404/126/19/454 bytes).
    uint8_t key = payload[0];
    std::string script;
    uint32_t chunkStart = 1;

    for (uint32_t i = 1; i < size; i++) {
        uint8_t decoded = payload[i] ^ key;
        key = static_cast<uint8_t>(key + decoded + 1);

        if (!decoded) {
            scripts.push_back(script);
            script.clear();
            chunkStart = i + 1;
        } else {
            script += static_cast<char>(decoded);
        }
    }

    if (chunkStart != size) {
        scripts.clear();
        return false;
    }

    return true;
}

bool BuckCodeEmu::RunChallenge(const uint8_t* payload, uint32_t size, uint32_t token, uint32_t key,
    std::vector<uint8_t>& response, uint32_t& nextKey) {
    Impl& impl = *m_impl;
    nextKey = 0;

    if (!impl.Boot()) {
        return false;
    }

    std::vector<uint8_t> decoded;

    if (!DecodeChallenge(payload, size, key, decoded)) {
        impl.Log("challenge too short to carry code");
        return false;
    }

    if (size < 12) {
        impl.Log("challenge too short to carry its answer");
        return false;
    }

    uint32_t nonce = 0;
    uint32_t codeLength = 0;
    memcpy(&nonce, decoded.data(), sizeof(nonce));
    memcpy(&codeLength, decoded.data() + 4, sizeof(codeLength));

    if (codeLength > size - 8) {
        codeLength = size - 8;
    }

    // The handler reads the nonce and the code length, then copies the code. The delivered code
    // follows with one more GetInt, which reads the answer dword the server placed after the
    // code; the reply carries that value back, encoded with the nonce.
    uint32_t readPosition = 10 + codeLength;

    if (readPosition > size + 2) {
        readPosition = size + 2;
    }

    impl.BuildMessage(decoded, readPosition);
    uint32_t replySize = 0;
    impl.BuildReply(replySize);

    impl.Log("challenge %u bytes, token %08x: nonce %08x, code %u bytes", size, token, nonce, codeLength);

    // The official handler copies the decoded message to handler + 0x154 and calls it there.
    // Run the code from the same place so any self-relative reference agrees.
    uint32_t codeAddress = CODE_FALLBACK;

    if (token >= CLIENT_BASE && token + 0x154 + codeLength < MACHINE_BASE + MACHINE_SIZE) {
        codeAddress = token + 0x154;
    }

    impl.WriteMemory(codeAddress, decoded.data() + 8, codeLength);
    uc_ctl_remove_cache(impl.machine, codeAddress, codeAddress + codeLength);

    uint32_t entryFrame[1] = { STOP_ADDRESS };
    impl.WriteMemory(STACK_TOP - 0x200, entryFrame, sizeof(entryFrame));

    uint32_t esp = STACK_TOP - 0x200;
    uint32_t esi = MESSAGE_OBJECT;
    uint32_t edi = REPLY_OBJECT;
    uc_reg_write(impl.machine, UC_X86_REG_ESP, &esp);
    uc_reg_write(impl.machine, UC_X86_REG_ESI, &esi);
    uc_reg_write(impl.machine, UC_X86_REG_EDI, &edi);

    impl.faulted = false;
    impl.faultReason.clear();
    uc_err error = impl.RunGuest(codeAddress);

    if (error != UC_ERR_OK || impl.faulted) {
        impl.Log("challenge code failed%s%s", error != UC_ERR_OK ? ": " : "", error != UC_ERR_OK ? uc_strerror(error) : "");
        return false;
    }

    // The handler's finishing step: pad the reply to a multiple of four bytes, then encode every
    // dword after the id with the nonce, rotating the key right one bit each dword.
    uint32_t used = 0;
    impl.ReadWord(REPLY_OBJECT + STORE_SIZE, used);

    while (used % 4) {
        impl.StorePut(REPLY_OBJECT, 0, 1);
        used++;
    }

    uint32_t buffer = 0;
    impl.ReadWord(REPLY_OBJECT + STORE_DATA, buffer);

    uint32_t encodingKey = nonce;

    for (uint32_t offset = 4; offset < used; offset += 4) {
        uint32_t word = 0;
        impl.ReadMemory(buffer + offset, &word, sizeof(word));
        word ^= encodingKey;
        impl.WriteMemory(buffer + offset, &word, sizeof(word));
        encodingKey = RotateRight(encodingKey, 1);
    }

    response.resize(used);
    impl.ReadMemory(buffer, response.data(), used);

    nextKey = nonce;
    impl.Log("challenge answered with %u bytes (next key %08x)", used, nextKey);
    return true;
}

bool BuckCodeEmu::RunHandlerChallenge(const uint8_t* handler, uint32_t handlerSize, const uint8_t* payload,
    uint32_t size, uint32_t token, uint32_t key, std::vector<uint8_t>& response, uint32_t& nextKey) {
    Impl& impl = *m_impl;
    nextKey = 0;

    if (!handler || !handlerSize || handlerSize > 0x1000) {
        return false;
    }

    if (!impl.Boot()) {
        return false;
    }

    uint32_t handlerAddress = CODE_FALLBACK;

    if (token >= CLIENT_BASE && static_cast<uint64_t>(token) + 0x1000 < MACHINE_BASE + MACHINE_SIZE) {
        handlerAddress = token;
    }

    // The install code allocated this buffer with VirtualAlloc(0x1000) and copied the handler to
    // its start; the scratch the handler copies the delivered code into begins at +0x154.
    std::vector<uint8_t> buffer(0x1000, 0);
    impl.WriteMemory(handlerAddress, buffer.data(), static_cast<uint32_t>(buffer.size()));
    impl.WriteMemory(handlerAddress, handler, handlerSize);
    uc_ctl_remove_cache(impl.machine, handlerAddress, handlerAddress + 0x1000);

    // The wire payload, still encoded: the handler decodes it in place, as it does live.
    std::vector<uint8_t> raw(payload, payload + size);
    impl.BuildMessage(raw, 2);

    // The dispatch call: arg1 is the key the framework holds for this handler (the install token
    // first, then whatever param the handler registered), arg4 is the message; EAX carries the
    // handler's own address, which it adds 0x154 to for the code.
    uint32_t frame[5] = { STOP_ADDRESS, key, 0, 0, MESSAGE_OBJECT };
    uint32_t esp = STACK_TOP - 0x400;
    impl.WriteMemory(esp, frame, sizeof(frame));

    uint32_t eax = handlerAddress;
    uc_reg_write(impl.machine, UC_X86_REG_EAX, &eax);
    uc_reg_write(impl.machine, UC_X86_REG_ESP, &esp);

    impl.sentPacket.clear();
    impl.sentPacketValid = false;
    impl.lastRegistrationValid = false;
    impl.faulted = false;
    impl.faultReason.clear();

    uc_err error = impl.RunGuest(handlerAddress);

    if (error != UC_ERR_OK || impl.faulted) {
        impl.Log("challenge handler failed%s%s", error != UC_ERR_OK ? ": " : "",
            error != UC_ERR_OK ? uc_strerror(error) : "");
        return false;
    }

    if (!impl.sentPacketValid || impl.sentPacket.empty()) {
        impl.Log("challenge handler produced no reply");
        return false;
    }

    response = impl.sentPacket;

    if (impl.lastRegistrationValid && impl.lastRegistrationOpcode == CHALLENGE_OPCODE) {
        nextKey = impl.lastRegistrationParam;
    }

    impl.Log("challenge handler replied with %u bytes (next key %08x)", static_cast<uint32_t>(response.size()), nextKey);
    return true;
}

bool BuckCodeEmu::ReadFrameworkState(uint32_t values[10]) {
    Impl& impl = *m_impl;
    return values && impl.Boot() && impl.ReadMemory(0x00DD0FD8, values, 10 * sizeof(uint32_t));
}

bool BuckCodeEmu::RunDataHandler(const uint8_t* handler, uint32_t handlerSize, const uint8_t* payload,
    uint32_t size, uint32_t token, uint32_t hookOffset) {
    Impl& impl = *m_impl;

    if (!handler || !handlerSize || handlerSize > 0x1000) {
        return false;
    }

    if (!impl.Boot()) {
        return false;
    }

    uint32_t handlerAddress = CODE_FALLBACK;

    if (token >= CLIENT_BASE && static_cast<uint64_t>(token) + 0x1000 < MACHINE_BASE + MACHINE_SIZE) {
        handlerAddress = token;
    }

    std::vector<uint8_t> buffer(0x1000, 0);
    impl.WriteMemory(handlerAddress, buffer.data(), static_cast<uint32_t>(buffer.size()));
    impl.WriteMemory(handlerAddress, handler, handlerSize);
    uc_ctl_remove_cache(impl.machine, handlerAddress, handlerAddress + 0x1000);

    std::vector<uint8_t> raw(payload, payload + size);
    impl.BuildMessage(raw, 2);

    uint32_t frame[5] = { STOP_ADDRESS, token, 0, 0, MESSAGE_OBJECT };
    uint32_t esp = STACK_TOP - 0x400;
    impl.WriteMemory(esp, frame, sizeof(frame));

    uint32_t eax = handlerAddress;
    uc_reg_write(impl.machine, UC_X86_REG_EAX, &eax);
    uc_reg_write(impl.machine, UC_X86_REG_ESP, &esp);

    impl.faulted = false;
    impl.faultReason.clear();
    uc_err error = impl.RunGuest(handlerAddress);

    if (error != UC_ERR_OK || impl.faulted) {
        impl.Log("data handler failed%s%s", error != UC_ERR_OK ? ": " : "",
            error != UC_ERR_OK ? uc_strerror(error) : "");
        return false;
    }

    // The 0x41C handler leaves the data table the later challenges read at 0x00DD0FD8.
    uint32_t table[10] = { 0 };
    impl.ReadMemory(0x00DD0FD8, table, sizeof(table));
    impl.Log("data handler done; 0xDD0FD8: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
        table[0], table[1], table[2], table[3], table[4], table[5], table[6], table[7], table[8], table[9]);

    if (hookOffset) {
        // The 0x41D handler patches the client instruction at 0x00405544 to branch here; the
        // official client reaches it on its next data query, which parses the decompressed
        // content and fills the table. This client never executes Wow.exe's own code, so run
        // the installed hook explicitly.
        uint32_t hookAddress = handlerAddress + hookOffset;
        uint32_t hookFrame[5] = { STOP_ADDRESS, token, 0, 0, MESSAGE_OBJECT };
        uint32_t hookEsp = STACK_TOP - 0x800;
        impl.WriteMemory(hookEsp, hookFrame, sizeof(hookFrame));
        uc_reg_write(impl.machine, UC_X86_REG_ESP, &hookEsp);
        impl.faulted = false;
        impl.faultReason.clear();
        uc_err hookError = impl.RunGuest(hookAddress);

        if (hookError != UC_ERR_OK || impl.faulted) {
            impl.Log("data handler hook failed%s%s", hookError != UC_ERR_OK ? ": " : "",
                hookError != UC_ERR_OK ? uc_strerror(hookError) : "");
            return false;
        }

        impl.ReadMemory(0x00DD0FD8, table, sizeof(table));
        impl.Log("data handler hook done; 0xDD0FD8: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
            table[0], table[1], table[2], table[3], table[4], table[5], table[6], table[7], table[8], table[9]);
    }

    return true;
}

#include "Engine/Source/Os/Mac/BuckCodeEmuThreads.inl"
#include "Engine/Source/Os/Mac/BuckCodeEmuFramework.inl"
#include "Engine/Source/Os/Mac/BuckCodeEmuContent.inl"

#endif
