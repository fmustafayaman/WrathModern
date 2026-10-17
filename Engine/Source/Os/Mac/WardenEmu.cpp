#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "Engine/Source/Os/WardenEmu.h"
#include "Engine/Source/Os/Mac/WardenSystemExports.h"

#include <unicorn/unicorn.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <strings.h>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <vector>

/*
 * Emulated BLL loader and Warden module runner (macOS, arm64).
 *
 * The 3.3.5a client loads Warden modules with Engine/Source/Os/Win32/BLLLoader.cpp: a custom
 * Blizzard image format loaded in-process. The module image is 32-bit Windows code, so an arm64
 * host cannot run it natively; this bridge runs it in a Unicorn x86 machine instead.
 *
 * Everything here reproduces what the original loader does:
 *  - the BLL image layout (40-byte header, run-length copy, relocations, section protections),
 *  - imports resolved to emulator stubs for the few system functions the emulator knows,
 *  - the module export and the four module entry points (WardenModule vtable),
 *  - the client's OurLibrary interface exposed to the module as a guest vtable whose slots are
 *    hooked back to WardenEmuServices.
 *
 * The emulation is faithful: the module's own code reads server data and decides what to send
 * back. Nothing fakes or filters responses; keeping the memory areas untouched is as important
 * here as running the code untouched.
 */

namespace {

// =====================================================================
// Guest memory layout
// =====================================================================

constexpr uint32_t TEB_BASE = 0x000FE000;
constexpr uint32_t GDT_BASE = 0x000FF000;
constexpr uint32_t PEB_BASE = 0x000FD000; // process environment block
constexpr uint32_t TLS_ARRAY_BASE = 0x000F8000; // static-TLS slot array (fs:[0x2C])
constexpr uint32_t ENV_BASE = 0x000F7000; // loader data, module list, heap list

constexpr uint32_t STACK_BASE = 0x00100000;
constexpr uint32_t STACK_SIZE = 0x00080000; // 512 KiB
constexpr uint32_t STACK_TOP = STACK_BASE + STACK_SIZE - 0x100;

constexpr uint32_t MISC_BASE = 0x00200000; // stop sentinel, objects, stubs, small copies
constexpr uint32_t MISC_SIZE = 0x00001000;

constexpr uint32_t SCRATCH_BASE = 0x00210000; // larger buffers handed across the boundary
constexpr uint32_t SCRATCH_SIZE = 0x00010000;

// 0x00400000 up to HEAP_BASE belongs to the client executable, as in the real process: Wow.exe
// maps at its preferred base 0x00400000 (up to 0x00DFD000 for 12340), and server requests
// resolve client functions as offsets into it. Everything the emulator owns stays clear of it.
constexpr uint32_t CLIENT_DEFAULT_BASE = 0x00400000;

constexpr uint32_t HEAP_BASE = 0x01000000;
constexpr uint32_t HEAP_SIZE = 0x00400000; // mapped heap, bump allocated for the module

constexpr uint32_t IMAGE_BASE = 0x01800000;
constexpr uint32_t IMAGE_SIZE = 0x00800000; // 8 MiB, covers every known module comfortably

constexpr uint32_t HEAP_EXT_BASE = 0x02000000;
constexpr uint32_t HEAP_EXT_SIZE = 0x00200000; // relief block if the first heap fills up

// Honest ceiling for bytes crossing the bridge: buffers handed to the module live in the
// scratch area below the out-parameter block, which caps the delivered packet size.
// Objects inside MISC_BASE / SCRATCH_BASE.
constexpr uint32_t STOP_ADDRESS = MISC_BASE + 0x10; // emu runs stop when EIP lands here
constexpr uint32_t LIB_OBJECT = MISC_BASE + 0x80; // the guest OurLibrary object
constexpr uint32_t LIB_VTABLE = MISC_BASE + 0xC0; // 16 slots
constexpr uint32_t LIB_STUBS = MISC_BASE + 0x100; // 16 stubs, 8 bytes each
constexpr uint32_t SCRATCH_KEY = SCRATCH_BASE + 0x0000; // session key copy
constexpr uint32_t SCRATCH_DATA = SCRATCH_BASE + 0x0100; // bytes delivered to the module
constexpr uint32_t SCRATCH_OUTPUT = SCRATCH_BASE + 0x9000; // out-parameters

// Honest ceiling for bytes crossing the bridge. Buffers handed to the module live in the
// scratch area below the out-parameter block, which caps the delivered packet size; everything
// else moves between the module's own memory and host buffers under the same generous bound.
constexpr uint32_t MAX_DELIVER_BYTES = SCRATCH_OUTPUT - SCRATCH_DATA;
constexpr uint32_t MAX_HOST_TRANSFER_BYTES = 0x40000; // module-heap <-> host moves; far beyond every real module

constexpr uint32_t LIB_SLOT_COUNT = 16;

// Pseudo-handles handed to the module for the objects an emulated Windows process owns. The
// values are arbitrary but stable and non-null, which is all the module ever relies on.
constexpr uint32_t PSEUDO_CURRENT_PROCESS = 0xFFFFFFFF; // (HANDLE)-1, exactly as Windows returns
constexpr uint32_t PSEUDO_PROCESS_HEAP = 0x03000000;
constexpr uint32_t PSEUDO_KERNEL32 = 0x03010000;
constexpr uint32_t PSEUDO_USER32 = 0x03020000;

// Client functions a module can reach inside the client image, bridged to this client's own
// implementation of the same function. The addresses are the 12340 Wow.exe entry points; the
// leading bytes confirm the mapped executable really has that function there before a bridge is
// armed. Anything else executed inside the client image stops the call (see HandleClientCode).
enum ClientFunctionId {
    CLIENT_OS_GET_ASYNC_TIME_MS,
    CLIENT_OS_LOAD_LIBRARY,
};

struct ClientFunction {
    ClientFunctionId id;
    const char* name;
    uint32_t address;
    uint8_t prologue[6];
};

constexpr ClientFunction s_clientFunctions[] = {
    // uint32_t __cdecl OsGetAsyncTimeMs(): mov ecx, [s_OsTimeMgr]; jmp OsTimeManager::Snapshot
    { CLIENT_OS_GET_ASYNC_TIME_MS, "OsGetAsyncTimeMs", 0x0086AE20, { 0x8B, 0x0D, 0x9C, 0x15, 0xD4, 0x00 } },
    // void* __cdecl OsLoadLibrary(const char* fileName): push ebp; mov ebp, esp; mov eax, [ebp+8]
    { CLIENT_OS_LOAD_LIBRARY, "OsLoadLibrary", 0x0086C4E0, { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 } },
};

constexpr uint32_t CLIENT_FUNCTION_COUNT = sizeof(s_clientFunctions) / sizeof(s_clientFunctions[0]);

// Windows memory-management constants, as VirtualQuery / VirtualProtect / VirtualAlloc use them.
constexpr uint32_t WIN_MEM_COMMIT = 0x00001000;
constexpr uint32_t WIN_MEM_RESERVE = 0x00002000;
constexpr uint32_t WIN_MEM_DECOMMIT = 0x00004000;
constexpr uint32_t WIN_MEM_RELEASE = 0x00008000;
constexpr uint32_t WIN_MEM_FREE = 0x00010000;
constexpr uint32_t WIN_MEM_PRIVATE = 0x00020000;
constexpr uint32_t WIN_MEM_IMAGE = 0x01000000;
constexpr uint32_t WIN_PAGE_NOACCESS = 0x01;
constexpr uint32_t WIN_PAGE_READONLY = 0x02;
constexpr uint32_t WIN_PAGE_READWRITE = 0x04;
constexpr uint32_t WIN_PAGE_EXECUTE_READ = 0x20;
constexpr uint32_t WIN_PAGE_EXECUTE_READWRITE = 0x40;
constexpr uint32_t WIN_PAGE_EXECUTE_WRITECOPY = 0x80;
constexpr uint32_t WIN_USER_SPACE_END = 0x7FFF0000; // lpMaximumApplicationAddress + 1

// MSVC __thiscall callee-side semantics: the stub's own `ret n` cleans the arguments, the hook
// only fills EAX (and lets the ret run). Sizes follow the OurLibrary signatures.
struct LibSlotInfo {
    const char* name;
    uint16_t argumentBytes;
    bool implemented;
};

constexpr LibSlotInfo s_libSlots[LIB_SLOT_COUNT] = {
    // Slots 0-6 follow the original declaration order of OurLibrary.
    { "Data", 8, true },
    { "ModuleUse", 8, true },
    { "ModuleCache", 12, true },
    { "MemoryAlloc", 4, true },
    { "MemoryFree", 4, true },
    { "StateSave", 8, true },
    { "StateLoad", 8, true },
    // Anything the decompiled client does not expose gets logged with a zero result, so newer
    // custom modules surface what they need instead of silently doing nothing.
    { "Unimplemented8", 8, false },
    { "Unimplemented8", 8, false },
    { "Unimplemented12", 12, false },
    { "Unimplemented8", 8, false },
    { "Unimplemented12", 12, false },
    { "Unimplemented8", 8, false },
    { "Unimplemented12", 12, false },
    { "Unimplemented8", 8, false },
    { "Unimplemented12", 12, false },
};

}

// =====================================================================
// Logging
// =====================================================================

static void WardenLog(const char* format, ...) {
    // The app chdirs into the game directory; write into the shared logs folder, falling back
    // to the directory root for first runs.
    FILE* file = fopen("Logs/warden.log", "a");

    if (!file) {
        file = fopen("warden.log", "a");
    }

    if (!file) {
        return;
    }

    static uint64_t s_logCounter;
    uint64_t stamp = ++s_logCounter;

    fprintf(file, "[%llu] ", (unsigned long long)stamp);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fclose(file);
}

// =====================================================================
// The bridge state
// =====================================================================

struct WardenEmu::Impl {
    WardenEmuServices* services = nullptr;
    uc_engine* machine = nullptr;
    bool booted = false;

    // Module state.
    bool imageLoaded = false;
    bool moduleCreated = false;
    uint32_t imageSize = 0;
    uint32_t exportAddress = 0;
    uint32_t moduleObject = 0;

    // Guest heap regions (bump allocated for the module). The extended region starts one page
    // in, its first page holds the import stubs.
    uint32_t m_heapNext[2] = { HEAP_BASE, HEAP_EXT_BASE + 0x1000 };
    uint32_t m_heapLimit[2] = { HEAP_BASE + HEAP_SIZE, HEAP_EXT_BASE + HEAP_EXT_SIZE };
    uint32_t heapRegion = 0;

    // Host-side staging for data moving between module memory and the services.
    std::vector<uint8_t> moduleTransferBuffer;

    // Thread-local storage the module allocates through TlsAlloc. The emulation runs one
    // emulated thread, so a flat table with an allocation bitmap is exact.
    static const uint32_t TLS_SLOT_COUNT = 64;
    uint32_t m_tlsValues[TLS_SLOT_COUNT] = { 0 };
    bool m_tlsUsed[TLS_SLOT_COUNT] = { false };

    // The unhandled-exception filter the module installs (SetUnhandledExceptionFilter).
    uint32_t m_unhandledFilter = 0;

    // The client executable inside the guest (see CLIENT_DEFAULT_BASE). Size zero when the
    // services have no Windows executable: the base is still what GetModuleHandle(NULL) names,
    // and anything reaching into it faults and is logged rather than answered.
    uint32_t m_clientBase = CLIENT_DEFAULT_BASE;
    uint32_t m_clientSize = 0;
    bool m_clientBridged[CLIENT_FUNCTION_COUNT] = { false };

    // The address space as Windows describes it to the module (VirtualQuery), page granular,
    // sorted and non-overlapping; anything not listed is free. It describes the process the
    // module believes it runs in: the client image by section, private allocations with the
    // protection they were given. The emulator's own plumbing (stubs, sentinel) is described as
    // what it stands in for, since the real process has no such memory of its own.
    struct MemoryRegion {
        uint32_t base;
        uint32_t size;
        uint32_t allocationBase;
        uint32_t allocationProtect;
        uint32_t protect;
        uint32_t state;
        uint32_t type;
    };

    std::vector<MemoryRegion> m_memoryMap;

    // Set when the module takes a path the emulator cannot continue from (RaiseException,
    // TerminateProcess). The call that triggered it then reports failure instead of resuming
    // with state the real process would never have reached.
    bool m_faulted = false;

    // Guest stub addresses handed out for imports.
    struct ImportStub {
        uint32_t address = 0;
        uint32_t argumentBytes = 0;
        char dll[64];
        char name[192];
        // False for a system export the module links but the emulator gives no behavior: the
        // import resolves as it does on Windows, and a call into it stops the module.
        bool implemented = true;
    };

    ImportStub imports[128];
    uint32_t importCount = 0;
    uint32_t nextImportStub = 0;

    // Diagnostics. The last basic blocks the guest entered (address and ESP on entry): when a
    // call goes wrong, the path that led there names the site that went astray, which neither
    // the entry address nor the faulting EIP alone can do for a wild branch.
    static const uint32_t TRACE_LENGTH = 256;
    uint32_t m_traceAddress[TRACE_LENGTH] = { 0 };
    uint32_t m_traceStack[TRACE_LENGTH] = { 0 };
    uint32_t m_traceRepeat[TRACE_LENGTH] = { 0 };
    uint32_t m_traceNext = 0;
    void RecordBlock(uint32_t address, uint32_t esp);
    uint32_t m_traceDumps = 0;

    // The machine as it stood at the start of the current entry call. A failing call writes it
    // to Logs/warden-snapshot.bin once per emulator, and ReplaySnapshot reruns the same call
    // from it; the session key and packet history that led there are then no longer needed.
    struct SnapshotRegion {
        uint32_t base;
        std::vector<uint8_t> bytes;
    };

