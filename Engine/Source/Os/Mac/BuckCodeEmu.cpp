#include <cstddef>

#if defined(WOWDECOMP_PLATFORM_MAC) && defined(WRATHMODERN_WARDEN_EMULATION)

#include "Engine/Source/Os/Mac/BuckCodeEmu.h"

#include <unicorn/unicorn.h>
#include <zlib.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

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
 *  0x0E000000              guest heap for the buffers the code allocates
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

constexpr uint32_t HEAP_BASE = 0x0E000000;
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
constexpr uint32_t STUB_SYSTEM_FILE_TIME = 0x0FE00070;
constexpr uint32_t STUB_GET_PROC_ADDRESS = 0x0FE00080;
constexpr uint32_t STUB_GET_CURRENT_HW_PROFILE = 0x0FE00090;
constexpr uint32_t STUB_CLOSE_HANDLE = 0x0FE000D0;

constexpr uint32_t IAT_LOAD_LIBRARY = 0x009DF248;
constexpr uint32_t IAT_FREE_LIBRARY = 0x009DF250;
constexpr uint32_t IAT_REG_OPEN_KEY = 0x009DF00C;
constexpr uint32_t IAT_REG_QUERY_VALUE = 0x009DF010;
constexpr uint32_t IAT_REG_CLOSE_KEY = 0x009DF008;
constexpr uint32_t IAT_SYSTEM_FILE_TIME = 0x009DF388;
constexpr uint32_t CLIENT_GLOBAL_GET_PROC_ADDRESS = 0x00B2ED98;
constexpr uint32_t IAT_CLOSE_HANDLE = 0x009DF124;

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

// A module as Module32First/Next report it: MODULEENTRY32 base, size and full path. The module
// scan the server sends after login walks this list, and a single-entry list is visibly not a
// Windows client.
struct EmulatedModule {
    uint32_t base;
    uint32_t size;
    const char* path;
};

