#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/sysctl.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

static bool s_continueAfterAssert;

static bool DebuggerAttached() {
    int32_t mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    kinfo_proc info = {};
    size_t size = sizeof(info);

    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) {
        return false;
    }

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

// Windows raises a breakpoint inside __try/__except, so it stops in a debugger and is ignored
// otherwise. A SIGTRAP without a debugger would terminate the process, so only raise it under one.
void Blizzard::Debug::Breakpoint() {
    if (DebuggerAttached()) {
        __builtin_debugtrap();
    }
}

void Blizzard::Debug::DefaultCallback(const char* expression, const char* message, const char* fileName, uint32_t lineNumber) {
    char text[2048];
    char canonical[256];
    char directory[512];
    Blizzard::Time::TimeRec rec;

    Blizzard::Time::BreakTime(Blizzard::Time::GetTimestamp(), rec);
    Blizzard::String::Format(text, sizeof(text), "%04d/%02d/%02d %02d:%02d:%02d  %s(%d) : Assertion failed: %s  \"%s\"\n", rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, CanonicalStr(fileName, canonical, sizeof(canonical)), lineNumber, expression, message);
    fprintf(stderr, "%s", text);

    Blizzard::Process::GetProcessDirectory(directory, sizeof(directory));
    Blizzard::String::Append(directory, "Errors", sizeof(directory));
    Blizzard::File::CreateDirectory(directory, true);
    Blizzard::String::Append(directory, "/Assert.log", sizeof(directory));

    FILE* stream = fopen(directory, "a");

    if (stream) {
        fprintf(stream, "%s", text);
        fclose(stream);
    }

    Breakpoint();

    if (!s_continueAfterAssert) {
        abort();
    }
}

void System_Debug::ShutdownProc() {
    // Windows unloads dbghelp.dll here; macOS symbolication needs no library.
}