    struct Snapshot {
        uint32_t entry = 0;
        uint32_t ecx = 0;
        uint32_t argumentCount = 0;
        uint32_t arguments[16] = { 0 };
        char what[64] = { 0 };
        uint32_t registers[9] = { 0 }; // eax ebx ecx edx esi edi ebp esp eflags
        std::vector<SnapshotRegion> regions;
    };

    Snapshot m_snapshot;
    bool m_snapshotWritten = false;
    bool m_replaying = false;

    bool Boot();
    void MapClientImage();
    void SetupEnvironment();
    void Teardown();
    void SetupStubs();

    // Import plumbing: which APIs have real implementations, and running one for a stub call.
    static uint32_t KnownImportArgumentBytes(const char* name);
    static bool IsSystemExport(const char* dll, const char* name);
    uint32_t StubForName(const char* name);
    void HandleImport(const ImportStub& stub);
    uint32_t RunImport(const ImportStub& stub, uint32_t* arguments, uint32_t argumentCount);

    bool ReadMemory(uint32_t address, void* out, uint32_t size) const;
    bool WriteMemory(uint32_t address, const void* in, uint32_t size);
    bool ReadWord(uint32_t address, uint32_t& value) const;
    bool WriteWord(uint32_t address, uint32_t value);

    // NUL-terminated guest string reader used by the string import implementations.
    uint32_t ReadGuestString(uint32_t address, char* out, uint32_t capacity) const;

    uint32_t GuestAlloc(uint32_t size);
    uint32_t GuestAllocPages(uint32_t size);

    // Memory map (see m_memoryMap).
    void SetMemoryRange(const MemoryRegion& region);
    void AddAllocation(uint32_t base, uint32_t size, uint32_t protect, uint32_t type);
    bool ProtectMemory(uint32_t address, uint32_t size, uint32_t protect, uint32_t& oldProtect);
    bool QueryMemory(uint32_t address, uint32_t* info) const;
    void DescribeBootMemory();
    uint32_t NewImportStub(const char* dll, const char* name, uint32_t argumentBytes);

    bool CallGuest(uint32_t address, const uint32_t* stackArguments, uint32_t argumentCount,
        uint32_t ecx, const char* what, uint32_t& eax);

    // Diagnostics.
    void DumpTrace(const char* reason);
    void CaptureSnapshot(uint32_t address, const uint32_t* stackArguments, uint32_t argumentCount,
        uint32_t ecx, const char* what);
    bool WriteSnapshot(const char* path) const;
    bool ReadSnapshot(const char* path);

    // Hook handlers.
    void HandleClientCode(uint32_t address);
    void HandleCodeHook(uint64_t address);
    bool HandleInvalidMemory(uc_mem_type type, uint64_t address, int size, int64_t value);
};

// =====================================================================
// Boot / teardown
// =====================================================================