// The exact module list the official Wine-hosted HD client reports through Module32First/Next
// at the login screen (43 entries, read from the live process with the same Toolhelp calls the
// 192-byte module-scan code uses). The server compares this list, so the bases, sizes and paths
// are reproduced verbatim, including the launcher mods/libSiliconPatch DLL and mtld3d.
const EmulatedModule EMULATED_MODULES[] = {
    { 0x00400000, 0x009FD000, "Z:\\Volumes\\Yaman SSD\\wow3.3.5a\\wow.exe" },
    { 0x7BF40000, 0x000A2000, "C:\\windows\\system32\\ntdll.dll" },
    { 0x7BED0000, 0x00053000, "C:\\windows\\system32\\kernel32.dll" },
    { 0x7BDB0000, 0x0010F000, "C:\\windows\\system32\\kernelbase.dll" },
    { 0x7BC80000, 0x00120000, "C:\\windows\\system32\\OPENGL32.dll" },
    { 0x7BC00000, 0x00070000, "C:\\windows\\system32\\gdi32.dll" },
    { 0x7BA50000, 0x0019B000, "C:\\windows\\system32\\user32.dll" },
    { 0x7BA20000, 0x0001B000, "C:\\windows\\system32\\sechost.dll" },
    { 0x7B930000, 0x000DA000, "C:\\windows\\system32\\ucrtbase.dll" },
    { 0x7B8E0000, 0x00036000, "C:\\windows\\system32\\advapi32.dll" },
    { 0x7B830000, 0x000A0000, "C:\\windows\\system32\\msvcrt.dll" },
    { 0x7B7E0000, 0x00033000, "C:\\windows\\system32\\win32u.dll" },
    { 0x7B780000, 0x00042000, "C:\\windows\\system32\\VERSION.dll" },
    { 0x7B750000, 0x00015000, "C:\\windows\\system32\\VCRUNTIME140.dll" },
    { 0x7B720000, 0x0001B000, "C:\\windows\\system32\\IMM32.dll" },
    { 0x7B6C0000, 0x0004E000, "C:\\windows\\system32\\WININET.dll" },
    { 0x7B690000, 0x00012000, "C:\\windows\\system32\\mpr.dll" },
    { 0x7B640000, 0x0003D000, "C:\\windows\\system32\\shlwapi.dll" },
    { 0x7B610000, 0x00013000, "C:\\windows\\system32\\shcore.dll" },
    { 0x7AD50000, 0x008AB000, "C:\\windows\\system32\\shell32.dll" },
    { 0x7AD10000, 0x00022000, "C:\\windows\\system32\\ws2_32.dll" },
    { 0x7ACC0000, 0x00036000, "C:\\windows\\system32\\DINPUT8.dll" },
    { 0x7AC90000, 0x00020000, "C:\\windows\\system32\\cfgmgr32.dll" },
    { 0x7AB90000, 0x000E9000, "C:\\windows\\system32\\comctl32.dll" },
    { 0x7AAE0000, 0x0009D000, "C:\\windows\\system32\\ole32.dll" },
    { 0x7AA80000, 0x00047000, "C:\\windows\\system32\\combase.dll" },
    { 0x7A9F0000, 0x00076000, "C:\\windows\\system32\\rpcrt4.dll" },
    { 0x7A9D0000, 0x00008000, "C:\\windows\\system32\\cryptbase.dll" },
    { 0x7A9A0000, 0x00014000, "C:\\windows\\system32\\coml2.dll" },
    { 0x7A980000, 0x0000C000, "C:\\windows\\system32\\hid.dll" },
    { 0x10000000, 0x00069000, "Z:\\Volumes\\Yaman SSD\\wow3.3.5a\\DivxDecoder.dll" },
    { 0x7A920000, 0x00043000, "C:\\windows\\system32\\WINMM.dll" },
    { 0x7A8F0000, 0x00016000, "C:\\windows\\system32\\msacm32.dll" },
    { 0x7A880000, 0x00059000, "C:\\windows\\system32\\SETUPAPI.dll" },
    { 0x7A800000, 0x0006D000, "Z:\\Volumes\\Yaman SSD\\wow3.3.5a\\mods\\libSiliconPatch_WOTLK_3.3.0_preview.dll" },
    { 0x7A7E0000, 0x00007000, "c:\\windows\\system32\\winemac.drv" },
    { 0x7A7A0000, 0x0002E000, "C:\\windows\\system32\\uxtheme.dll" },
    { 0x7A780000, 0x00007000, "Z:\\Volumes\\Yaman SSD\\wow3.3.5a\\libDllLdr.dll" },
    { 0x7A620000, 0x00146000, "Z:\\Volumes\\Yaman SSD\\wow3.3.5a\\d3d9.dll" },
    { 0x7A5D0000, 0x00033000, "C:\\windows\\system32\\mtld3d.dll" },
    { 0x7A560000, 0x00051000, "C:\\windows\\system32\\bcrypt.dll" },
    { 0x7A520000, 0x00023000, "C:\\windows\\system32\\mmdevapi.dll" },
    { 0x7A460000, 0x000A5000, "C:\\windows\\system32\\oleaut32.dll" },
};

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

    // The last SetMessageHandler registration the delivered code made, so the caller can carry
    // the handler's param (the next message's key) forward exactly as the framework does.
    bool lastRegistrationValid = false;
    uint32_t lastRegistrationOpcode = 0;
    uint32_t lastRegistrationParam = 0;

    bool Boot();
    void Teardown();

    void Log(const char* format, ...) __attribute__((format(printf, 2, 3)));

    bool ReadMemory(uint32_t address, void* out, uint32_t size) const;
    bool WriteMemory(uint32_t address, const void* in, uint32_t size) const;
    bool ReadWord(uint32_t address, uint32_t& value) const;
    bool WriteWord(uint32_t address, uint32_t value) const;
    std::string ReadGuestString(uint32_t address, uint32_t capacity = 4096) const;

    uint32_t HeapAlloc(uint32_t size, uint32_t alignment = 16);
    uint32_t HeapAllocPages(uint32_t size);

    // CDataStore / WDataStore semantics on guest objects.
    void StoreConstruct(uint32_t store);
    uint32_t StoreGetInt(uint32_t store);
    void StorePutBytes(uint32_t store, const void* data, uint32_t size);
    uint32_t StorePut(uint32_t store, uint32_t value, uint32_t width);
    void StoreGetArray(uint32_t store, uint32_t destination, uint32_t count);

    bool RunFunction(uint32_t address, const uint32_t* stackArguments, uint32_t argumentCount, uint32_t ecx, uint32_t& eax);

    bool HandleClientCall(uint32_t address);
    bool FillModuleEntry(uint32_t entry, uint32_t index);
    void SeedClientGlobals(const uint8_t* image, uint32_t size);
    void HandleInvalidMemory(uc_mem_type type, uint64_t address, int size);

    // Module32First/Next walk position. CreateToolhelp32Snapshot resets it.
    uint32_t s_moduleIndex = 0;

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
    auto read16 = [](const uint8_t* p) {
        uint16_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    };
    auto read32 = [](const uint8_t* p) {
        uint32_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    };

    if (!image || size < 0x40) {
        return;
    }

    // IMAGE_DOS_HEADER.e_lfanew -> IMAGE_NT_HEADERS; PE32 optional header, so ImageBase is at
    // +0x34 and the section table follows the optional header.
    uint32_t header = read32(image + 0x3C);

    if (header + 0x18 > size || read32(image + header) != 0x00004550) {
        return;
    }

    uint16_t sectionCount = read16(image + header + 6);
    uint16_t optionalSize = read16(image + header + 20);
    uint32_t imageBase = read32(image + header + 0x34);
    const uint8_t* section = image + header + 0x18 + optionalSize;

    for (uint16_t i = 0; i < sectionCount; i++, section += 0x28) {
        if (section + 0x28 > image + size) {
            return;
        }

        if (memcmp(section, ".text", 5) != 0) {
            continue;
        }

        uint32_t virtualSize = read32(section + 8);
        uint32_t virtualAddress = read32(section + 12);
        uint32_t start = imageBase + virtualAddress;
        uint32_t end = start + virtualSize;

        WriteWord(CLIENT_TEXT_START_GLOBAL, start);
        WriteWord(CLIENT_TEXT_END_GLOBAL, end);
        Log("client .text %08x..%08x cached at %08x/%08x (OsFindTextSectionBounds %08x, "
            "OsValidateFunctionPointer %08x)",
            start, end, CLIENT_TEXT_START_GLOBAL, CLIENT_TEXT_END_GLOBAL,
            CLIENT_OS_FIND_TEXT_SECTION_BOUNDS, CLIENT_OS_VALIDATE_FUNCTION_POINTER);
        return;
    }
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
    WriteWord(IAT_SYSTEM_FILE_TIME, STUB_SYSTEM_FILE_TIME);
    WriteWord(CLIENT_GLOBAL_GET_PROC_ADDRESS, STUB_GET_PROC_ADDRESS);
    WriteWord(IAT_CLOSE_HANDLE, STUB_CLOSE_HANDLE);

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

    return true;
}

