#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pwd.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/Storm/Source/MachineState.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/Paths.h"

// Machine state for error and crash logs on macOS, laid out like the Windows logs: header, registers,
// stack trace, loaded modules and memory dumps. Registers and the stack walk are arm64; the stack is
// walked along the frame pointer chain, which macOS arm64 code always maintains, and symbolized with
// dladdr. Memory is read through the kernel so a bad address cannot fault the logger.

#define LOG_PTR "%016llX"
#define LOG_PTR_EXTRA 8

struct MEMDUMP {
    void (*logLineProc)(void*, const char*, ...);
    void* logLineProcParam;
};

static char s_monthNames[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static volatile int32_t sgRecursionLevel;

void GetLocalTime(SYSTEMTIME* time) {
    timeval now;
    gettimeofday(&now, nullptr);

    time_t seconds = now.tv_sec;
    tm local;
    localtime_r(&seconds, &local);

    time->wYear = static_cast<uint16_t>(local.tm_year + 1900);
    time->wMonth = static_cast<uint16_t>(local.tm_mon + 1);
    time->wDayOfWeek = static_cast<uint16_t>(local.tm_wday);
    time->wDay = static_cast<uint16_t>(local.tm_mday);
    time->wHour = static_cast<uint16_t>(local.tm_hour);
    time->wMinute = static_cast<uint16_t>(local.tm_min);
    time->wSecond = static_cast<uint16_t>(local.tm_sec);
    time->wMilliseconds = static_cast<uint16_t>(now.tv_usec / 1000);
}

// Reads memory of this process without faulting on unmapped addresses.
static bool SafeRead(const void* address, void* buffer, size_t size) {
    mach_vm_size_t read = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(), reinterpret_cast<vm_address_t>(address), size, reinterpret_cast<vm_address_t>(buffer), reinterpret_cast<vm_size_t*>(&read));

    return result == KERN_SUCCESS && read == size;
}

static void sLogSeparatorLine(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, char character, int32_t longLine) {
    char line[80];
    uint32_t length = longLine ? 78 : 40;
    memset(line, character, length);
    line[length] = 0;
    logLineProc(logLineProcParam, "%s", line);
}

static void sLogHeader(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const char* headerString) {
    logLineProc(logLineProcParam, "");
    sLogSeparatorLine(logLineProc, logLineProcParam, '-', 0);
    logLineProc(logLineProcParam, "    %s", headerString);
    sLogSeparatorLine(logLineProc, logLineProcParam, '-', 0);
    logLineProc(logLineProcParam, "");
}

static void sLogMemoryHexDump(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint8_t* address, uint32_t numBytes, int32_t alignedLines) {
    char buffer[80 + LOG_PTR_EXTRA];
    uint32_t numLines = numBytes >> 4;
    uint32_t offset = reinterpret_cast<uintptr_t>(address) & 0xF;

    if (alignedLines) {
        memset(buffer, ' ', 78 + LOG_PTR_EXTRA);
        buffer[0] = '*';
        char* marker = &buffer[10 + LOG_PTR_EXTRA + 3 * offset + (offset >> 2)];
        buffer[78 + LOG_PTR_EXTRA] = 0;
        buffer[2] = '=';
        memcpy(&buffer[4], "addr", 4);
        marker[0] = '*';
        marker[1] = '*';
        buffer[62 + LOG_PTR_EXTRA + offset] = '*';
        logLineProc(logLineProcParam, "%s", buffer);

        if (offset) {
            address -= offset;
            ++numLines;
        }
    }

    for (; numLines; --numLines) {
        char* text = buffer + sprintf(buffer, LOG_PTR ": ", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
        uint8_t bytes[16];

        if (!SafeRead(address, bytes, sizeof(bytes))) {
            strcpy(text, "<can't read from this address>");
            logLineProc(logLineProcParam, "%s", buffer);
            return;
        }

        text += sprintf(text, "%02X %02X %02X %02X  ", bytes[0], bytes[1], bytes[2], bytes[3]);
        text += sprintf(text, "%02X %02X %02X %02X  ", bytes[4], bytes[5], bytes[6], bytes[7]);
        text += sprintf(text, "%02X %02X %02X %02X  ", bytes[8], bytes[9], bytes[10], bytes[11]);
        text += sprintf(text, "%02X %02X %02X %02X  ", bytes[12], bytes[13], bytes[14], bytes[15]);

        for (uint32_t i = 0; i < 16; ++i) {
            text[i] = isprint(bytes[i]) ? static_cast<char>(bytes[i]) : '.';
        }

        text[16] = 0;
        logLineProc(logLineProcParam, "%s", buffer);
        address += 16;
    }
}

static void sLogExeFile(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    char path[4096];

    if (!MacPaths::ExecutablePath(path, sizeof(path))) {
        strcpy(path, "<unknown>");
    }

    logLineProc(logLineProcParam, "%-10s%s", "Exe:", path);
}

static void sLogDateTimeString(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const SYSTEMTIME* time) {
    uint16_t hour = time->wHour;
    char ampm = hour < 12 ? 'A' : 'P';

    if (hour > 12) {
        hour -= 12;
    } else if (!hour) {
        hour = 12;
    }

    logLineProc(logLineProcParam, "%-10s%3s %2d, %4d %2d:%02d:%02d.%03d %cM", "Time:", s_monthNames[time->wMonth - 1], time->wDay, time->wYear, hour, time->wMinute, time->wSecond, time->wMilliseconds, ampm);
}

static void sLogUserName(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    passwd* entry = getpwuid(getuid());
    logLineProc(logLineProcParam, "%-10s%s", "User:", entry && entry->pw_name ? entry->pw_name : "<unknown>");
}

static void sLogComputerName(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    char computerName[256];

    if (gethostname(computerName, sizeof(computerName)) != 0) {
        strcpy(computerName, "<unknown>");
    }

    computerName[sizeof(computerName) - 1] = 0;
    logLineProc(logLineProcParam, "%-10s%s", "Computer:", computerName);
}

static void sLogSystem(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    char version[64] = "<unknown>";
    char build[64] = "";
    char model[64] = "<unknown>";
    char cpu[128] = "<unknown>";
    uint64_t memory = 0;
    size_t size;

    size = sizeof(version);
    sysctlbyname("kern.osproductversion", version, &size, nullptr, 0);
    size = sizeof(build);
    sysctlbyname("kern.osversion", build, &size, nullptr, 0);
    size = sizeof(model);
    sysctlbyname("hw.model", model, &size, nullptr, 0);
    size = sizeof(cpu);
    sysctlbyname("machdep.cpu.brand_string", cpu, &size, nullptr, 0);
    size = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);

    logLineProc(logLineProcParam, "%-10smacOS %s (%s)", "System:", version, build);
    logLineProc(logLineProcParam, "%-10s%s, %s, %llu MB", "Machine:", model, cpu, static_cast<unsigned long long>(memory >> 20));
}