bool WardenEmu::Impl::Boot() {
    if (booted) {
        return true;
    }

    uc_err error = uc_open(UC_ARCH_X86, UC_MODE_32, &machine);

    if (error != UC_ERR_OK) {
        WardenLog("uc_open failed: %s", uc_strerror(error));
        return false;
    }

    // Every region is mapped up front: Unicorn does not allow remapping while the machine is
    // running, and hooks (MemoryAlloc, import stubs) run inside uc_emu_start.
    uc_mem_map(machine, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
    uc_mem_map(machine, MISC_BASE, MISC_SIZE, UC_PROT_ALL);
    uc_mem_map(machine, SCRATCH_BASE, SCRATCH_SIZE, UC_PROT_ALL);
    uc_mem_map(machine, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL);
    uc_mem_map(machine, IMAGE_BASE, IMAGE_SIZE, UC_PROT_ALL);
    uc_mem_map(machine, HEAP_EXT_BASE, HEAP_EXT_SIZE, UC_PROT_ALL);
    uc_mem_map(machine, TEB_BASE, 0x1000, UC_PROT_READ | UC_PROT_WRITE);
    uc_mem_map(machine, GDT_BASE, 0x1000, UC_PROT_READ);
    uc_mem_map(machine, PEB_BASE, 0x1000, UC_PROT_READ | UC_PROT_WRITE);
    uc_mem_map(machine, TLS_ARRAY_BASE, 0x1000, UC_PROT_READ | UC_PROT_WRITE);
    uc_mem_map(machine, ENV_BASE, 0x1000, UC_PROT_READ | UC_PROT_WRITE);

    // Flat 32-bit segments, fs pointing at the TEB page for code that touches it.
    uint8_t descriptors[4][8];
    memset(descriptors, 0, sizeof(descriptors));

    auto putDescriptor = [&](uint32_t index, uint32_t limit, uint32_t base, uint8_t access, uint8_t flags) {
        descriptors[index][0] = limit & 0xFF;
        descriptors[index][1] = (limit >> 8) & 0xFF;
        descriptors[index][2] = base & 0xFF;
        descriptors[index][3] = (base >> 8) & 0xFF;
        descriptors[index][4] = (base >> 16) & 0xFF;
        descriptors[index][5] = access;
        descriptors[index][6] = flags;
        descriptors[index][7] = (base >> 24) & 0xFF;
    };

    putDescriptor(1, 0xFFFF, 0, 0x9A, 0xCF); // 0x08 code, 4 GiB
    putDescriptor(2, 0xFFFF, 0, 0x92, 0xCF); // 0x10 data, 4 GiB
    putDescriptor(3, 0x0FFF, TEB_BASE, 0x92, 0x00); // 0x18 fs, TEB area

    WriteMemory(GDT_BASE, descriptors, sizeof(descriptors));

    uc_x86_mmr gdtr;
    memset(&gdtr, 0, sizeof(gdtr));
    gdtr.base = GDT_BASE;
    gdtr.limit = 0x20;
    uc_reg_write(machine, UC_X86_REG_GDTR, &gdtr);

    uint32_t selector;
    selector = 0x08;
    uc_reg_write(machine, UC_X86_REG_CS, &selector);
    selector = 0x10;
    uc_reg_write(machine, UC_X86_REG_DS, &selector);
    uc_reg_write(machine, UC_X86_REG_ES, &selector);
    uc_reg_write(machine, UC_X86_REG_SS, &selector);
    uc_reg_write(machine, UC_X86_REG_GS, &selector);
    selector = 0x18;
    uc_reg_write(machine, UC_X86_REG_FS, &selector);

    DescribeBootMemory();
    MapClientImage();
    SetupEnvironment();

    // The stop sentinel and the stubs are code; a plain ret is enough because the hooks take
    // care of the work (and the argument cleanup rides on the stub's own ret n).
    uint8_t stopBytes[16];
    memset(stopBytes, 0xC3, sizeof(stopBytes));
    WriteMemory(STOP_ADDRESS, stopBytes, sizeof(stopBytes));

    SetupStubs();

    uint32_t vtableValue;
    for (uint32_t i = 0; i < LIB_SLOT_COUNT; i++) {
        vtableValue = LIB_STUBS + i * 8;
        WriteWord(LIB_VTABLE + i * 4, vtableValue);
    }

    // The OurLibrary object only needs its vtable pointer; the hooks ignore the object identity
    // since the emulation runs one module at a time from the client side.
    WriteWord(LIB_OBJECT, LIB_VTABLE);

    // Unicorn's C API takes hooks as plain function pointers; the lambdas decay to pointers
    // and keep C++ happy while dispatching back into the bridge.
    uc_hook codeHook;
    uc_hook_add(machine, &codeHook, UC_HOOK_CODE,
        reinterpret_cast<void*>(+[](uc_engine* machine, uint64_t address, uint32_t size, void* user) {
            (void)machine;
            (void)size;
            static_cast<WardenEmu::Impl*>(user)->HandleCodeHook(address);
        }),
        this, LIB_STUBS, LIB_STUBS + LIB_SLOT_COUNT * 8 - 1);

    // The import stubs live on their own page; a separate range keeps the code hook off every
    // module instruction, which matters once real modules start running loops.
    uc_hook importHook;
    uc_hook_add(machine, &importHook, UC_HOOK_CODE,
        reinterpret_cast<void*>(+[](uc_engine* machine, uint64_t address, uint32_t size, void* user) {
            (void)machine;
            (void)size;
            static_cast<WardenEmu::Impl*>(user)->HandleCodeHook(address);
        }),
        this, HEAP_EXT_BASE, HEAP_EXT_BASE + 0x1000 - 1);

    // begin=1, end=0 is Unicorn's "every address" range: the earlier (0, 0) only covered address
    // zero, so genuine unmapped accesses were missed and surfaced only as a bare emu error.
    uc_hook invalidHook;
    uc_hook_add(machine, &invalidHook, UC_HOOK_MEM_INVALID,
        reinterpret_cast<void*>(+[](uc_engine* machine, uc_mem_type type, uint64_t address, int size,
            int64_t value, void* user) -> bool {
            (void)machine;
            return static_cast<WardenEmu::Impl*>(user)->HandleInvalidMemory(type, address, size, value);
        }),
        this, 1, 0);

    // Branch history for the diagnostics: one ring entry per basic block, cheap next to the
    // per-instruction hooks and enough to reconstruct how a wild call was reached.
    uc_hook blockHook;
    uc_hook_add(machine, &blockHook, UC_HOOK_BLOCK,
        reinterpret_cast<void*>(+[](uc_engine* machine, uint64_t address, uint32_t size, void* user) {
            (void)size;
            WardenEmu::Impl* impl = static_cast<WardenEmu::Impl*>(user);
            uint32_t esp = 0;
            uc_reg_read(machine, UC_X86_REG_ESP, &esp);
            impl->RecordBlock((uint32_t)address, esp);
        }),
        this, 1, 0);

    uc_hook intrHook;
    uc_hook_add(machine, &intrHook, UC_HOOK_INTR,
        reinterpret_cast<void*>(+[](uc_engine* machine, int intno, void* user) {
            (void)machine;
            (void)user;
            WardenLog("guest raised interrupt %d", intno);
        }),
        this, 0, 0);

    booted = true;
    return true;
}

// A Windows process environment: the TEB the CRT reads through fs: and the PEB behind it,
// plus a minimal loader list and heap list. Server modules are MSVC-built and their runtime
// walks these structures (fs:[0x18] self, fs:[0x2C] static-TLS array, fs:[0x30] PEB); leaving
// them zero makes the first such dereference fault at address zero.
void WardenEmu::Impl::SetupEnvironment() {
    WriteWord(TEB_BASE + 0x00, 0xFFFFFFFF); // ExceptionList: end of the SEH chain
    WriteWord(TEB_BASE + 0x04, STACK_TOP); // StackBase
    WriteWord(TEB_BASE + 0x08, STACK_BASE); // StackLimit
    WriteWord(TEB_BASE + 0x18, TEB_BASE); // Self
    WriteWord(TEB_BASE + 0x20, (uint32_t)getpid()); // ClientId.UniqueProcess
    WriteWord(TEB_BASE + 0x24, (uint32_t)(uintptr_t)pthread_self()); // ClientId.UniqueThread
    WriteWord(TEB_BASE + 0x2C, TLS_ARRAY_BASE); // static-TLS slot array
    WriteWord(TEB_BASE + 0x30, PEB_BASE); // ProcessEnvironmentBlock

    WriteWord(PEB_BASE + 0x02, 0); // BeingDebugged: this is not a debugged process
    WriteWord(PEB_BASE + 0x08, m_clientBase); // ImageBaseAddress: the client executable
    WriteWord(PEB_BASE + 0x0C, ENV_BASE); // Ldr
    WriteWord(PEB_BASE + 0x18, PSEUDO_PROCESS_HEAP); // ProcessHeap
    WriteWord(PEB_BASE + 0x60, 1); // NumberOfProcessors
    WriteWord(PEB_BASE + 0x80, 1); // NumberOfHeaps
    WriteWord(PEB_BASE + 0x84, 16); // MaximumNumberOfHeaps
    WriteWord(PEB_BASE + 0x88, ENV_BASE + 0xA0); // ProcessHeaps

    // A one-entry process heap list, so the list walk the CRT does finds what it expects.
    WriteWord(ENV_BASE + 0xA0, PSEUDO_PROCESS_HEAP);

    // PEB_LDR_DATA with a single module: the client executable. Warden modules are loaded by
    // the client's own BLL loader and never appear in the loader lists on Windows either.
    const uint32_t ldr = ENV_BASE + 0x00;
    const uint32_t entry = ENV_BASE + 0x40;
    const uint32_t nameBuffer = ENV_BASE + 0xE0;
    const char* moduleName = "Wow.exe";
    const uint32_t nameCharacters = (uint32_t)strlen(moduleName);
    const uint32_t nameLength = nameCharacters * 2; // UTF-16

    WriteWord(ldr + 0x00, 0x30); // Length
    WriteWord(ldr + 0x04, 1); // Initialized
    WriteWord(ldr + 0x0C, entry); // InLoadOrderModuleList.Flink
    WriteWord(ldr + 0x10, entry); // InLoadOrderModuleList.Blink
    WriteWord(ldr + 0x14, entry + 0x08); // InMemoryOrderModuleList.Flink
    WriteWord(ldr + 0x18, entry + 0x08); // InMemoryOrderModuleList.Blink
    WriteWord(ldr + 0x1C, entry + 0x10); // InInitializationOrderModuleList.Flink
    WriteWord(ldr + 0x20, entry + 0x10); // InInitializationOrderModuleList.Blink

    WriteWord(entry + 0x00, entry); // InLoadOrderLinks.Flink
    WriteWord(entry + 0x04, entry); // InLoadOrderLinks.Blink
    WriteWord(entry + 0x08, entry + 0x08); // InMemoryOrderLinks.Flink
    WriteWord(entry + 0x0C, entry + 0x08); // InMemoryOrderLinks.Blink
    WriteWord(entry + 0x10, entry + 0x10); // InInitializationOrderLinks.Flink
    WriteWord(entry + 0x14, entry + 0x10); // InInitializationOrderLinks.Blink
    WriteWord(entry + 0x18, m_clientBase); // DllBase
    WriteWord(entry + 0x1C, 0); // EntryPoint
    WriteWord(entry + 0x20, m_clientSize); // SizeOfImage
    WriteWord(entry + 0x24, nameLength); // FullDllName.Length
    WriteWord(entry + 0x26, nameLength); // FullDllName.MaximumLength
    WriteWord(entry + 0x28, nameBuffer); // FullDllName.Buffer
    WriteWord(entry + 0x2C, nameLength); // BaseDllName.Length
    WriteWord(entry + 0x2E, nameLength); // BaseDllName.MaximumLength
    WriteWord(entry + 0x30, nameBuffer); // BaseDllName.Buffer

    uint8_t nameText[64] = { 0 };

    for (uint32_t i = 0; i < nameCharacters; i++) {
        nameText[i * 2] = (uint8_t)moduleName[i];
    }

    WriteMemory(nameBuffer, nameText, nameLength + 2);
}

// The client executable inside the guest, at the base the services report (0x00400000 for the
// 12340 Wow.exe), with a bridge for each client function the module may call. The executable's
// bytes are what the module reads when it inspects the client; its x86 code never runs here.
void WardenEmu::Impl::MapClientImage() {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t base = 0;

    m_clientBase = CLIENT_DEFAULT_BASE;
    m_clientSize = 0;
    memset(m_clientBridged, 0, sizeof(m_clientBridged));

    if (!services || !services->ClientImage(data, size, base) || !data || !size) {
        WardenLog("no client executable available; GetModuleHandle(NULL) names unmapped memory at %08x",
            m_clientBase);
        return;
    }

    uint32_t mappedSize = (size + 0xFFF) & ~0xFFFu;

    if ((base & 0xFFF) || base < SCRATCH_BASE + SCRATCH_SIZE || base + mappedSize > HEAP_BASE) {
        WardenLog("client executable at %08x (+%08x) does not fit the guest layout; not mapped", base, size);
        return;
    }

    if (uc_mem_map(machine, base, mappedSize, UC_PROT_ALL) != UC_ERR_OK || !WriteMemory(base, data, size)) {
        WardenLog("client executable could not be mapped at %08x", base);
        return;
    }

    m_clientBase = base;
    m_clientSize = size;

    // The image as the loader maps it: one MEM_IMAGE allocation, the headers read-only and each
    // section with the protection its characteristics give a running process.
    uint32_t ntOffset = 0;
    memcpy(&ntOffset, data + 0x3C, sizeof(ntOffset));

    if (ntOffset + 4 + 20 <= size) {
        uint16_t sectionCount = 0;
        uint16_t optionalSize = 0;
        uint32_t headersSize = 0;
        memcpy(&sectionCount, data + ntOffset + 6, sizeof(sectionCount));
        memcpy(&optionalSize, data + ntOffset + 20, sizeof(optionalSize));

        if (ntOffset + 24 + 64 <= size) {
            memcpy(&headersSize, data + ntOffset + 24 + 60, sizeof(headersSize));
        }

        MemoryRegion headers = { base, (headersSize + 0xFFF) & ~0xFFFu, base, WIN_PAGE_EXECUTE_WRITECOPY,
            WIN_PAGE_READONLY, WIN_MEM_COMMIT, WIN_MEM_IMAGE };
        SetMemoryRange(headers);

        uint32_t sectionTable = ntOffset + 24 + optionalSize;

        for (uint32_t i = 0; i < sectionCount && sectionTable + (i + 1) * 40 <= size; i++) {
            uint32_t virtualSize, virtualAddress, characteristics;
            memcpy(&virtualSize, data + sectionTable + i * 40 + 8, 4);
            memcpy(&virtualAddress, data + sectionTable + i * 40 + 12, 4);
            memcpy(&characteristics, data + sectionTable + i * 40 + 36, 4);

            bool executable = (characteristics & 0x20000000) != 0;
            bool writable = (characteristics & 0x80000000) != 0;
            uint32_t protect = executable ? (writable ? WIN_PAGE_EXECUTE_READWRITE : WIN_PAGE_EXECUTE_READ)
                                          : (writable ? WIN_PAGE_READWRITE : WIN_PAGE_READONLY);

            MemoryRegion section = { base + virtualAddress, (virtualSize + 0xFFF) & ~0xFFFu, base,
                WIN_PAGE_EXECUTE_WRITECOPY, protect, WIN_MEM_COMMIT, WIN_MEM_IMAGE };
            SetMemoryRange(section);
        }
    }

    for (uint32_t i = 0; i < CLIENT_FUNCTION_COUNT; i++) {
        const ClientFunction& function = s_clientFunctions[i];
        uint32_t offset = function.address - base;

        m_clientBridged[i] = function.address >= base && offset + sizeof(function.prologue) <= size
            && memcmp(data + offset, function.prologue, sizeof(function.prologue)) == 0;

        if (!m_clientBridged[i]) {
            WardenLog("client function %s is not at %08x in this executable; not bridged", function.name,
                function.address);
        }
    }

    // Block granularity is enough: a module can only enter client code by branching to it.
    uc_hook clientHook;
    uc_hook_add(machine, &clientHook, UC_HOOK_BLOCK,
        reinterpret_cast<void*>(+[](uc_engine* machine, uint64_t address, uint32_t size, void* user) {
            (void)machine;
            (void)size;
            static_cast<WardenEmu::Impl*>(user)->HandleClientCode((uint32_t)address);
        }),
        this, base, base + mappedSize - 1);

    WardenLog("client executable mapped at %08x (%u bytes)", base, size);
}

void WardenEmu::Impl::Teardown() {
    if (machine) {
        uc_close(machine);
        machine = nullptr;
    }

    booted = false;
    m_heapNext[0] = HEAP_BASE;
    m_heapNext[1] = HEAP_EXT_BASE + 0x1000;
    m_heapLimit[0] = HEAP_BASE + HEAP_SIZE;
    m_heapLimit[1] = HEAP_EXT_BASE + HEAP_EXT_SIZE;
    heapRegion = 0;
    importCount = 0;
    nextImportStub = 0;
    imageLoaded = false;
    moduleCreated = false;
    moduleObject = 0;
    exportAddress = 0;
    imageSize = 0;
}

// =====================================================================
// Memory helpers
// =====================================================================

bool WardenEmu::Impl::ReadMemory(uint32_t address, void* out, uint32_t size) const {
    if (uc_mem_read(machine, address, out, size) != UC_ERR_OK) {
        WardenLog("read failed at %08x size %u", address, size);
        return false;
    }

    return true;
}

bool WardenEmu::Impl::WriteMemory(uint32_t address, const void* in, uint32_t size) {
    if (uc_mem_write(machine, address, in, size) != UC_ERR_OK) {
        WardenLog("write failed at %08x size %u", address, size);
        return false;
    }

    return true;
}

bool WardenEmu::Impl::ReadWord(uint32_t address, uint32_t& value) const {
    if (uc_mem_read(machine, address, &value, sizeof(value)) != UC_ERR_OK) {
        WardenLog("read failed at %08x size 4", address);
        return false;
    }

    return true;
}

bool WardenEmu::Impl::WriteWord(uint32_t address, uint32_t value) {
    if (uc_mem_write(machine, address, &value, sizeof(value)) != UC_ERR_OK) {
        WardenLog("write failed at %08x size 4", address);
        return false;
    }

    return true;
}

uint32_t WardenEmu::Impl::ReadGuestString(uint32_t address, char* out, uint32_t capacity) const {
    for (uint32_t i = 0; i + 1 < capacity; i++) {
        uint8_t byte;

        if (!ReadMemory(address + i, &byte, 1)) {
            return 0;
        }

        out[i] = static_cast<char>(byte);

        if (!byte) {
            return i + 1;
        }
    }

    out[capacity - 1] = '\0';
    return capacity;
}

// =====================================================================
// Guest allocation and stub housekeeping
// =====================================================================

uint32_t WardenEmu::Impl::GuestAlloc(uint32_t size) {
    // The extended region starts one page in: the import stubs own its first page.
    size = size ? (size + 7) & ~7u : 8;

    uint32_t region = heapRegion;

    while (region < 2 && m_heapNext[region] + size > m_heapLimit[region]) {
        region++;
    }

    if (region >= 2) {
        WardenLog("guest heap allocation failed (%u bytes)", size);
        return 0;
    }

    if (region != heapRegion) {
        WardenLog("guest heap region %u exhausted, continuing in region %u", heapRegion, region);
        heapRegion = region;
    }

    uint32_t address = m_heapNext[region];
    m_heapNext[region] += size;

    // Fresh memory reads as zeros in the original (VirtualAlloc MEM_COMMIT semantics).
    uint32_t zero = 0;

    for (uint32_t off = 0; off < size; off += sizeof(zero)) {
        uint32_t chunk = size - off < sizeof(zero) ? size - off : sizeof(zero);
        WriteMemory(address + off, &zero, chunk);
    }

    return address;
}

// Page-aligned blocks, as VirtualAlloc hands them out: the bump pointer skips to the next page
// first, so the block owns its pages and can carry its own protection in the memory map.
uint32_t WardenEmu::Impl::GuestAllocPages(uint32_t size) {
    uint32_t pages = (size + 0xFFF) & ~0xFFFu;

    if (!pages) {
        return 0;
    }

    uint32_t region = heapRegion;

    while (region < 2) {
        uint32_t start = (m_heapNext[region] + 0xFFF) & ~0xFFFu;

        if (start + pages <= m_heapLimit[region]) {
            break;
        }

        region++;
    }

    if (region >= 2) {
        WardenLog("guest page allocation failed (%u bytes)", size);
        return 0;
    }

    heapRegion = region;
    m_heapNext[region] = (m_heapNext[region] + 0xFFF) & ~0xFFFu;
    return GuestAlloc(pages);
}

// =====================================================================
// The memory map Windows reports (VirtualQuery and friends)
// =====================================================================

void WardenEmu::Impl::SetMemoryRange(const MemoryRegion& region) {
    std::vector<MemoryRegion> updated;
    updated.reserve(m_memoryMap.size() + 2);
    uint64_t regionEnd = (uint64_t)region.base + region.size;

    for (const MemoryRegion& existing : m_memoryMap) {
        uint64_t existingEnd = (uint64_t)existing.base + existing.size;

        if (existingEnd <= region.base || existing.base >= regionEnd) {
            updated.push_back(existing);
            continue;
        }

        if (existing.base < region.base) {
            MemoryRegion left = existing;
            left.size = region.base - existing.base;
            updated.push_back(left);
        }

        if (existingEnd > regionEnd) {
            MemoryRegion right = existing;
            right.base = (uint32_t)regionEnd;
            right.size = (uint32_t)(existingEnd - regionEnd);
            updated.push_back(right);
        }
    }

    if (region.size) {
        updated.push_back(region);
    }

    std::sort(updated.begin(), updated.end(),
        [](const MemoryRegion& a, const MemoryRegion& b) { return a.base < b.base; });
    m_memoryMap.swap(updated);
}

// One committed allocation of its own (VirtualAlloc semantics: the allocation base is its start).
void WardenEmu::Impl::AddAllocation(uint32_t base, uint32_t size, uint32_t protect, uint32_t type) {
    MemoryRegion region = { base, (size + 0xFFF) & ~0xFFFu, base, protect, protect, WIN_MEM_COMMIT, type };
    SetMemoryRange(region);
}

// VirtualProtect: every page in the range must be committed; the old protection is the first
// page's, exactly as the API reports it.
bool WardenEmu::Impl::ProtectMemory(uint32_t address, uint32_t size, uint32_t protect, uint32_t& oldProtect) {
    uint32_t start = address & ~0xFFFu;
    uint64_t end = ((uint64_t)address + (size ? size : 1) + 0xFFF) & ~(uint64_t)0xFFF;
    uint64_t covered = start;
    std::vector<MemoryRegion> pieces;

    for (const MemoryRegion& region : m_memoryMap) {
        uint64_t regionEnd = (uint64_t)region.base + region.size;

        if (regionEnd <= covered || region.base >= end) {
            continue;
        }

        if (region.base > covered || region.state != WIN_MEM_COMMIT) {
            return false; // a gap or an uncommitted page inside the range
        }

        if (pieces.empty()) {
            oldProtect = region.protect;
        }

        MemoryRegion piece = region;
        piece.base = (uint32_t)covered;
        piece.size = (uint32_t)((regionEnd < end ? regionEnd : end) - covered);
        piece.protect = protect;
        pieces.push_back(piece);
        covered += piece.size;

        if (covered >= end) {
            break;
        }
    }

    if (covered < end) {
        return false;
    }

    for (const MemoryRegion& piece : pieces) {
        SetMemoryRange(piece);
    }

    return true;
}

// MEMORY_BASIC_INFORMATION for the page holding address: the run of following pages with the same
// allocation, state, protection and type, or the free gap up to the next allocation.
bool WardenEmu::Impl::QueryMemory(uint32_t address, uint32_t* info) const {
    if (address >= WIN_USER_SPACE_END) {
        return false; // ERROR_INVALID_PARAMETER above lpMaximumApplicationAddress
    }

    uint32_t page = address & ~0xFFFu;
    memset(info, 0, 7 * sizeof(uint32_t));

    for (size_t i = 0; i < m_memoryMap.size(); i++) {
        const MemoryRegion& region = m_memoryMap[i];

        if (page >= region.base + region.size) {
            continue;
        }

        if (page < region.base) {
            info[0] = page;
            info[3] = region.base - page; // free up to this allocation
            info[4] = WIN_MEM_FREE;
            info[5] = WIN_PAGE_NOACCESS;
            return true;
        }

        uint32_t end = region.base + region.size;

        for (size_t j = i + 1; j < m_memoryMap.size(); j++) {
            const MemoryRegion& next = m_memoryMap[j];

            if (next.base != end || next.allocationBase != region.allocationBase || next.state != region.state
                || next.protect != region.protect || next.type != region.type) {
                break;
            }

            end = next.base + next.size;
        }

        info[0] = page;
        info[1] = region.allocationBase;
        info[2] = region.allocationProtect;
        info[3] = end - page;
        info[4] = region.state;
        info[5] = region.state == WIN_MEM_COMMIT ? region.protect : 0;
        info[6] = region.type;
        return true;
    }

    info[0] = page;
    info[3] = WIN_USER_SPACE_END - page;
    info[4] = WIN_MEM_FREE;
    info[5] = WIN_PAGE_NOACCESS;
    return true;
}

// The address space at boot, as the real client process would show it. The client image comes
// from MapClientImage (by section); the module image from Load.
void WardenEmu::Impl::DescribeBootMemory() {
    m_memoryMap.clear();

    // Thread and process blocks, loader data and the static-TLS array: private read/write.
    AddAllocation(ENV_BASE, 0x1000, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);
    AddAllocation(TLS_ARRAY_BASE, 0x1000, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);
    AddAllocation(PEB_BASE, 0x1000, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);
    AddAllocation(TEB_BASE, 0x1000, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);

    // The thread stack, and the client-side data the bridge hands over (the OurLibrary object
    // and the buffers, which live in the client's own data on Windows): private read/write.
    AddAllocation(STACK_BASE, STACK_SIZE, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);
    AddAllocation(MISC_BASE, MISC_SIZE, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);
    AddAllocation(SCRATCH_BASE, SCRATCH_SIZE, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);

    // The heaps MemoryAlloc and the CRT draw from.
    AddAllocation(HEAP_BASE, HEAP_SIZE, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);
    AddAllocation(HEAP_EXT_BASE + 0x1000, HEAP_EXT_SIZE - 0x1000, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);

    // The import stubs stand in for system library code: image memory, execute/read.
    MemoryRegion stubs = { HEAP_EXT_BASE, 0x1000, HEAP_EXT_BASE, WIN_PAGE_EXECUTE_WRITECOPY,
        WIN_PAGE_EXECUTE_READ, WIN_MEM_COMMIT, WIN_MEM_IMAGE };
    SetMemoryRange(stubs);
}

uint32_t WardenEmu::Impl::NewImportStub(const char* dll, const char* name, uint32_t argumentBytes) {
    if (importCount >= sizeof(imports) / sizeof(imports[0]) || nextImportStub * 8 > 0x1000) {
        return 0;
    }

    // Import stubs live at the bottom of the extended area, below the heap bump start: the
    // stub page is code, the rest of the extended region stays data for the guest heap.
    uint32_t address = HEAP_EXT_BASE + nextImportStub * 8;

    // __stdcall by construction: the stub itself cleans the arguments, which is what the
    // patched thunk callers expect from a LoadLibraryA-resolved address.
    uint8_t code[8] = { 0xC2, (uint8_t)(argumentBytes & 0xFF),
        (uint8_t)((argumentBytes >> 8) & 0xFF), 0xC3, 0xC3, 0xC3, 0xC3, 0xC3 };
    WriteMemory(address, code, sizeof(code));

    ImportStub& stub = imports[importCount++];
    memset(&stub, 0, sizeof(stub));
    stub.implemented = true;
    stub.address = address;
    stub.argumentBytes = argumentBytes;
    snprintf(stub.dll, sizeof(stub.dll), "%s", dll);
    snprintf(stub.name, sizeof(stub.name), "%s", name ? name : "<ordinal>");
    ++nextImportStub;
    return address;
}

// The import set with real implementations. argumentBytes follows each function's __stdcall
// signature (the stub cleans the stack). An entry missing here means the load is refused, and
// the refused load lists every missing API, so implementations get added by what real modules
// actually ask for rather than by guesswork.
uint32_t WardenEmu::Impl::KnownImportArgumentBytes(const char* name) {
    struct KnownImport {
        const char* name;
        uint32_t argumentBytes;
    };

    static const KnownImport known[] = {
        // Timing and string helpers the Warden checks call directly.
        { "Sleep", 4 },
        { "GetTickCount", 0 },
        { "LCMapStringA", 24 }, // locale, flags, src, cchSrc, dest, cchDest
        { "lstrlenA", 4 },
        { "lstrcpyA", 8 },
        { "lstrcatA", 8 },
        { "CharUpperBuffA", 8 }, // string, count
        { "MulDiv", 12 }, // a, b, c

        // Memory.
        { "VirtualAlloc", 16 }, // address, size, allocationType, protect
        { "VirtualFree", 12 }, // address, size, freeType
        { "VirtualProtect", 16 }, // address, size, newProtect, &oldProtect
        { "VirtualQuery", 12 }, // address, info, infoSize
        { "ReadProcessMemory", 20 }, // process, base, buffer, size, &read
        { "WriteProcessMemory", 20 }, // process, base, buffer, size, &written
        { "GetProcessHeap", 0 },
        { "HeapFree", 12 }, // heap, flags, pointer

        // Process, thread and TLS.
        { "GetCurrentProcessId", 0 },
        { "GetCurrentThreadId", 0 },
        { "GetCurrentProcess", 0 },
        { "TlsAlloc", 0 },
        { "TlsFree", 4 },
        { "TlsGetValue", 4 },
        { "TlsSetValue", 8 },

        // Environment the module inspects.
        { "GetSystemInfo", 4 }, // &SYSTEM_INFO
        { "GetVersionExA", 4 }, // &OSVERSIONINFOA
        { "QueryPerformanceCounter", 4 }, // &counter
        { "GetSystemTimeAsFileTime", 4 }, // &filetime

        // Dynamic linking.
        { "GetModuleHandleA", 4 }, // name
        { "LoadLibraryA", 4 }, // name
        { "FreeLibrary", 4 }, // module
        { "GetProcAddress", 8 }, // module, name
        { "QueryDosDeviceA", 12 }, // name, buffer, size

        // Handles.
        { "CloseHandle", 4 },
        { "DuplicateHandle", 28 }, // 7 arguments

        // Exception machinery (MSVC CRT).
        { "RaiseException", 16 }, // code, flags, count, arguments
        { "RtlUnwind", 16 }, // frame, targetIp, exceptionRecord, returnValue
        { "UnhandledExceptionFilter", 4 }, // exceptionInfo
        { "SetUnhandledExceptionFilter", 4 }, // filter

        // Process termination.
        { "TerminateProcess", 8 }, // process, exitCode

        // The module links USER32 but its reconciled uses are icon/theme helpers; they must
        // still resolve, and each reports what it can or cannot do honestly.
        { "DestroyIcon", 4 },
        { "DrawTextA", 20 }, // dc, text, count, rect, format
        { "BeginPaint", 8 }, // window, &paintStruct
        { "DefWindowProcA", 16 }, // window, message, wParam, lParam

        // Seen linked (never called) by Warmane's module.
        { "SetSystemTime", 4 }, // &systemTime
    };

    for (const KnownImport& import : known) {
        if (strcasecmp(import.name, name) == 0) {
            return import.argumentBytes;
        }
    }

    return UINT32_MAX;
}

// Whether a Windows process would resolve dll!name at load time: the name is an export of the
// 32-bit system library the import names (WardenSystemExports.h). Library names compare without
// case, as the loader does; export names are exact, as GetProcAddress matches them.
bool WardenEmu::Impl::IsSystemExport(const char* dll, const char* name) {
    const char* const* begin;
    const char* const* end;

    if (strcasecmp(dll, "kernel32.dll") == 0 || strcasecmp(dll, "kernel32") == 0) {
        begin = s_kernel32Exports;
        end = s_kernel32Exports + sizeof(s_kernel32Exports) / sizeof(s_kernel32Exports[0]);
    } else if (strcasecmp(dll, "user32.dll") == 0 || strcasecmp(dll, "user32") == 0) {
        begin = s_user32Exports;
        end = s_user32Exports + sizeof(s_user32Exports) / sizeof(s_user32Exports[0]);
    } else {
        return false;
    }

    return std::binary_search(begin, end, name,
        [](const char* a, const char* b) { return strcmp(a, b) < 0; });
}

// A module call into an import stub: gather the stack arguments, run the real behavior, fill
// EAX. The stub's own `ret n` then cleans the arguments as __stdcall expects.
void WardenEmu::Impl::HandleImport(const ImportStub& stub) {
    uint32_t esp;
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);

    if (!stub.implemented) {
        char reason[320];
        snprintf(reason, sizeof(reason), "module called %s!%s, which the emulator does not implement; the call stops",
            stub.dll, stub.name);
        WardenLog("%s", reason);
        DumpTrace(reason);
        m_faulted = true;
        uc_emu_stop(machine);
        return;
    }

    uint32_t arguments[8];
    uint32_t argumentCount = stub.argumentBytes / 4;

    if (argumentCount > 8) {
        argumentCount = 8;
    }

    for (uint32_t i = 0; i < argumentCount; i++) {
        ReadWord(esp + 4 + i * 4, arguments[i]);
    }

    // Every genuine import call returns into the module image. A return address anywhere else
    // means the stub was reached by a wild branch (a bad function pointer, a corrupted return),
    // so the arguments read here are not API arguments at all: record the path that led here.
    uint32_t caller = 0;
    ReadWord(esp, caller);

    if (caller < IMAGE_BASE || caller >= IMAGE_BASE + imageSize) {
        char reason[256];
        snprintf(reason, sizeof(reason), "%s entered with return address %08x outside the image",
            stub.name, caller);
        DumpTrace(reason);
    }

    uint32_t returnEax = RunImport(stub, arguments, argumentCount);
    uc_reg_write(machine, UC_X86_REG_EAX, &returnEax);
}

