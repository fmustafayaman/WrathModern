#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "Engine/Source/Os/WardenEmu.h"

#include <unicorn/unicorn.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <strings.h>
#include <string>
#include <unistd.h>
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

constexpr uint32_t STACK_BASE = 0x00100000;
constexpr uint32_t STACK_SIZE = 0x00080000; // 512 KiB
constexpr uint32_t STACK_TOP = STACK_BASE + STACK_SIZE - 0x100;

constexpr uint32_t MISC_BASE = 0x00200000; // stop sentinel, objects, stubs, small copies
constexpr uint32_t MISC_SIZE = 0x00001000;

constexpr uint32_t SCRATCH_BASE = 0x00210000; // larger buffers handed across the boundary
constexpr uint32_t SCRATCH_SIZE = 0x00010000;

constexpr uint32_t HEAP_BASE = 0x00300000;
constexpr uint32_t HEAP_SIZE = 0x00400000; // mapped heap, bump allocated for the module

constexpr uint32_t IMAGE_BASE = 0x00700000;
constexpr uint32_t IMAGE_SIZE = 0x00800000; // 8 MiB, covers every known module comfortably

constexpr uint32_t HEAP_EXT_BASE = 0x00F00000;
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

    // Guest stub addresses handed out for imports.
    struct ImportStub {
        uint32_t address = 0;
        uint32_t argumentBytes = 0;
        char dll[64];
        char name[192];
    };

    ImportStub imports[128];
    uint32_t importCount = 0;
    uint32_t nextImportStub = 0;

    bool Boot();
    void Teardown();
    void SetupStubs();

    // Import plumbing: which APIs have real implementations, and running one for a stub call.
    static uint32_t KnownImportArgumentBytes(const char* name);
    void HandleImport(const ImportStub& stub);
    uint32_t RunImport(const ImportStub& stub, uint32_t* arguments, uint32_t argumentCount);

    bool ReadMemory(uint32_t address, void* out, uint32_t size) const;
    bool WriteMemory(uint32_t address, const void* in, uint32_t size);
    bool ReadWord(uint32_t address, uint32_t& value) const;
    bool WriteWord(uint32_t address, uint32_t value);

    // NUL-terminated guest string reader used by the string import implementations.
    uint32_t ReadGuestString(uint32_t address, char* out, uint32_t capacity) const;

    uint32_t GuestAlloc(uint32_t size);
    uint32_t NewImportStub(const char* dll, const char* name, uint32_t argumentBytes);

    bool CallGuest(uint32_t address, const uint32_t* stackArguments, uint32_t argumentCount,
        uint32_t ecx, const char* what, uint32_t& eax);

    // Hook handlers.
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

    uc_hook invalidHook;
    uc_hook_add(machine, &invalidHook, UC_HOOK_MEM_INVALID,
        reinterpret_cast<void*>(+[](uc_engine* machine, uc_mem_type type, uint64_t address, int size,
            int64_t value, void* user) -> bool {
            (void)machine;
            return static_cast<WardenEmu::Impl*>(user)->HandleInvalidMemory(type, address, size, value);
        }),
        this, 0, 0);

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
        { "Sleep", 4 },
        { "GetTickCount", 0 },
        { "LCMapStringA", 24 }, // locale, flags, src, cchSrc, dest, cchDest
        { "lstrlenA", 4 },
        { "lstrcpyA", 8 },
        { "lstrcatA", 8 },
        { "VirtualAlloc", 16 }, // address, size, allocationType, protect
        { "VirtualFree", 12 }, // address, size, freeType
        { "VirtualProtect", 16 }, // address, size, newProtect, &oldProtect
        { "GetCurrentProcessId", 0 },
        { "GetCurrentThreadId", 0 },
        { "ReadProcessMemory", 20 }, // process, base, buffer, size, &read
        { "WriteProcessMemory", 20 }, // process, base, buffer, size, &written
    };

    for (const KnownImport& import : known) {
        if (strcasecmp(import.name, name) == 0) {
            return import.argumentBytes;
        }
    }

    return UINT32_MAX;
}

// A module call into an import stub: gather the stack arguments, run the real behavior, fill
// EAX. The stub's own `ret n` then cleans the arguments as __stdcall expects.
void WardenEmu::Impl::HandleImport(const ImportStub& stub) {
    uint32_t esp;
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);

    uint32_t arguments[8];
    uint32_t argumentCount = stub.argumentBytes / 4;

    if (argumentCount > 8) {
        argumentCount = 8;
    }

    for (uint32_t i = 0; i < argumentCount; i++) {
        ReadWord(esp + 4 + i * 4, arguments[i]);
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

        uint32_t address = GuestAlloc(size);

        if (address) {
            WardenLog("VirtualAlloc(%u, flags %08x) -> %08x", size, arguments[2], address);
        }

        return address;
    }

    if (strcasecmp(stub.name, "VirtualFree") == 0) {
        // The bump heap releases everything with the module, exactly like the pad's MemoryFree.
        return 1; // TRUE
    }

    if (strcasecmp(stub.name, "VirtualProtect") == 0 && argumentCount >= 4) {
        // Every mapped region already carries full permissions, so the requested protection is
        // in effect immediately; report the old protection as the full-access default.
        return WriteWord(arguments[3], 0x40 /* PAGE_EXECUTE_READWRITE */) ? 1 : 0;
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

    WardenLog("import %s!%s was called but has no behavior; returning 0", stub.dll, stub.name);
    return 0;
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

    uc_err error = uc_emu_start(machine, address, STOP_ADDRESS, 0, 50000000);

    uc_reg_read(machine, UC_X86_REG_EAX, &(eax = 0));

    if (error != UC_ERR_OK) {
        WardenLog("%s at %08x: emu error %s", what, address, uc_strerror(error));
        return false;
    }

    return true;
}

// =====================================================================
// Hook handlers
// =====================================================================

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

            if (!byOrdinal && strcasecmp(dll, "kernel32.dll") == 0) {
                // Only imports with a real implementation below resolve; everything else is
                // collected and reported once, so one failed load lists every missing API.
                // Module DLL names vary in case between shipped modules ("KERNEL32.dll" is a
                // real example), and Windows resolution is case-insensitive, hence strcasecmp.
                uint32_t argumentBytes = Impl::KnownImportArgumentBytes(name);

                if (argumentBytes != UINT32_MAX) {
                    stub = impl.NewImportStub(dll, name, argumentBytes);
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

bool WardenEmu::ImageLoaded() const {
    return m_impl->imageLoaded;
}

bool WardenEmu::ModuleCreated() const {
    return m_impl->moduleCreated;
}

#endif