void LoadMachineStateSymbols() {
}

void UnloadMachineStateSymbols() {
}

void LogComputerInfoHeader(uint32_t logOptions, void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const char* headerTitleLine, const SYSTEMTIME* time) {
    if (__atomic_add_fetch(&sgRecursionLevel, 1, __ATOMIC_SEQ_CST) == 1) {
        if (!logOptions || (logOptions & LOG_OPTION_DEFAULT)) {
            logOptions |= LOG_OPTION_EXEFILE | LOG_OPTION_DATETIME | LOG_OPTION_COMPUTERNAME | LOG_OPTION_USERNAME;

            if (headerTitleLine && *headerTitleLine) {
                logOptions |= LOG_OPTION_TITLE;
            }
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '=', 1);

        if ((logOptions & LOG_OPTION_TITLE) && headerTitleLine && *headerTitleLine) {
            logLineProc(logLineProcParam, "%s", headerTitleLine);
            logLineProc(logLineProcParam, "");
        }

        if (logOptions & LOG_OPTION_EXEFILE) {
            sLogExeFile(logLineProc, logLineProcParam);
        }

        if (logOptions & LOG_OPTION_DATETIME) {
            SYSTEMTIME currentTime;

            if (!time) {
                GetLocalTime(&currentTime);
                time = &currentTime;
            }

            sLogDateTimeString(logLineProc, logLineProcParam, time);
        }

        if (logOptions & LOG_OPTION_USERNAME) {
            sLogUserName(logLineProc, logLineProcParam);
        }

        if (logOptions & LOG_OPTION_COMPUTERNAME) {
            sLogComputerName(logLineProc, logLineProcParam);
        }

        sLogSystem(logLineProc, logLineProcParam);
        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
    }

    __atomic_sub_fetch(&sgRecursionLevel, 1, __ATOMIC_SEQ_CST);
}

static void sLogRegisters(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, const CONTEXT* context) {
    sLogHeader(logLineProc, logLineProcParam, "Registers");

    const __darwin_arm_thread_state64& state = context->__ss;

    for (int32_t i = 0; i < 28; i += 4) {
        logLineProc(logLineProcParam, "X%-2d=" LOG_PTR "  X%-2d=" LOG_PTR "  X%-2d=" LOG_PTR "  X%-2d=" LOG_PTR, i, static_cast<unsigned long long>(state.__x[i]), i + 1, static_cast<unsigned long long>(state.__x[i + 1]), i + 2, static_cast<unsigned long long>(state.__x[i + 2]), i + 3, static_cast<unsigned long long>(state.__x[i + 3]));
    }

    logLineProc(logLineProcParam, "X28=" LOG_PTR "  FP =" LOG_PTR "  LR =" LOG_PTR "  SP =" LOG_PTR, static_cast<unsigned long long>(state.__x[28]), static_cast<unsigned long long>(__darwin_arm_thread_state64_get_fp(state)), static_cast<unsigned long long>(__darwin_arm_thread_state64_get_lr(state)), static_cast<unsigned long long>(__darwin_arm_thread_state64_get_sp(state)));
    logLineProc(logLineProcParam, "PC =" LOG_PTR "  CPSR=%08X", static_cast<unsigned long long>(__darwin_arm_thread_state64_get_pc(state)), state.__cpsr);
    logLineProc(logLineProcParam, "FAR=" LOG_PTR "  ESR=%08X", static_cast<unsigned long long>(context->__es.__far), context->__es.__esr);
}