uint32_t WardenEmu::Impl::RunImport(const ImportStub& stub, uint32_t* arguments, uint32_t argumentCount) {
    // Windows API names resolve case-insensitively; match that instead of trusting modules to
    // have spelled exactly like the headers.
    if (strcasecmp(stub.name, "Sleep") == 0) {
        WardenLog("Sleep(%u ms)", stub.argumentBytes >= 4 ? arguments[0] : 0);
        // Waiting pauses the module on Windows only; in the synchronous emulator nothing can
        // race the module, so the pause has no observable effect.
        return 0;
    }

    if (strcasecmp(stub.name, "GetTickCount") == 0) {
        // Real monotonic milliseconds, the same query the module issues on Windows.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint32_t)(now.tv_nsec / 1000000u));
    }

    if (strcasecmp(stub.name, "LCMapStringA") == 0 && argumentCount >= 6) {
        uint32_t srcAddress = arguments[2];
        int32_t cchSrc = (int32_t)arguments[3];
        uint32_t destAddress = arguments[4];
        int32_t cchDest = (int32_t)arguments[5];

        WardenLog("LCMapStringA args: locale=%08x flags=%08x src=%08x cchSrc=%d dest=%08x cchDest=%d",
            arguments[0], arguments[1], srcAddress, cchSrc, destAddress, cchDest);

        const bool uppercase = (arguments[1] & 0x200) != 0; // LCMAP_UPPERCASE
        const bool lowercase = (arguments[1] & 0x100) != 0; // LCMAP_LOWERCASE

        // Anything beyond the plain case maps is refused the way the API refuses impossible
        // work: a zero return and an untouched buffer, so the module sees a real failure
        // rather than a made-up answer.
        if ((arguments[1] & ~(0x200u | 0x100u)) != 0 || (uppercase && lowercase)) {
            WardenLog("LCMapStringA: unsupported mapping flags %08x", arguments[1]);
            return 0;
        }

        char source[1024];
        uint32_t sourceLength;

        if (cchSrc < 0) {
            sourceLength = ReadGuestString(srcAddress, source, sizeof(source));

            if (!sourceLength) {
                WardenLog("LCMapStringA: unreadable source %08x", srcAddress);
                return 0;
            }
        } else {
            if (cchSrc == 0 || (uint32_t)cchSrc > sizeof(source)) {
                WardenLog("LCMapStringA: bad source length %d", cchSrc);
                return 0;
            }

            sourceLength = (uint32_t)cchSrc;

            if (!ReadMemory(srcAddress, source, sourceLength)) {
                WardenLog("LCMapStringA: unreadable source %08x", srcAddress);
                return 0;
            }
        }

        const uint32_t requiredLength = sourceLength;

        WardenLog("LCMapStringA flags=%08x src=%08x cchSrc=%d len=%u", arguments[1], srcAddress, cchSrc,
            requiredLength);

        if (cchDest == 0) {
            return requiredLength; // the size-query form
        }

        if (cchDest < (int32_t)requiredLength) {
            WardenLog("LCMapStringA: destination buffer too small (%d < %u)", cchDest, requiredLength);
            return 0;
        }

        for (uint32_t i = 0; i < sourceLength; i++) {
            uint8_t byte = (uint8_t)source[i];

            // ASCII plus the Latin-1 accented range, the same case behaviour of the Western
            // codepages; Turkish-specific pairs are outside what a byte mapping can express.
            if (uppercase) {
                if ((byte >= 'a' && byte <= 'z') || (byte >= 0xE0 && byte <= 0xFE && byte != 0xF7)) {
                    byte -= 0x20;
                }
            } else if (lowercase) {
                if ((byte >= 'A' && byte <= 'Z') || (byte >= 0xC0 && byte <= 0xDE && byte != 0xD7)) {
                    byte += 0x20;
                }
            }

            source[i] = (char)byte;

            if (!byte && cchSrc < 0) {
                // The mapped terminator is the end of the mapped result; the rest of the
                // caller's buffer stays untouched, exactly like the API.
                break;
            }
        }

        if (!WriteMemory(destAddress, source, requiredLength)) {
            WardenLog("LCMapStringA: unwritable destination %08x", destAddress);
            return 0;
        }

        return requiredLength;
    }

    if (strcasecmp(stub.name, "lstrlenA") == 0 && argumentCount >= 1) {
        char text[1024];
        uint32_t length = ReadGuestString(arguments[0], text, sizeof(text));

        if (!length) {
            return UINT32_MAX; // the lstrlen failure value (-1)
        }

        return length - 1;
    }

    if (strcasecmp(stub.name, "lstrcpyA") == 0 && argumentCount >= 2) {
        char text[1024];
        uint32_t length = ReadGuestString(arguments[1], text, sizeof(text));

        if (!length || !WriteMemory(arguments[0], text, length)) {
            WardenLog("lstrcpyA failed on %08x <- %08x", arguments[0], arguments[1]);
            return 0;
        }

        return arguments[0];
    }

    if (strcasecmp(stub.name, "lstrcatA") == 0 && argumentCount >= 2) {
        char destination[1024];
        char addition[1024];
        uint32_t destinationLength = ReadGuestString(arguments[0], destination, sizeof(destination));
        uint32_t additionLength = ReadGuestString(arguments[1], addition, sizeof(addition));

        if (!destinationLength || !additionLength ||
            destinationLength + additionLength - 1 > sizeof(destination)) {
            WardenLog("lstrcatA failed on %08x <- %08x", arguments[0], arguments[1]);
            return 0;
        }

        memcpy(destination + destinationLength - 1, addition, additionLength);

        if (!WriteMemory(arguments[0], destination, destinationLength + additionLength - 1)) {
            WardenLog("lstrcatA unwritable destination %08x", arguments[0]);
            return 0;
        }

        return arguments[0];
    }

    if (strcasecmp(stub.name, "VirtualAlloc") == 0 && argumentCount >= 4) {
        uint32_t requestedAddress = arguments[0];
        uint32_t size = arguments[1];

        // A specific address can only be honoured when the bump heap happens to be there; the
        // honest answer for an unavailable region is failure, which is what a real allocator
        // returns. Modules in practice pass null and take whatever comes back.
        if (requestedAddress) {
            WardenLog("VirtualAlloc(%08x, %u): fixed-address requests are not available", requestedAddress, size);
            return 0;
        }

        uint32_t address = GuestAllocPages(size);

        if (address) {
            // MEM_RESERVE alone leaves the pages uncommitted; anything with MEM_COMMIT commits.
            bool commit = (arguments[2] & WIN_MEM_COMMIT) != 0;
            MemoryRegion region = { address, (size + 0xFFF) & ~0xFFFu, address, arguments[3],
                commit ? arguments[3] : 0, commit ? WIN_MEM_COMMIT : WIN_MEM_RESERVE, WIN_MEM_PRIVATE };
            SetMemoryRange(region);
            WardenLog("VirtualAlloc(%u, type %08x, protect %08x) -> %08x", size, arguments[2], arguments[3], address);
        }

        return address;
    }

    if (strcasecmp(stub.name, "VirtualFree") == 0 && argumentCount >= 3) {
        // The bump heap keeps the bytes, but the address space reads as Windows leaves it:
        // MEM_RELEASE frees the whole allocation, MEM_DECOMMIT turns the pages back to reserved.
        uint32_t address = arguments[0];

        if (arguments[2] & WIN_MEM_RELEASE) {
            std::vector<MemoryRegion> kept;
            bool found = false;

            for (const MemoryRegion& region : m_memoryMap) {
                if (region.allocationBase == address && region.type == WIN_MEM_PRIVATE) {
                    found = true;
                } else {
                    kept.push_back(region);
                }
            }

            m_memoryMap.swap(kept);
            return found ? 1 : 0;
        }

        if (arguments[2] & WIN_MEM_DECOMMIT) {
            uint32_t oldProtect;

            if (!ProtectMemory(address, arguments[1], 0, oldProtect)) {
                return 0;
            }

            uint64_t end = ((uint64_t)address + (arguments[1] ? arguments[1] : 1) + 0xFFF) & ~(uint64_t)0xFFF;

            for (MemoryRegion& region : m_memoryMap) {
                if (region.base >= (address & ~0xFFFu) && region.base < end && region.protect == 0
                    && region.state == WIN_MEM_COMMIT) {
                    region.state = WIN_MEM_RESERVE;
                }
            }

            return 1;
        }

        return 0;
    }

    if (strcasecmp(stub.name, "VirtualProtect") == 0 && argumentCount >= 4) {
        // Guest pages stay fully accessible to the emulator; the protection the module sets is
        // what the address space reports from now on, and the old one is the real old one.
        uint32_t oldProtect = 0;

        if (!ProtectMemory(arguments[0], arguments[1], arguments[2], oldProtect)) {
            WardenLog("VirtualProtect(%08x, %u): range is not committed", arguments[0], arguments[1]);
            return 0;
        }

        return WriteWord(arguments[3], oldProtect) ? 1 : 0;
    }

    if (strcasecmp(stub.name, "GetCurrentProcessId") == 0) {
        // The client's real host process identity; Warden only needs something stable.
        return (uint32_t)getpid();
    }

    if (strcasecmp(stub.name, "GetCurrentThreadId") == 0) {
        return (uint32_t)(uintptr_t)pthread_self();
    }

    if ((strcasecmp(stub.name, "ReadProcessMemory") == 0 ||
            strcasecmp(stub.name, "WriteProcessMemory") == 0) &&
        argumentCount >= 5) {
        uint32_t baseAddress = arguments[1];
        uint32_t targetBuffer = arguments[2];
        uint32_t size = arguments[3];
        bool writing = strcasecmp(stub.name, "WriteProcessMemory") == 0;

        if (size > MAX_HOST_TRANSFER_BYTES) {
            WardenLog("%s: size too large (%u)", stub.name, size);
            return 0;
        }

        moduleTransferBuffer.resize(size);

        if (writing) {
            // The module writing into "the process" is writing into our guest memory space.
            if (!ReadMemory(targetBuffer, moduleTransferBuffer.data(), size) ||
                !WriteMemory(baseAddress, moduleTransferBuffer.data(), size)) {
                WardenLog("WriteProcessMemory failed on %08x size %u", baseAddress, size);
                return 0;
            }
        } else {
            if (!ReadMemory(baseAddress, moduleTransferBuffer.data(), size) ||
                !WriteMemory(targetBuffer, moduleTransferBuffer.data(), size)) {
                WardenLog("ReadProcessMemory failed on %08x size %u", baseAddress, size);
                return 0;
            }
        }

        return WriteWord(arguments[4], size) ? 1 : 0;
    }

    if (strcasecmp(stub.name, "MulDiv") == 0 && argumentCount >= 3) {
        int32_t a = (int32_t)arguments[0];
        int32_t b = (int32_t)arguments[1];
        int32_t c = (int32_t)arguments[2];

        if (c == 0) {
            return 0xFFFFFFFF; // the API's division-by-zero result
        }

        // Rounded to nearest, halves away from zero, exactly like the API.
        int64_t product = (int64_t)a * (int64_t)b;
        int64_t divisor = c;
        int64_t result = product >= 0 ? (product + divisor / 2) / divisor : (product - divisor / 2) / divisor;
        return (uint32_t)(int32_t)result;
    }

    if (strcasecmp(stub.name, "CharUpperBuffA") == 0 && argumentCount >= 2) {
        uint32_t address = arguments[0];
        uint32_t count = arguments[1];

        if (count > 65536) {
            WardenLog("CharUpperBuffA: implausible length %u", count);
            return 0;
        }

        for (uint32_t i = 0; i < count; i++) {
            uint8_t byte;

            if (!ReadMemory(address + i, &byte, 1)) {
                return i; // the API stops at the first character it cannot touch
            }

            if ((byte >= 'a' && byte <= 'z') || (byte >= 0xE0 && byte <= 0xFE && byte != 0xF7)) {
                byte -= 0x20;
            }

            if (!WriteMemory(address + i, &byte, 1)) {
                return i;
            }
        }

        return count;
    }

    if (strcasecmp(stub.name, "GetCurrentProcess") == 0) {
        return PSEUDO_CURRENT_PROCESS;
    }

    if (strcasecmp(stub.name, "GetProcessHeap") == 0) {
        return PSEUDO_PROCESS_HEAP;
    }

    if (strcasecmp(stub.name, "HeapFree") == 0) {
        // The module's heap is released with the module; freeing is a no-op that succeeds.
        return 1;
    }

    if (strcasecmp(stub.name, "TlsAlloc") == 0) {
        for (uint32_t i = 0; i < TLS_SLOT_COUNT; i++) {
            if (!m_tlsUsed[i]) {
                m_tlsUsed[i] = true;
                m_tlsValues[i] = 0;
                return i;
            }
        }

        WardenLog("TlsAlloc: no free slots");
        return 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
    }

    if (strcasecmp(stub.name, "TlsFree") == 0) {
        uint32_t index = arguments[0];

        if (index >= TLS_SLOT_COUNT || !m_tlsUsed[index]) {
            return 0;
        }

        m_tlsUsed[index] = false;
        m_tlsValues[index] = 0;
        return 1;
    }

    if (strcasecmp(stub.name, "TlsGetValue") == 0) {
        uint32_t index = arguments[0];
        return index < TLS_SLOT_COUNT ? m_tlsValues[index] : 0;
    }

    if (strcasecmp(stub.name, "TlsSetValue") == 0) {
        uint32_t index = arguments[0];

        if (index >= TLS_SLOT_COUNT) {
            return 0;
        }

        m_tlsValues[index] = arguments[1];
        return 1;
    }

    if (strcasecmp(stub.name, "QueryPerformanceCounter") == 0 && argumentCount >= 1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t counter = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;

        if (!WriteWord(arguments[0], (uint32_t)counter) ||
            !WriteWord(arguments[0] + 4, (uint32_t)(counter >> 32))) {
            return 0;
        }

        return 1;
    }

    if (strcasecmp(stub.name, "GetSystemTimeAsFileTime") == 0 && argumentCount >= 1) {
        // FILETIME counts 100 ns ticks since 1601-01-01; assembled from the real wall clock.
        uint64_t seconds = (uint64_t)time(nullptr) + 11644473600ull;
        uint64_t ticks = seconds * 10000000ull;

        if (!WriteWord(arguments[0], (uint32_t)ticks) ||
            !WriteWord(arguments[0] + 4, (uint32_t)(ticks >> 32))) {
            return 0;
        }

        return 1;
    }

    if (strcasecmp(stub.name, "GetSystemInfo") == 0 && argumentCount >= 1) {
        uint32_t info[9];
        memset(info, 0, sizeof(info));

        info[0] = 0;                    // PROCESSOR_ARCHITECTURE_INTEL
        info[1] = 4096;                 // dwPageSize
        info[2] = 0x00010000;           // lpMinimumApplicationAddress
        info[3] = 0x7FFEFFFF;           // lpMaximumApplicationAddress
        info[4] = 1;                    // dwActiveProcessorMask
        info[5] = 1;                    // dwNumberOfProcessors
        info[6] = 586;                  // dwProcessorType (Pentium)
        info[7] = 65536;                // dwAllocationGranularity
        info[8] = (uint32_t)6 | (0x3A09u << 16); // wProcessorLevel | wProcessorRevision

        return WriteMemory(arguments[0], info, sizeof(info)) ? 1 : 0;
    }

    if (strcasecmp(stub.name, "GetVersionExA") == 0 && argumentCount >= 1) {
        uint32_t out = arguments[0];
        uint32_t declaredSize = 0;

        if (!ReadWord(out, declaredSize) || declaredSize < 148) {
            WardenLog("GetVersionExA: bad version info size %u", declaredSize);
            return 0;
        }

        // The emulated environment presents the Windows release the 3.3.5a client was built
        // against. The module reads these values back and reports them; there is no host OS
        // version that would mean anything to it.
        uint8_t info[148];
        memset(info, 0, sizeof(info));

        uint32_t major = 6;    // Windows 7
        uint32_t minor = 1;
        uint32_t build = 7601;
        uint32_t platform = 2; // VER_PLATFORM_WIN32_NT
        memcpy(info + 0, &declaredSize, 4);
        memcpy(info + 4, &major, 4);
        memcpy(info + 8, &minor, 4);
        memcpy(info + 12, &build, 4);
        memcpy(info + 16, &platform, 4);

        return WriteMemory(out, info, sizeof(info)) ? 1 : 0;
    }

    if (strcasecmp(stub.name, "GetModuleHandleA") == 0) {
        char name[128] = { 0 };

        // NULL names the process executable, which is the client, not the Warden module: the
        // module is loaded by the client's BLL loader and is no loader-registered image.
        if (!arguments[0] || !ReadGuestString(arguments[0], name, sizeof(name))) {
            return m_clientBase;
        }

        if (strcasecmp(name, "wow.exe") == 0 || strcasecmp(name, "wow") == 0) {
            return m_clientBase;
        }

        if (strcasecmp(name, "kernel32.dll") == 0 || strcasecmp(name, "kernel32") == 0) {
            return PSEUDO_KERNEL32;
        }

        if (strcasecmp(name, "user32.dll") == 0 || strcasecmp(name, "user32") == 0) {
            return PSEUDO_USER32;
        }

        WardenLog("GetModuleHandleA(%s): no such module loaded", name);
        return 0;
    }

    if (strcasecmp(stub.name, "LoadLibraryA") == 0) {
        char name[128] = { 0 };

        if (!ReadGuestString(arguments[0], name, sizeof(name))) {
            return 0;
        }

        if (strcasecmp(name, "kernel32.dll") == 0 || strcasecmp(name, "kernel32") == 0) {
            return PSEUDO_KERNEL32;
        }

        if (strcasecmp(name, "user32.dll") == 0 || strcasecmp(name, "user32") == 0) {
            return PSEUDO_USER32;
        }

        WardenLog("LoadLibraryA(%s): only the emulated system libraries are available", name);
        return 0;
    }

    if (strcasecmp(stub.name, "FreeLibrary") == 0) {
        return 1;
    }

    if (strcasecmp(stub.name, "GetProcAddress") == 0 && argumentCount >= 2) {
        char name[128] = { 0 };

        if (!ReadGuestString(arguments[1], name, sizeof(name))) {
            return 0;
        }

        uint32_t stubAddress = StubForName(name);

        if (!stubAddress) {
            WardenLog("GetProcAddress(%s): not implemented by the emulator", name);
        }

        return stubAddress;
    }

    if (strcasecmp(stub.name, "QueryDosDeviceA") == 0 && argumentCount >= 3) {
        char name[128] = { 0 };

        if (!ReadGuestString(arguments[0], name, sizeof(name))) {
            return 0;
        }

        // The emulated process has no Windows device namespace; an unknown device is what the
        // API returns for a device that does not exist.
        WardenLog("QueryDosDeviceA(%s): no device namespace in the emulated environment", name);
        return 0;
    }

    if (strcasecmp(stub.name, "CloseHandle") == 0) {
        return 1;
    }

    if (strcasecmp(stub.name, "DuplicateHandle") == 0 && argumentCount >= 7) {
        // Handles here are stable values without a kernel table, so a duplicate is the same
        // value; every later use keeps referring to the same emulated object.
        return WriteWord(arguments[3], arguments[1]) ? 1 : 0;
    }

    if (strcasecmp(stub.name, "VirtualQuery") == 0 && argumentCount >= 3) {
        uint32_t info[7];

        if (!QueryMemory(arguments[0], info)) {
            return 0;
        }

        uint32_t responseSize = 28; // sizeof(MEMORY_BASIC_INFORMATION) on 32-bit

        if (arguments[2] < responseSize) {
            return 0; // ERROR_BAD_LENGTH: the API writes nothing into a short buffer
        }

        return WriteMemory(arguments[1], info, responseSize) ? responseSize : 0;
    }

    if (strcasecmp(stub.name, "SetUnhandledExceptionFilter") == 0) {
        uint32_t previous = m_unhandledFilter;
        m_unhandledFilter = arguments[0];
        return previous;
    }

    if (strcasecmp(stub.name, "UnhandledExceptionFilter") == 0) {
        // The default filter terminates the process; report that decision.
        return 1; // EXCEPTION_EXECUTE_HANDLER
    }

    if (strcasecmp(stub.name, "RaiseException") == 0) {
        WardenLog("RaiseException(%08x): guest SEH unwinding is not available, the call aborts",
            arguments[0]);
        m_faulted = true;
        uc_emu_stop(machine);
        return 0;
    }

    if (strcasecmp(stub.name, "RtlUnwind") == 0) {
        WardenLog("RtlUnwind: guest SEH unwinding is not available, the call aborts");
        m_faulted = true;
        uc_emu_stop(machine);
        return 0;
    }

    if (strcasecmp(stub.name, "TerminateProcess") == 0) {
        WardenLog("TerminateProcess(%u): the module asked to end the process", arguments[1]);
        m_faulted = true;
        uc_emu_stop(machine);
        return 1;
    }

    if (strcasecmp(stub.name, "DestroyIcon") == 0) {
        return 1;
    }

    if (strcasecmp(stub.name, "DrawTextA") == 0) {
        WardenLog("DrawTextA: the emulated environment has no device context");
        return 0;
    }

    // The module owns no window, so every handle it could pass is invalid: BeginPaint returns
    // no device context and DefWindowProc has nothing to process, as Windows answers for an
    // invalid window.
    if (strcasecmp(stub.name, "BeginPaint") == 0) {
        WardenLog("BeginPaint(%08x): no such window", arguments[0]);
        return 0;
    }

    if (strcasecmp(stub.name, "DefWindowProcA") == 0) {
        WardenLog("DefWindowProcA(%08x, %04x): no such window", arguments[0], arguments[1]);
        return 0;
    }

    // Setting the clock needs SeSystemtimePrivilege, which the client process does not hold;
    // Windows fails the call (ERROR_PRIVILEGE_NOT_HELD) and the host clock is never touched.
    if (strcasecmp(stub.name, "SetSystemTime") == 0) {
        WardenLog("SetSystemTime: the process holds no privilege to set the clock; the call fails");
        return 0;
    }

    WardenLog("import %s!%s was called but has no behavior; returning 0", stub.dll, stub.name);
    return 0;
}

