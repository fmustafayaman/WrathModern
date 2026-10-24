#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pwd.h>
#include <sys/sysctl.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/Sync.h"

static int32_t s_sleepInBackground = 1;
static const char* s_osTypeNames[] = { "Unknown", "Win95", "Win95OSR2", "Win98", "Win98SE", "WinME", "WinNT4", "Win2000", "WinXP", "Win2003", "Win9X_Other", "WinNT_Other", "MacOS9", "MacOSX", "Linux", "WinVista" };

static const uint8_t s_hexDigitValues[128] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0,
    0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static int32_t s_haveProcessorFeatures;
static int32_t s_processorVendor;
static uint32_t s_processorFeatures;
static int32_t s_processorSockets;
static int32_t s_processorCores;
static int32_t s_processorCount;
static uintptr_t s_textStart;
static uintptr_t s_textEnd;
static int32_t s_cpuLogEnabled;
static uint32_t s_backgroundSleepMs;
static uint64_t s_cpuTicksPerSecond;

static int32_t SysctlInt(const char* name, int32_t fallback) {
    int32_t value = 0;
    size_t size = sizeof(value);

    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value > 0 ? value : fallback;
}

int32_t IOsGetProcessorFeatures(CpuIdInfo* info) {
    // There is no CPUID on arm64; report the processor by name, with no CPUID data.
    memset(info, 0, sizeof(CpuIdInfo));

    size_t size = sizeof(info->m_brand);
    sysctlbyname("machdep.cpu.brand_string", info->m_brand, &size, nullptr, 0);
    info->m_brand[sizeof(info->m_brand) - 1] = '\0';
    memcpy(info->m_vendor, "Apple       ", 12);

    return 0;
}

void OsSystemEnableCpuLog() {
    s_cpuLogEnabled = 1;
}

void IOsSystemCpuLog(const CpuIdInfo* info) {
    if (!s_cpuLogEnabled) {
        return;
    }

    OsCreateDirectory("Logs", 0);

    HSLOG log;
    SLogCreate("Logs\\cpu.log", 0, &log);

    SLogWrite(log, "vendor: %d", s_processorVendor);
    SLogWrite(log, "features: %08X", s_processorFeatures & 0x7FFFFFFF);
    SLogWrite(log, "sockets: %d", s_processorSockets);
    SLogWrite(log, "cores: %d", s_processorCores);
    SLogWrite(log, "processors: %d", s_processorCount);

    const char* brand = info->m_brand;

    while (*brand == ' ') {
        brand++;
    }

    SLogWrite(log, "processor brand string= %s", brand);
    SLogClose(log);
}

uint32_t OsGetNumberOfProcessors() {
    return static_cast<uint32_t>(SysctlInt("hw.logicalcpu", 1));
}

int32_t OsSleepInBackground() {
    return s_sleepInBackground;
}

void OsSetSleepInBackground(int32_t sleep) {
    s_sleepInBackground = sleep;
}

uint32_t OsGetBackgroundSleepMs() {
    return s_backgroundSleepMs;
}

void OsSetBackgroundSleepMs(uint32_t milliseconds) {
    s_backgroundSleepMs = milliseconds;
}

OsType OsGetVersion() {
    return OsType_MacOSX;
}

void OsGetVersionString(char* buffer, int32_t size) {
    SStrCopy(buffer, s_osTypeNames[OsGetVersion()], size);
}

// The platform and operating system the client reports to the server, as four-character codes:
// "x86" and "Win". Warmane drops clients that report "OSX", so this
// build reports what the Windows client does. The servers then send Windows Warden modules,
// which the emulated Warden client runs in a 32-bit machine (Engine/Source/Os/WardenEmu.h).
uint32_t OsGetProcessorId() {
    return 0x783836;
}

uint32_t OsGetOsId() {
    return 0x57696E;
}

int32_t OsGetComputerName(char* buffer, uint32_t* size) {
    char name[256];

    if (gethostname(name, sizeof(name)) != 0) {
        return 0;
    }

    name[sizeof(name) - 1] = '\0';

    // GetComputerNameA semantics: fails without writing when the buffer is too small, and reports
    // the length without the terminator.
    uint32_t length = static_cast<uint32_t>(strlen(name));

    if (length + 1 > *size) {
        *size = length + 1;
        return 0;
    }

    memcpy(buffer, name, length + 1);
    *size = length;

    return 1;
}

int32_t OsGetUserName(char* buffer, uint32_t* size) {
    passwd* entry = getpwuid(getuid());

    if (!entry || !entry->pw_name) {
        return 0;
    }

    // GetUserNameA reports the length including the terminator.
    uint32_t length = static_cast<uint32_t>(strlen(entry->pw_name)) + 1;

    if (length > *size) {
        *size = length;
        return 0;
    }

    memcpy(buffer, entry->pw_name, length);
    *size = length;

    return 1;
}

uint64_t OsGetPhysicalMemory() {
    uint64_t memory = 0;
    size_t size = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);

    return memory;
}

void* CreateNamedEventA(const char* name, int32_t manualReset) {
    return MacSync::EventCreate(manualReset != 0, false);
}

void OsFindTextSectionBounds() {
    // The text segment of the client executable, the counterpart of Wow.exe's .text section.
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const mach_header_64* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));

        if (header->filetype != MH_EXECUTE) {
            continue;
        }

        unsigned long size = 0;
        uint8_t* text = getsegmentdata(header, "__TEXT", &size);

        if (text) {
            s_textStart = reinterpret_cast<uintptr_t>(text);
            s_textEnd = s_textStart + size;
            return;
        }
    }

    SErrDisplayAppFatal("Unable to find .text section");
}