static void sLogFrame(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint32_t index, uintptr_t address, uintptr_t frame) {
    Dl_info info;

    if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname) {
        const char* module = strrchr(info.dli_fname, '/');
        module = module ? module + 1 : info.dli_fname;
        uintptr_t moduleOffset = address - reinterpret_cast<uintptr_t>(info.dli_fbase);

        if (info.dli_sname) {
            logLineProc(logLineProcParam, "%3u  " LOG_PTR " " LOG_PTR "  %s!%s+0x%llX", index, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(address), module, info.dli_sname, static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(info.dli_saddr)));
        } else {
            logLineProc(logLineProcParam, "%3u  " LOG_PTR " " LOG_PTR "  %s+0x%llX", index, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(address), module, static_cast<unsigned long long>(moduleOffset));
        }
    } else {
        logLineProc(logLineProcParam, "%3u  " LOG_PTR " " LOG_PTR "  <unknown>", index, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(address));
    }
}

static void sLogStackTrace(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uintptr_t pc, uintptr_t lr, uintptr_t fp, bool fromContext, uint32_t stackFramesToSkip) {
    sLogHeader(logLineProc, logLineProcParam, "Call Stack");
    logLineProc(logLineProcParam, "       Frame            Address");

    uint32_t index = 0;
    uint32_t skipped = 0;

    if (fromContext) {
        sLogFrame(logLineProc, logLineProcParam, index++, pc, fp);

        // The faulting function may not have pushed a frame yet; its caller is in LR.
        uintptr_t saved[2];

        if (SafeRead(reinterpret_cast<void*>(fp), saved, sizeof(saved)) && (saved[1] & ~0xFULL) != (lr & ~0xFULL)) {
            sLogFrame(logLineProc, logLineProcParam, index++, lr, fp);
        }
    }

    for (uint32_t depth = 0; fp && depth < 256; depth++) {
        uintptr_t saved[2];

        if ((fp & 0xF) || !SafeRead(reinterpret_cast<void*>(fp), saved, sizeof(saved))) {
            break;
        }

        uintptr_t returnAddress = saved[1] & 0x0000FFFFFFFFFFFFULL;

        if (!returnAddress) {
            break;
        }

        if (skipped < stackFramesToSkip) {
            skipped++;
        } else {
            sLogFrame(logLineProc, logLineProcParam, index++, returnAddress, fp);
        }

        if (saved[0] <= fp) {
            break;
        }

        fp = saved[0];
    }
}

static void sLogLoadedModules(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    logLineProc(logLineProcParam, "");
    sLogHeader(logLineProc, logLineProcParam, "Loaded Modules");

    uint32_t count = _dyld_image_count();

    for (uint32_t i = 0; i < count; i++) {
        const mach_header_64* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        const char* path = _dyld_get_image_name(i);

        if (!header || header->magic != MH_MAGIC_64) {
            continue;
        }

        uint64_t textSize = 0;
        const uint8_t* uuid = nullptr;
        const load_command* command = reinterpret_cast<const load_command*>(header + 1);

        for (uint32_t c = 0; c < header->ncmds; c++) {
            if (command->cmd == LC_SEGMENT_64) {
                const segment_command_64* segment = reinterpret_cast<const segment_command_64*>(command);

                if (!strcmp(segment->segname, "__TEXT")) {
                    textSize = segment->vmsize;
                }
            } else if (command->cmd == LC_UUID) {
                uuid = reinterpret_cast<const uuid_command*>(command)->uuid;
            }

            command = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
        }

        char uuidText[40] = "";

        if (uuid) {
            snprintf(uuidText, sizeof(uuidText), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        }

        logLineProc(logLineProcParam, "DBG-MODULE<" LOG_PTR " %08llX \"%s\" {%s}>", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(header)), static_cast<unsigned long long>(textSize), path ? path : "", uuidText);
    }
}