// A function address for GetProcAddress-style dynamic lookups: an already-created import stub
// when one exists, otherwise a fresh stub for a name the emulator implements.
uint32_t WardenEmu::Impl::StubForName(const char* name) {
    for (uint32_t i = 0; i < importCount; i++) {
        if (strcasecmp(imports[i].name, name) == 0) {
            return imports[i].address;
        }
    }

    uint32_t argumentBytes = KnownImportArgumentBytes(name);

    if (argumentBytes == UINT32_MAX) {
        return 0;
    }

    return NewImportStub("kernel32.dll", name, argumentBytes);
}

// The OurLibrary stubs: one code stub per vtable slot. The hooks perform the work and the stub
// itself performs the `ret n` (callee-style, matching MSVC __thiscall).
void WardenEmu::Impl::SetupStubs() {
    for (uint32_t i = 0; i < LIB_SLOT_COUNT; i++) {
        uint8_t code[8] = { 0xC2, (uint8_t)(s_libSlots[i].argumentBytes & 0xFF),
            (uint8_t)((s_libSlots[i].argumentBytes >> 8) & 0xFF), 0xC3, 0xC3, 0xC3, 0xC3, 0xC3 };
        WriteMemory(LIB_STUBS + i * 8, code, sizeof(code));
    }
}