void BuckCodeEmu::Impl::Teardown() {
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

std::string BuckCodeEmu::Impl::ReadGuestString(uint32_t address, uint32_t capacity) const {
    std::string text;

    for (uint32_t i = 0; address && i < capacity; i++) {
        uint8_t byte = 0;

        if (!ReadMemory(address + i, &byte, 1) || !byte) {
            break;
        }

        text += static_cast<char>(byte);
    }

    return text;
}

uint32_t BuckCodeEmu::Impl::HeapAlloc(uint32_t size, uint32_t alignment) {
    if (alignment > 1) {
        heapNext = (heapNext + alignment - 1) & ~(alignment - 1);
    }

    uint32_t result = heapNext;
    heapNext += size;
    heapNext = (heapNext + 15) & ~15u;

    if (heapNext > HEAP_LIMIT) {
        Log("buck heap exhausted (%u bytes wanted)", size);
    }

    return result;
}

uint32_t BuckCodeEmu::Impl::HeapAllocPages(uint32_t size) {
    uint32_t rounded = (size + 0xFFF) & ~0xFFFu;
    uint32_t result = (heapNext + 0xFFF) & ~0xFFFu;
    heapNext = result + rounded;

    if (heapNext > HEAP_LIMIT) {
        Log("buck page heap exhausted (%u bytes wanted)", size);
    }

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

    if (read + 4 <= size && buffer) {
        ReadMemory(buffer + read - base, &value, sizeof(value));
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
    uint32_t bytes = count < available ? count : available;

    static const bool trace = getenv("BUCK_TRACE") != nullptr;

    if (trace) {
        Log("buck GetArray store=%08x dst=%08x count=%u read=%u size=%u -> %u bytes from %08x",
            store, destination, count, read, size, bytes, buffer + read - base);
    }

    if (bytes && buffer && destination) {
        std::vector<uint8_t> copy(bytes);
        ReadMemory(buffer + read - base, copy.data(), bytes);
        WriteMemory(destination, copy.data(), bytes);
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
    const uint32_t count = sizeof(EMULATED_MODULES) / sizeof(EMULATED_MODULES[0]);

    if (index >= count) {
        return false;
    }

    const EmulatedModule& module = EMULATED_MODULES[index];
    const char* name = strrchr(module.path, '\\');
    name = name ? name + 1 : module.path;

    // MODULEENTRY32: base at +0x14, size at +0x18, module name at +0x20, full path at +0x120.
    // The 192-byte scan reads base, size and the path at +0x120; the name keeps the structure
    // usable by any other Toolhelp consumer.
    WriteWord(entry + 0x14, module.base);
    WriteWord(entry + 0x18, module.size);
    WriteMemory(entry + 0x20, name, static_cast<uint32_t>(strlen(name)) + 1);
    WriteMemory(entry + 0x120, module.path, static_cast<uint32_t>(strlen(module.path)) + 1);
    return true;
}

bool BuckCodeEmu::Impl::HandleClientCall(uint32_t address) {
    if (address == STOP_ADDRESS) {
        uc_emu_stop(machine);
        return true;
    }

    uint32_t esp = 0;
    uc_reg_read(machine, UC_X86_REG_ESP, &esp);

    uint32_t frame[6] = { 0 };
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
        case CLIENT_STORE_GET_INT: {
            // CDataStore::GetInt: consumes the next dword and returns it. The 0x41B handler
            // reads the challenge nonce, then the code length, then the delivered code reads
            // the trailing answer dword with the same call.
            result = StoreGetInt(ecx);
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
                ReadMemory(data, bytes.data(), count);
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

            if (read < size && buffer) {
                ReadMemory(buffer + read - base, &byte, 1);
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
            uint32_t bytes = arguments[1] < available ? arguments[1] : available;

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
            WriteMemory(arguments[0], bytes.data(), size);
            result = arguments[0];
            finish(0); // __cdecl, the caller cleans
            return true;
        }
        case CLIENT_MEMCPY: {
            uint32_t size = arguments[2];
            std::vector<uint8_t> bytes(size);
            ReadMemory(arguments[1], bytes.data(), size);
            WriteMemory(arguments[0], bytes.data(), size);
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
            result = HeapAlloc(arguments[0], 16);
            finish(0);
            return true;
        }
        case CLIENT_OPERATOR_DELETE: {
            result = 0;
            finish(0);
            return true;
        }
        case CLIENT_SMEM_ALLOC: {
            result = HeapAlloc((arguments[0] + 7) & ~7u, 16);
            finish(16); // STORMAPI (stdcall)
            return true;
        }
        case CLIENT_SMEM_REALLOC: {
            uint32_t bytes = arguments[1];
            uint32_t old = arguments[0];

            if (!bytes) {
                result = 0;
            } else if (!old) {
                result = HeapAlloc(bytes, 16);
            } else {
                result = HeapAlloc(bytes, 16);
                std::vector<uint8_t> copy(bytes);
                ReadMemory(old, copy.data(), bytes);
                WriteMemory(result, copy.data(), bytes);
            }

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
            result = 0;
            finish(0); // __cdecl, the caller pops the arguments
            return true;
        }
        case STUB_VIRTUAL_ALLOC: {
            uint32_t requested = arguments[0];
            uint32_t size = arguments[1];

            if (requested) {
                result = requested;
            } else {
                result = HeapAllocPages(size);
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
        case STUB_SYSTEM_FILE_TIME: {
            // The code zeroes an 8-byte buffer and asks the client for a 64-bit time value.
            uint64_t ticks = static_cast<uint64_t>(time(nullptr)) * 10000000ull + 116444736000000000ull;
            WriteMemory(arguments[2], &ticks, sizeof(ticks));
            result = 0;
            finish(16);
            return true;
        }
        case STUB_CLOSE_HANDLE: {
            result = 1;
            finish(4);
            return true;
        }
        case CLIENT_TOOLHELP_SNAPSHOT: {
            // CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0): a pseudo-handle for the module walk.
            s_moduleIndex = 0;
            result = 0x03000300;
            finish(0);
            return true;
        }
        case CLIENT_MODULE32_FIRST: {
            s_moduleIndex = 0;
            result = FillModuleEntry(arguments[1], s_moduleIndex) ? 1 : 0;

            if (result) {
                s_moduleIndex++;
            }

            finish(0);
            return true;
        }
        case CLIENT_MODULE32_NEXT: {
            result = FillModuleEntry(arguments[1], s_moduleIndex) ? 1 : 0;

            if (result) {
                s_moduleIndex++;
            }

            finish(0);
            return true;
        }
        default:
            break;
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
    uc_err error = uc_emu_start(impl.machine, codeAddress, STOP_ADDRESS, 0, 0);

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

    uc_err error = uc_emu_start(impl.machine, handlerAddress, STOP_ADDRESS, 0, 0);

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
    uc_err error = uc_emu_start(impl.machine, handlerAddress, STOP_ADDRESS, 0, 0);

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
        uc_err hookError = uc_emu_start(impl.machine, hookAddress, STOP_ADDRESS, 0, 0);

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

#endif