static void sLogMemory(uint32_t logOptions, void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint8_t* instructionPointer, uint8_t* stackPointer) {
    sLogHeader(logLineProc, logLineProcParam, "Memory Dump");

    if (logOptions & LOG_OPTION_CODE) {
        logLineProc(logLineProcParam, "Code: %d bytes starting at (PC = " LOG_PTR ")", 16, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(instructionPointer)));
        logLineProc(logLineProcParam, "");
        sLogMemoryHexDump(logLineProc, logLineProcParam, instructionPointer, 16, 0);
        logLineProc(logLineProcParam, "");
        logLineProc(logLineProcParam, "");
    }

    if (logOptions & LOG_OPTION_STACK) {
        logLineProc(logLineProcParam, "Stack: %d bytes starting at (SP = " LOG_PTR ")", 1024, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stackPointer)));
        logLineProc(logLineProcParam, "");
        sLogMemoryHexDump(logLineProc, logLineProcParam, stackPointer, 1024, 1);
        logLineProc(logLineProcParam, "");
        logLineProc(logLineProcParam, "");
    }
}

static void STORMAPI CmdMemOutput(void* hOutput, const MEMREPORTENTRY* entry) {
    MEMDUMP* dump = static_cast<MEMDUMP*>(hOutput);
    dump->logLineProc(dump->logLineProcParam, "%5d %5d %s(%d)", entry->unk_0010, entry->unk_0014, entry->unk_0024, entry->unk_0020 >> 1);
}

void SMemGenerateReport(SMEMREPORTTYPE type, void (STORMAPI* callback)(void*, const MEMREPORTENTRY*), void* context) {
    StormCallService(STORM_SERVICE_GENERATEREPORT, type, callback, context);
}

static void sShowOutOfMemory(void (*logLineProc)(void*, const char*, ...), void* logLineProcParam) {
    if (!g_memFullError) {
        return;
    }

    MEMDUMP dump;
    dump.logLineProc = logLineProc;
    dump.logLineProcParam = logLineProcParam;
    SMemGenerateReport(SMEM_REPORT_BY_CALLER, CmdMemOutput, &dump);
    sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
}

void LogMachineState(uint32_t logOptions, void (*logLineProc)(void*, const char*, ...), void* logLineProcParam, uint32_t stackFramesToSkip, CONTEXT* context) {
    if (__atomic_add_fetch(&sgRecursionLevel, 1, __ATOMIC_SEQ_CST) == 1) {
        if (!logOptions || (logOptions & LOG_OPTION_DEFAULT)) {
            logOptions |= LOG_OPTION_DBGHELPSTACK | LOG_OPTION_STACK | LOG_OPTION_MODULES;

            if (context) {
                logOptions |= LOG_OPTION_REGISTERS | LOG_OPTION_CODE;
            }
        }

        uintptr_t pc;
        uintptr_t lr;
        uintptr_t fp;
        uintptr_t sp;

        if (context) {
            pc = __darwin_arm_thread_state64_get_pc(context->__ss);
            lr = __darwin_arm_thread_state64_get_lr(context->__ss);
            fp = __darwin_arm_thread_state64_get_fp(context->__ss);
            sp = __darwin_arm_thread_state64_get_sp(context->__ss);
            stackFramesToSkip = 0;
        } else {
            pc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            lr = pc;
            fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
            sp = fp;
            logOptions &= ~static_cast<uint32_t>(LOG_OPTION_REGISTERS | LOG_OPTION_CODE);
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);

        if ((logOptions & LOG_OPTION_REGISTERS) && context) {
            sLogRegisters(logLineProc, logLineProcParam, context);
        }

        if (logOptions & (LOG_OPTION_DBGHELPSTACK | LOG_OPTION_MANUALSTACK)) {
            sLogStackTrace(logLineProc, logLineProcParam, pc, lr, fp, context != nullptr, stackFramesToSkip);
        }

        if (logOptions & LOG_OPTION_MODULES) {
            sLogLoadedModules(logLineProc, logLineProcParam);
        }

        if (logOptions & (LOG_OPTION_CODE | LOG_OPTION_STACK)) {
            sLogMemory(logOptions, logLineProc, logLineProcParam, reinterpret_cast<uint8_t*>(pc), reinterpret_cast<uint8_t*>(sp));
        }

        sLogSeparatorLine(logLineProc, logLineProcParam, '-', 1);
        sShowOutOfMemory(logLineProc, logLineProcParam);
    }

    __atomic_sub_fetch(&sgRecursionLevel, 1, __ATOMIC_SEQ_CST);
}

// Minidumps are a Windows debugging format; macOS writes its own crash reports (with the full
// thread and image state) to ~/Library/Logs/DiagnosticReports for any crash that reaches it.

int32_t LogMiniDump(void* logfile, EXCEPTION_POINTERS* exceptionpointers, uint32_t count, const char** strings) {
    return 0;
}

int32_t LogMiniDumpIsAvailable() {
    return 0;
}