// =====================================================================
// Calling into guest code
// =====================================================================

bool WardenEmu::Impl::CallGuest(uint32_t address, const uint32_t* stackArguments, uint32_t argumentCount,
    uint32_t ecx, const char* what, uint32_t& eax) {
    if (argumentCount > 16) {
        WardenLog("%s: too many arguments (%u)", what, argumentCount);
        return false;
    }

    if (!m_replaying) {
        CaptureSnapshot(address, stackArguments, argumentCount, ecx, what);
    }

    uint32_t block[17];
    block[0] = STOP_ADDRESS;

    for (uint32_t i = 0; i < argumentCount; i++) {
        block[i + 1] = stackArguments[i];
    }

    uint32_t esp = STACK_TOP - 4 * (argumentCount + 1);
    WriteMemory(esp, block, (argumentCount + 1) * 4);

    uc_reg_write(machine, UC_X86_REG_ESP, &esp);
    uint32_t ebp = esp + 4;
    uc_reg_write(machine, UC_X86_REG_EBP, &ebp);
    uc_reg_write(machine, UC_X86_REG_ECX, &ecx);

    m_faulted = false;
    uc_err error = uc_emu_start(machine, address, STOP_ADDRESS, 0, 50000000);

    uc_reg_read(machine, UC_X86_REG_EAX, &(eax = 0));

    if (!m_faulted && error == UC_ERR_OK) {
        return true;
    }

    char reason[160];

    if (m_faulted) {
        snprintf(reason, sizeof(reason), "%s at %08x: the module took a path the emulator cannot follow (see above)",
            what, address);
    } else {
        // The entry address alone does not say where it went wrong; read the faulting
        // instruction pointer and the address it touched so the site can be disassembled.
        uint32_t eip = 0;
        uc_reg_read(machine, UC_X86_REG_EIP, &eip);
        snprintf(reason, sizeof(reason), "%s at %08x: emu error %s (faulting EIP %08x)", what, address,
            uc_strerror(error), eip);
    }

    WardenLog("%s", reason);
    DumpTrace(reason);

    // The first failure of each emulator keeps the state it started from, for ReplaySnapshot.
    if (!m_replaying && !m_snapshotWritten) {
        m_snapshotWritten = WriteSnapshot("Logs/warden-snapshot.bin") || WriteSnapshot("warden-snapshot.bin");
    }

    return false;
}

// =====================================================================
// Hook handlers
// =====================================================================

// The module branched into the client executable. A bridged client function runs as this
// client's own implementation and returns to the caller exactly like the __cdecl original
// (EAX holds the result, the caller pops its arguments). Any other client address would need
// the Windows client's x86 code, which is not this client: the call stops and is logged.
void WardenEmu::Impl::HandleClientCode(uint32_t address) {
    uint32_t esp = 0;
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);

    uint32_t frame[4] = { 0 }; // return address, then the first stack arguments
    uc_mem_read(machine, esp, frame, sizeof(frame));

    for (uint32_t i = 0; i < CLIENT_FUNCTION_COUNT; i++) {
        const ClientFunction& function = s_clientFunctions[i];

        if (function.address != address || !m_clientBridged[i]) {
            continue;
        }

        uint32_t result = 0;

        switch (function.id) {
            case CLIENT_OS_GET_ASYNC_TIME_MS: {
                result = services->ClientTimeMs();
                break;
            }
            case CLIENT_OS_LOAD_LIBRARY: {
                // OsLoadLibrary is LoadLibraryA on the given name; the emulated LoadLibraryA is
                // the process's only loader.
                uint32_t stubAddress = StubForName("LoadLibraryA");

                for (uint32_t j = 0; j < importCount; j++) {
                    if (imports[j].address == stubAddress) {
                        result = RunImport(imports[j], &frame[1], 1);
                        break;
                    }
                }

                char name[128] = { 0 };
                ReadGuestString(frame[1], name, sizeof(name));
                WardenLog("client %s(%s) -> %08x", function.name, name, result);
                break;
            }
        }

        uint32_t returnAddress = frame[0];
        esp += 4;
        uc_reg_write(machine, UC_X86_REG_EAX, &result);
        uc_reg_write(machine, UC_X86_REG_ESP, &esp);
        uc_reg_write(machine, UC_X86_REG_EIP, &returnAddress);
        return;
    }

    char reason[192];
    snprintf(reason, sizeof(reason),
        "module called client code at %08x (client RVA %06x) with no native bridge; stack %08x %08x %08x %08x",
        address, address - m_clientBase, frame[0], frame[1], frame[2], frame[3]);
    WardenLog("%s", reason);
    DumpTrace(reason);
    m_faulted = true;
    uc_emu_stop(machine);
}

void WardenEmu::Impl::HandleCodeHook(uint64_t address) {
    if (address < LIB_STUBS || address >= LIB_STUBS + LIB_SLOT_COUNT * 8) {
        // Import stub calls land here: run the real behavior, then the stub's `ret n` follows
        // with EAX filled in.
        if (address >= HEAP_EXT_BASE && address < HEAP_EXT_BASE + 0x1000) {
            for (uint32_t i = 0; i < importCount; i++) {
                if (imports[i].address == (uint32_t)address) {
                    HandleImport(imports[i]);
                    break;
                }
            }
        }

        return;
    }

    uint32_t slot = (uint32_t)((address - LIB_STUBS) / 8);
    const LibSlotInfo& slotInfo = s_libSlots[slot];

    // Arguments in MSVC __thiscall order: ECX = this, then the arguments. The stub's own
    // `ret n` takes care of stack cleanup once the hook returns.
    uint32_t esp;
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);

    uint32_t arguments[4];
    uint32_t argumentCount = slotInfo.argumentBytes / 4;

    for (uint32_t i = 0; i < argumentCount; i++) {
        ReadWord(esp + 4 + i * 4, arguments[i]);
    }

    uint32_t returnEax = 0;

    switch (slot) {
        case 0: { // Data(data, size)
            uint32_t size = arguments[1];

            if (size > MAX_HOST_TRANSFER_BYTES) {
                WardenLog("module Data too large: %u", size);
                break;
            }

            moduleTransferBuffer.resize(size);
            ReadMemory(arguments[0], moduleTransferBuffer.data(), size);
            services->ModuleOutput(moduleTransferBuffer.data(), size);
            break;
        }
        case 1: { // ModuleUse(id, key)
            uint8_t id[16];
            uint8_t key[16];
            ReadMemory(arguments[0], id, sizeof(id));
            ReadMemory(arguments[1], key, sizeof(key));
            returnEax = services->ModuleUse(id, key) ? 1 : 0;
            break;
        }
        case 2: { // ModuleCache(id, data, size)
            uint8_t id[16];
            uint32_t size = arguments[2];
            ReadMemory(arguments[0], id, sizeof(id));

            if (size > MAX_HOST_TRANSFER_BYTES) {
                WardenLog("module cache data too large: %u", size);
                break;
            }

            moduleTransferBuffer.resize(size);
            ReadMemory(arguments[1], moduleTransferBuffer.data(), size);
            services->ModuleCache(id, moduleTransferBuffer.data(), size);
            break;
        }
        case 3: { // MemoryAlloc(size)
            returnEax = GuestAlloc(arguments[0]);
            break;
        }
        case 4: { // MemoryFree: the bump allocator releases everything with the module. The
            // running module is the only user of that memory, so handing back the same address
            // keeps its own observations consistent.
            returnEax = arguments[0];
            break;
        }
        case 5: { // StateSave(data, size)
            uint32_t size = arguments[1];

            if (size > MAX_HOST_TRANSFER_BYTES) {
                WardenLog("state save too large: %u", size);
                break;
            }

            moduleTransferBuffer.resize(size);
            ReadMemory(arguments[0], moduleTransferBuffer.data(), size);
            services->StateSave(moduleTransferBuffer.data(), size);
            break;
        }
        case 6: { // StateLoad(data, &size)
            uint32_t dataAddress = arguments[0];
            uint32_t sizeAddress = arguments[1];
            uint32_t wanted = 0;
            ReadWord(sizeAddress, wanted);

            if (wanted > MAX_HOST_TRANSFER_BYTES) {
                WardenLog("state load too large: %u", wanted);
                WriteWord(sizeAddress, 0);
                break;
            }

            moduleTransferBuffer.resize(wanted);
            uint32_t sizeInOut = wanted;
            bool result = services->StateLoad(moduleTransferBuffer.data(), sizeInOut);

            if (result && sizeInOut) {
                WriteMemory(dataAddress, moduleTransferBuffer.data(), sizeInOut);
            }

            WriteWord(sizeAddress, sizeInOut);
            returnEax = result ? 1 : 0;
            break;
        }
        default: {
            WardenLog("our library slot %u (%s) is called but not implemented", slot, slotInfo.name);
            break;
        }
    }

    uc_reg_write(machine, UC_X86_REG_EAX, &returnEax);
}