// Text segments of the mods the client loaded itself (Mods/*.dylib). Their Lua functions and console
// variable callbacks pass the same check as the client's own code; code from anywhere else still fails.
static struct {
    uintptr_t m_start;
    uintptr_t m_end;
} s_modText[64];
static uint32_t s_modTextCount;

void OsAllowFunctionPointerImage(const void* header) {
    unsigned long size = 0;
    uint8_t* text = header ? getsegmentdata(static_cast<const mach_header_64*>(header), "__TEXT", &size) : nullptr;

    if (text && s_modTextCount < sizeof(s_modText) / sizeof(s_modText[0])) {
        s_modText[s_modTextCount].m_start = reinterpret_cast<uintptr_t>(text);
        s_modText[s_modTextCount].m_end = reinterpret_cast<uintptr_t>(text) + size;
        s_modTextCount++;
    }
}

static bool IsModText(uintptr_t address) {
    for (uint32_t i = 0; i < s_modTextCount; i++) {
        if (address >= s_modText[i].m_start && address < s_modText[i].m_end) {
            return true;
        }
    }

    return false;
}

void OsValidateFunctionPointer(const void* ptr) {
    if (!s_textStart || !s_textEnd) {
        OsFindTextSectionBounds();
    }

    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);

    if ((address < s_textStart || address >= s_textEnd) && !IsModText(address)) {
        char buffer[64];
        SStrPrintf(buffer, sizeof(buffer), "Invalid function pointer: %p", ptr);
        SErrDisplayAppFatal(buffer);
    }
}

int32_t IOsParseProcessorFrequency(const char* brand) {
    if (!brand || !*brand) {
        return 0;
    }

    const char* p = brand + strlen(brand) - 1;

    if (p <= brand || *p != 'z') {
        return 0;
    }

    p--;

    if (p <= brand || *p != 'H') {
        return 0;
    }

    p--;

    if (p <= brand) {
        return 0;
    }

    double scale;

    if (*p == 'G') {
        scale = 1000000000.0;
    } else if (*p == 'M') {
        scale = 1000000.0;
    } else if (*p == 'T') {
        scale = 1000000000000.0;
    } else {
        return 0;
    }

    char number[48];
    memset(number, ' ', sizeof(number));
    number[47] = '\0';

    p--;
    char* dst = &number[46];

    while (p > brand && *p && ((*p >= '0' && *p <= '9') || *p == '.')) {
        *dst = *p;
        p--;
        dst--;
    }

    double value = atof(number);

    if (value == 0.0) {
        return 0;
    }

    s_cpuTicksPerSecond = static_cast<int64_t>(value * scale);

    return 1;
}

int32_t IOsGetPowerProfFrequency() {
    uint64_t frequency = 0;
    size_t size = sizeof(frequency);

    // Intel Macs report their nominal clock; Apple silicon does not.
    if (sysctlbyname("hw.cpufrequency", &frequency, &size, nullptr, 0) == 0 && frequency) {
        s_cpuTicksPerSecond = frequency;
        return 1;
    }

    return 0;
}

int32_t OsIsRemoteSession() {
    return 0;
}

int32_t OsLaunchURL(const char* url) {
    if (!url || !*url) {
        return 0;
    }

    if (SStrLen(url) >= 1024) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    // The client passes URLs %-escaped; Windows unescapes them before handing them to the shell.
    char file[1024];
    char* dst = file;
    const char* src = url + 1;

    for (char c = *url; c; c = *src++, dst++) {
        if (c == '%') {
            char high = *src++;

            if (!high) {
                break;
            }

            char low = *src++;

            if (!low) {
                break;
            }

            *dst = static_cast<char>((s_hexDigitValues[high & 0x7F] << 4) | s_hexDigitValues[low & 0x7F]);
        } else {
            *dst = c;
        }
    }

    *dst = '\0';

    @autoreleasepool {
        NSString* string = [NSString stringWithUTF8String:file];
        NSURL* target = string ? [NSURL URLWithString:string] : nil;

        if (!target) {
            target = string ? [NSURL URLWithString:[string stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]]] : nil;
        }

        return target && [[NSWorkspace sharedWorkspace] openURL:target] ? 1 : 0;
    }
}

uint32_t OsGetProcessorFeaturesEx(int32_t& vendor) {
    if (s_haveProcessorFeatures) {
        vendor = s_processorVendor;
        return s_processorFeatures;
    }

    s_haveProcessorFeatures = 1;

    s_processorCount = SysctlInt("hw.logicalcpu", 1);
    s_processorCores = SysctlInt("hw.physicalcpu", 1);
    s_processorSockets = SysctlInt("hw.packages", 1);

    CpuIdInfo info;
    IOsGetProcessorFeatures(&info);

    // An unknown vendor (0) with the features the client's SSE paths need: they run through sse2neon
    // on arm64. Feature bits: 1 TSC, 2 MMX, 4 SSE, 0x10 SSE2, 0x40 multiple cores.
    s_processorVendor = 0;
    uint32_t features = 0x1 | 0x2 | 0x4 | 0x10;

    if (s_processorCores > 1) {
        features |= 0x40;
    }

    s_processorFeatures = features | 0x80000000;

    IOsSystemCpuLog(&info);

    if (!IOsParseProcessorFrequency(info.m_brand) && !IOsGetPowerProfFrequency()) {
        s_cpuTicksPerSecond = OsGetAsyncClocksPerSecond();
    }

    vendor = s_processorVendor;
    return s_processorFeatures;
}

uint64_t OsGetProcessorTicksPerSecond() {
    if (!s_cpuTicksPerSecond) {
        int32_t vendor;
        OsGetProcessorFeaturesEx(vendor);
    }

    return s_cpuTicksPerSecond;
}