bool WardenEmu::Impl::HandleInvalidMemory(uc_mem_type type, uint64_t address, int size, int64_t value) {
    const char* kinds[] = { "read", "write", "fetch", "read unchanged", "write unchanged" };
    const char* kind = type < UC_MEM_READ_UNMAPPED || type > UC_MEM_WRITE_PROT ? "access" : kinds[type - UC_MEM_READ_UNMAPPED];

    WardenLog("guest %s fault at %016llx size %d value %lld", kind, (unsigned long long)address, size,
        (long long)value);
    return false;
}

// =====================================================================
// Diagnostics: branch trace and replayable snapshots
// =====================================================================

// One ring entry per block, except that a run of blocks outside the image (code sliding through
// unused memory after a wild branch) folds into the entry where the run began: the branch that
// left the image stays in the history however long the run is.
void WardenEmu::Impl::RecordBlock(uint32_t address, uint32_t esp) {
    bool inImage = address >= IMAGE_BASE && address < IMAGE_BASE + imageSize;

    if (!inImage && m_traceNext) {
        uint32_t last = (m_traceNext - 1) % TRACE_LENGTH;
        uint32_t lastAddress = m_traceAddress[last];
        bool lastInImage = lastAddress >= IMAGE_BASE && lastAddress < IMAGE_BASE + imageSize;

        if (!lastInImage && esp == m_traceStack[last]) {
            ++m_traceRepeat[last];
            return;
        }
    }

    uint32_t slot = m_traceNext++ % TRACE_LENGTH;
    m_traceAddress[slot] = address;
    m_traceStack[slot] = esp;
    m_traceRepeat[slot] = 0;
}

void WardenEmu::Impl::DumpTrace(const char* reason) {
    // A module stuck in a failing loop would otherwise repeat the same dump every frame; the
    // first few carry everything, and a replay always gets its own.
    if (!m_replaying && m_traceDumps >= 4) {
        return;
    }

    ++m_traceDumps;

    uint32_t registers[9];
    const int ids[9] = { UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
        UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP };

    for (uint32_t i = 0; i < 9; i++) {
        registers[i] = 0;
        uc_reg_read(machine, ids[i], &registers[i]);
    }

    WardenLog("trace: %s", reason);
    WardenLog("trace: eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x",
        registers[0], registers[1], registers[2], registers[3], registers[4], registers[5], registers[6],
        registers[7], registers[8]);

    uint32_t stackWords[12] = { 0 };
    uc_mem_read(machine, registers[7], stackWords, sizeof(stackWords));
    WardenLog("trace: [esp] %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", stackWords[0],
        stackWords[1], stackWords[2], stackWords[3], stackWords[4], stackWords[5], stackWords[6],
        stackWords[7], stackWords[8], stackWords[9], stackWords[10], stackWords[11]);

    // Oldest first, so the log reads in execution order; image addresses also carry the RVA
    // that a disassembly of the module image uses.
    uint32_t count = m_traceNext < TRACE_LENGTH ? m_traceNext : TRACE_LENGTH;
    uint32_t shown = m_replaying ? count : (count < 64 ? count : 64);

    for (uint32_t i = count - shown; i < count; i++) {
        uint32_t slot = (m_traceNext - count + i) % TRACE_LENGTH;
        uint32_t blockAddress = m_traceAddress[slot];

        if (imageSize && blockAddress >= IMAGE_BASE && blockAddress < IMAGE_BASE + imageSize) {
            WardenLog("trace:   %08x rva %04x esp %08x", blockAddress, blockAddress - IMAGE_BASE,
                m_traceStack[slot]);
        } else if (m_traceRepeat[slot]) {
            WardenLog("trace:   %08x          esp %08x  (+%u more blocks outside the image)", blockAddress,
                m_traceStack[slot], m_traceRepeat[slot]);
        } else {
            WardenLog("trace:   %08x          esp %08x", blockAddress, m_traceStack[slot]);
        }
    }
}

void WardenEmu::Impl::CaptureSnapshot(uint32_t address, const uint32_t* stackArguments,
    uint32_t argumentCount, uint32_t ecx, const char* what) {
    Snapshot& snapshot = m_snapshot;
    snapshot.entry = address;
    snapshot.ecx = ecx;
    snapshot.argumentCount = argumentCount;

    for (uint32_t i = 0; i < 16; i++) {
        snapshot.arguments[i] = i < argumentCount ? stackArguments[i] : 0;
    }

    snprintf(snapshot.what, sizeof(snapshot.what), "%s", what);

    const int ids[9] = { UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
        UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS };

    for (uint32_t i = 0; i < 9; i++) {
        snapshot.registers[i] = 0;
        uc_reg_read(machine, ids[i], &snapshot.registers[i]);
    }

    // Every byte the guest can observe: the environment pages, the whole stack (uninitialised
    // locals included, so a replay sees the same garbage), the bridge areas, and the used part
    // of the image and both heaps.
    const struct {
        uint32_t base;
        uint32_t size;
    } regions[] = {
        { ENV_BASE, 0x1000 },
        { TLS_ARRAY_BASE, 0x1000 },
        { PEB_BASE, 0x1000 },
        { TEB_BASE, 0x1000 },
        { GDT_BASE, 0x1000 },
        { STACK_BASE, STACK_SIZE },
        { MISC_BASE, MISC_SIZE },
        { SCRATCH_BASE, SCRATCH_SIZE },
        { HEAP_BASE, m_heapNext[0] - HEAP_BASE },
        { IMAGE_BASE, imageSize },
        { HEAP_EXT_BASE, m_heapNext[1] - HEAP_EXT_BASE },
    };

    const uint32_t regionCount = sizeof(regions) / sizeof(regions[0]);
    snapshot.regions.resize(regionCount);

    for (uint32_t i = 0; i < regionCount; i++) {
        snapshot.regions[i].base = regions[i].base;
        snapshot.regions[i].bytes.resize(regions[i].size);

        if (regions[i].size) {
            uc_mem_read(machine, regions[i].base, snapshot.regions[i].bytes.data(), regions[i].size);
        }
    }
}

namespace {

const char SNAPSHOT_MAGIC[8] = { 'W', 'E', 'M', 'U', 'S', 'N', 'P', '3' };

}

bool WardenEmu::Impl::WriteSnapshot(const char* path) const {
    const Snapshot& snapshot = m_snapshot;

    if (snapshot.regions.empty()) {
        return false;
    }

    FILE* file = fopen(path, "wb");

    if (!file) {
        return false;
    }

    uint32_t scalars[13] = { imageLoaded ? 1u : 0u, moduleCreated ? 1u : 0u, imageSize, exportAddress,
        moduleObject, m_heapNext[0], m_heapNext[1], m_heapLimit[0], m_heapLimit[1], heapRegion,
        m_unhandledFilter, importCount, nextImportStub };

    fwrite(SNAPSHOT_MAGIC, 1, sizeof(SNAPSHOT_MAGIC), file);
    fwrite(&snapshot.entry, 4, 1, file);
    fwrite(&snapshot.ecx, 4, 1, file);
    fwrite(&snapshot.argumentCount, 4, 1, file);
    fwrite(snapshot.arguments, 4, 16, file);
    fwrite(snapshot.what, 1, sizeof(snapshot.what), file);
    fwrite(snapshot.registers, 4, 9, file);
    fwrite(scalars, 4, 13, file);
    fwrite(m_tlsValues, 4, TLS_SLOT_COUNT, file);
    fwrite(m_tlsUsed, 1, TLS_SLOT_COUNT, file);
    fwrite(imports, sizeof(ImportStub), importCount, file);

    uint32_t mapCount = (uint32_t)m_memoryMap.size();
    fwrite(&mapCount, 4, 1, file);
    fwrite(m_memoryMap.data(), sizeof(MemoryRegion), mapCount, file);

    uint32_t regionCount = (uint32_t)snapshot.regions.size();
    fwrite(&regionCount, 4, 1, file);

    for (const SnapshotRegion& region : snapshot.regions) {
        uint32_t size = (uint32_t)region.bytes.size();
        fwrite(&region.base, 4, 1, file);
        fwrite(&size, 4, 1, file);
        fwrite(region.bytes.data(), 1, size, file);
    }

    bool ok = ferror(file) == 0;
    fclose(file);

    if (ok) {
        WardenLog("snapshot of the failing %s call written to %s", snapshot.what, path);
    }

    return ok;
}

bool WardenEmu::Impl::ReadSnapshot(const char* path) {
    FILE* file = fopen(path, "rb");

    if (!file) {
        WardenLog("snapshot %s: cannot open", path);
        return false;
    }

    Snapshot& snapshot = m_snapshot;
    char magic[8];
    uint32_t scalars[13];
    bool ok = fread(magic, 1, sizeof(magic), file) == sizeof(magic)
        && memcmp(magic, SNAPSHOT_MAGIC, sizeof(magic)) == 0
        && fread(&snapshot.entry, 4, 1, file) == 1
        && fread(&snapshot.ecx, 4, 1, file) == 1
        && fread(&snapshot.argumentCount, 4, 1, file) == 1
        && snapshot.argumentCount <= 16
        && fread(snapshot.arguments, 4, 16, file) == 16
        && fread(snapshot.what, 1, sizeof(snapshot.what), file) == sizeof(snapshot.what)
        && fread(snapshot.registers, 4, 9, file) == 9
        && fread(scalars, 4, 13, file) == 13
        && fread(m_tlsValues, 4, TLS_SLOT_COUNT, file) == TLS_SLOT_COUNT
        && fread(m_tlsUsed, 1, TLS_SLOT_COUNT, file) == TLS_SLOT_COUNT
        && scalars[11] <= sizeof(imports) / sizeof(imports[0])
        && fread(imports, sizeof(ImportStub), scalars[11], file) == scalars[11];

    snapshot.what[sizeof(snapshot.what) - 1] = '\0';
    uint32_t mapCount = 0;
    ok = ok && fread(&mapCount, 4, 1, file) == 1 && mapCount <= 4096;
    std::vector<MemoryRegion> memoryMap(ok ? mapCount : 0);
    ok = ok && fread(memoryMap.data(), sizeof(MemoryRegion), mapCount, file) == mapCount;

    uint32_t regionCount = 0;
    ok = ok && fread(&regionCount, 4, 1, file) == 1 && regionCount <= 32;
    snapshot.regions.clear();

    for (uint32_t i = 0; ok && i < regionCount; i++) {
        SnapshotRegion region;
        uint32_t size = 0;
        ok = fread(&region.base, 4, 1, file) == 1 && fread(&size, 4, 1, file) == 1 && size <= IMAGE_SIZE;

        if (ok) {
            region.bytes.resize(size);
            ok = fread(region.bytes.data(), 1, size, file) == size;
        }

        if (ok) {
            snapshot.regions.push_back(std::move(region));
        }
    }

    fclose(file);

    if (!ok) {
        WardenLog("snapshot %s: truncated or not a snapshot", path);
        return false;
    }

    imageLoaded = scalars[0] != 0;
    moduleCreated = scalars[1] != 0;
    imageSize = scalars[2];
    exportAddress = scalars[3];
    moduleObject = scalars[4];
    m_heapNext[0] = scalars[5];
    m_heapNext[1] = scalars[6];
    m_heapLimit[0] = scalars[7];
    m_heapLimit[1] = scalars[8];
    heapRegion = scalars[9];
    m_unhandledFilter = scalars[10];
    importCount = scalars[11];
    nextImportStub = scalars[12];
    m_memoryMap.swap(memoryMap);

    for (const SnapshotRegion& region : snapshot.regions) {
        if (!region.bytes.empty() && !WriteMemory(region.base, region.bytes.data(), (uint32_t)region.bytes.size())) {
            return false;
        }
    }

    const int ids[9] = { UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
        UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS };

    for (uint32_t i = 0; i < 9; i++) {
        uc_reg_write(machine, ids[i], &snapshot.registers[i]);
    }

    return true;
}

// =====================================================================
// Loading the BLL image
// =====================================================================

bool WardenEmu::Load(const void* data, uint32_t size) {
    Impl& impl = *m_impl;

    if (impl.imageLoaded) {
        WardenLog("Load called twice on the same emulator instance");
        return false;
    }

    if (!impl.booted && !impl.Boot()) {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t header[12];

    if (size < 40) {
        WardenLog("image too small: %u", size);
        return false;
    }

    memcpy(header, bytes, sizeof(header));

    uint32_t finalSize = header[0];

    if (finalSize == 0 || finalSize > IMAGE_SIZE) {
        WardenLog("image size out of range: %u", finalSize);
        return false;
    }

    // Run-length fill, mirroring the original loader: write the raw 40-byte header, then copy
    // chunks from the compressed tail into the section destinations.
    uint8_t zero[4096];
    memset(zero, 0, sizeof(zero));

    for (uint32_t off = 0; off < finalSize; off += sizeof(zero)) {
        uint32_t chunk = finalSize - off < sizeof(zero) ? finalSize - off : sizeof(zero);
        impl.WriteMemory(IMAGE_BASE + off, zero, chunk);
    }
    impl.WriteMemory(IMAGE_BASE, bytes, 40);

    uint32_t chunkOffset = 4 * (3 * header[9] + 10);
    uint32_t dest = header[10];
    bool copy = true;
    uint32_t guard = 0;

    while (dest < finalSize && guard++ < 1000000) {
        uint16_t length;

        if (chunkOffset + 2 > size) {
            WardenLog("run-length stream overruns the image");
            return false;
        }

        memcpy(&length, bytes + chunkOffset, sizeof(length));
        chunkOffset += 2;

        if (copy) {
            if (chunkOffset + length > size || dest + length > finalSize) {
                WardenLog("run-length chunk overruns the image");
                return false;
            }

            impl.WriteMemory(IMAGE_BASE + dest, bytes + chunkOffset, length);
            chunkOffset += length;
        }

        copy = !copy;
        dest += length;
    }

    if (dest != finalSize) {
        WardenLog("run-length fill ended at %08x instead of %08x", dest, finalSize);
        return false;
    }

    impl.imageSize = finalSize;
    impl.imageLoaded = true;

    // The rest of the image area is not part of the module. A branch into it is a wild branch,
    // and it has to fault on the spot: zero bytes decode as instructions, so executable
    // padding lets a wild branch slide through megabytes of them before anything stops it.
    uint32_t imageEnd = (finalSize + 0xFFF) & ~0xFFFu;

    if (imageEnd < IMAGE_SIZE) {
        uc_mem_protect(impl.machine, IMAGE_BASE + imageEnd, IMAGE_SIZE - imageEnd, UC_PROT_READ | UC_PROT_WRITE);
    }

    // The memory map as BLLLoader::Load leaves it: one read/write VirtualAlloc block, each
    // section given the protection its header entry names, and everything from the relocation
    // table onwards decommitted.
    impl.AddAllocation(IMAGE_BASE, finalSize, WIN_PAGE_READWRITE, WIN_MEM_PRIVATE);

    for (uint32_t i = 0; i < header[9] && 4 * (3 * i + 13) <= size; i++) {
        uint32_t entry[3];
        memcpy(entry, bytes + 4 * (3 * i + 10), sizeof(entry));
        uint32_t oldProtect;

        if (!impl.ProtectMemory(IMAGE_BASE + entry[0], entry[1], entry[2], oldProtect)) {
            WardenLog("section %u (%08x +%08x) protection %08x lies outside the image", i, entry[0], entry[1],
                entry[2]);
        }
    }

    uint32_t decommitStart = (header[2] + 0xFFF) & ~0xFFFu;

    if (header[2] < finalSize && decommitStart < finalSize) {
        Impl::MemoryRegion reserved = { IMAGE_BASE + decommitStart, imageEnd - decommitStart, IMAGE_BASE,
            WIN_PAGE_READWRITE, 0, WIN_MEM_RESERVE, WIN_MEM_PRIVATE };
        impl.SetMemoryRange(reserved);
    }

    // Offline analysis hook: WARDEN_IMAGE_DUMP=<path> writes the relocated-free image (RVAs
    // match Load's addresses) so it can be disassembled without a live session.
    if (const char* dumpPath = getenv("WARDEN_IMAGE_DUMP")) {
        std::vector<uint8_t> raw(finalSize);

        if (impl.ReadMemory(IMAGE_BASE, raw.data(), finalSize)) {
            FILE* dumpFile = fopen(dumpPath, "wb");

            if (dumpFile) {
                fwrite(raw.data(), 1, finalSize, dumpFile);
                fclose(dumpFile);
                WardenLog("image dumped to %s (%u bytes)", dumpPath, finalSize);
            }
        }
    }

    // Relocations, big-endian control words exactly like the original loader.
    uint32_t relOffset = header[2];
    uint32_t relCount = header[3];
    uint32_t offset = 0;

    for (uint32_t i = 0; i < relCount && relOffset < finalSize; i++) {
        uint8_t words[4];
        impl.ReadMemory(IMAGE_BASE + relOffset, words, 4);

        if (words[0] & 0x80) {
            offset = words[3] + ((words[2] + ((words[1] + ((words[0] & 0x7F) << 8)) << 8)) << 8);
            relOffset += 4;
        } else {
            offset += words[1] + (words[0] << 8);
            relOffset += 2;
        }

        if (offset + 4 > finalSize) {
            WardenLog("relocation points past the image: %u", offset);
            return false;
        }

        uint32_t value;
        impl.ReadWord(IMAGE_BASE + offset, value);
        impl.WriteWord(IMAGE_BASE + offset, value + IMAGE_BASE);
    }

    // Imports. The built-in module has none; custom modules resolve to stubs for the few
    // system functions the emulator knows, and anything unknown blocks the load. That is the
    // honest path: pretending to answer unknown imports would fake the module's behaviour.
    uint32_t importOffset = header[7];
    uint32_t importCount = header[8];
    std::vector<std::string> unknowns;

    for (uint32_t i = 0; i < importCount; i++) {
        uint32_t dllPointer;
        uint32_t thunkPointer;
        impl.ReadWord(IMAGE_BASE + importOffset + i * 8, dllPointer);
        impl.ReadWord(IMAGE_BASE + importOffset + i * 8 + 4, thunkPointer);

        char dll[64];
        impl.ReadMemory(IMAGE_BASE + dllPointer, dll, sizeof(dll) - 1);
        dll[sizeof(dll) - 1] = '\0';
        dll[strcspn(dll, "\r\n")] = '\0';

        uint32_t thunk = thunkPointer;

        while (true) {
            uint32_t entry;
            impl.ReadWord(IMAGE_BASE + thunk, entry);

            if (!entry) {
                break;
            }

            char name[192];
            bool byOrdinal = (entry & 0x80000000) != 0;

            if (byOrdinal) {
                snprintf(name, sizeof(name), "ordinal %u", entry & 0x7FFFFFFF);
            } else {
                impl.ReadMemory(IMAGE_BASE + entry, name, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                name[strcspn(name, "\r\n")] = '\0';
            }

            WardenLog("import %s!%s", dll, name);

            uint32_t stub = 0;

            if (!byOrdinal) {
                // Only imports with a real implementation below resolve; everything else is
                // collected and reported once, so one failed load lists every missing API.
                // Module DLL names vary in case between shipped modules ("KERNEL32.dll" is a
                // real example), and Windows resolution is case-insensitive, hence strcasecmp.
                // The emulator implements functions by name, so the DLL the thunk names does
                // not restrict resolution (the real module imports from kernel32 and user32).
                uint32_t argumentBytes = Impl::KnownImportArgumentBytes(name);

                if (argumentBytes != UINT32_MAX) {
                    stub = impl.NewImportStub(dll, name, argumentBytes);
                } else if (Impl::IsSystemExport(dll, name)) {
                    // A real export the module links without the emulator implementing it
                    // (the per-module filler imports retail Warden modules carry: none of
                    // them is ever called). Windows resolves it, so the load goes on; a call
                    // would stop the module instead of returning an invented result.
                    stub = impl.NewImportStub(dll, name, 0);

                    if (stub) {
                        impl.imports[impl.importCount - 1].implemented = false;
                        WardenLog("import %s!%s resolves without emulator behavior; a call to it stops the module",
                            dll, name);
                    }
                }
            }

            if (!stub) {
                unknowns.emplace_back(dll);
                unknowns.back() += "!";
                unknowns.back() += name;
                // Keep resolving the remaining imports so their stubs fill in; the load still
                // fails below, but the log then shows the complete set the module needs.
                impl.WriteWord(IMAGE_BASE + thunk, impl.NewImportStub(dll, name, 4));
                thunk += 4;
                continue;
            }

            impl.WriteWord(IMAGE_BASE + thunk, stub);
            thunk += 4;
        }
    }

    if (!unknowns.empty()) {
        for (const std::string& unknown : unknowns) {
            WardenLog("import %s has no emulator implementation", unknown.c_str());
        }

        WardenLog("module load refused: %u unknown import(s)", (uint32_t)unknowns.size());
        return false;
    }

    // Exports. The stored values stay unrelocated at load time (the builtin module's table is
    // outside the relocation stream) and GetExport adds the image base, mirroring the original
    // loader's pointer arithmetic.
    if (header[5] == 0) {
        WardenLog("image exports nothing");
        return false;
    }

    uint32_t exportRva;
    impl.ReadWord(IMAGE_BASE + header[4], exportRva);
    impl.exportAddress = IMAGE_BASE + exportRva;

    WardenLog("module loaded: imageSize=%u exports=%u imports=%u export=%08x", finalSize, header[5],
        importCount, impl.exportAddress);
    return true;
}

// =====================================================================
// Public API
// =====================================================================

WardenEmu::WardenEmu(WardenEmuServices* services) : m_impl(new Impl) {
    m_impl->services = services;
}

WardenEmu::~WardenEmu() {
    m_impl->services = nullptr;
    m_impl->Teardown();
    delete m_impl;
}

bool WardenEmu::CreateModule() {
    Impl& impl = *m_impl;

    if (!impl.imageLoaded) {
        WardenLog("CreateModule called before Load");
        return false;
    }

    if (impl.moduleCreated) {
        return true;
    }

    uint32_t eax = 0;

    // The export is a __fastcall function; its one argument (OurLibrary*) travels in ECX.
    if (!impl.CallGuest(impl.exportAddress, nullptr, 0, LIB_OBJECT, "CreateWardenModule", eax)) {
        return false;
    }

    // The module allocates its object through OurLibrary::MemoryAlloc, so the pointer lives
    // on the guest heap rather than inside the image; only the vtable must point into the
    // module image.
    if (!eax) {
        WardenLog("CreateWardenModule returned %08x", eax);
        return false;
    }

    uint32_t vtable;
    impl.ReadWord(eax, vtable);

    if (vtable < IMAGE_BASE || vtable >= IMAGE_BASE + IMAGE_SIZE) {
        WardenLog("module vtable pointer invalid: %08x", vtable);
        return false;
    }

    impl.moduleObject = eax;
    impl.moduleCreated = true;
    WardenLog("module created, object %08x vtable %08x", eax, vtable);
    return true;
}

bool WardenEmu::Initialize(const uint8_t* sessionKey, uint32_t keyLength) {
    Impl& impl = *m_impl;

    if (!impl.moduleCreated || keyLength > 0x100) {
        WardenLog("Initialize precondition failed (created=%d keylen=%u)", (int)impl.moduleCreated,
            keyLength);
        return false;
    }

    impl.WriteMemory(SCRATCH_KEY, sessionKey, keyLength);

    uint32_t vtable;
    impl.ReadWord(impl.moduleObject, vtable);

    uint32_t functionAddress;
    impl.ReadWord(vtable, functionAddress);

    uint32_t arguments[2] = { SCRATCH_KEY, keyLength };
    uint32_t eax = 0;

    if (!impl.CallGuest(functionAddress, arguments, 2, impl.moduleObject,
            "WardenModule::Initialize", eax)) {
        return false;
    }

    WardenLog("Initialize done (client key length %u, result %08x)", keyLength, eax);
    return true;
}

void WardenEmu::Shutdown() {
    Impl& impl = *m_impl;

    if (!impl.moduleCreated) {
        return;
    }

    uint32_t vtable;
    impl.ReadWord(impl.moduleObject, vtable);

    uint32_t functionAddress;
    impl.ReadWord(vtable + 4, functionAddress);

    uint32_t eax = 0;
    impl.CallGuest(functionAddress, nullptr, 0, impl.moduleObject, "WardenModule::Shutdown", eax);
}

void WardenEmu::Data(const void* data, uint32_t size, uint32_t& consumed) {
    Impl& impl = *m_impl;
    consumed = 0;

    if (!impl.moduleCreated) {
        WardenLog("Data called without a created module");
        return;
    }

    if (size > MAX_DELIVER_BYTES) {
        WardenLog("Data delivered too large: %u", size);
        return;
    }

    impl.WriteMemory(SCRATCH_DATA, data, size);

    uint32_t vtable;
    impl.ReadWord(impl.moduleObject, vtable);

    uint32_t functionAddress;
    impl.ReadWord(vtable + 8, functionAddress);

    impl.WriteWord(SCRATCH_OUTPUT, 0);

    uint32_t arguments[3] = { SCRATCH_DATA, size, SCRATCH_OUTPUT };
    uint32_t eax = 0;

    if (!impl.CallGuest(functionAddress, arguments, 3, impl.moduleObject, "WardenModule::Data",
            eax)) {
        return;
    }

    impl.ReadWord(SCRATCH_OUTPUT, consumed);
}

void WardenEmu::Process(uint32_t elapsedMs) {
    Impl& impl = *m_impl;

    if (!impl.moduleCreated) {
        return;
    }

    uint32_t vtable;
    impl.ReadWord(impl.moduleObject, vtable);

    uint32_t functionAddress;
    impl.ReadWord(vtable + 12, functionAddress);

    uint32_t arguments[1] = { elapsedMs };
    uint32_t eax = 0;
    impl.CallGuest(functionAddress, arguments, 1, impl.moduleObject, "WardenModule::Process", eax);
}

bool WardenEmu::ReplaySnapshot(const char* path) {
    Impl& impl = *m_impl;

    if (impl.imageLoaded) {
        WardenLog("ReplaySnapshot needs a fresh emulator instance");
        return false;
    }

    if (!impl.booted && !impl.Boot()) {
        return false;
    }

    if (!impl.ReadSnapshot(path)) {
        return false;
    }

    // The replay keeps the full branch history and never overwrites the snapshot it came from.
    impl.m_replaying = true;

    const Impl::Snapshot& snapshot = impl.m_snapshot;
    WardenLog("replaying %s at %08x (ecx %08x, %u argument(s)) from %s", snapshot.what, snapshot.entry,
        snapshot.ecx, snapshot.argumentCount, path);

    uint32_t eax = 0;
    bool ok = impl.CallGuest(snapshot.entry, snapshot.arguments, snapshot.argumentCount, snapshot.ecx,
        snapshot.what, eax);

    WardenLog("replay %s: eax %08x", ok ? "completed" : "failed", eax);
    return ok;
}

bool WardenEmu::ImageLoaded() const {
    return m_impl->imageLoaded;
}

bool WardenEmu::ModuleCreated() const {
    return m_impl->moduleCreated;
}

#endif
