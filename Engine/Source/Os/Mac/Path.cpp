#include <cstdint>
#include <cstring>
#include "Engine/Source/Os/Path.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/CommandLine.h"
#include "Platform/Mac/Paths.h"

// The module path of the client is the application bundle in the client directory, the counterpart
// of Wow.exe; see Platform/Mac/Paths.h.

uint32_t GetModuleFileNameUTF8(void* module, char* path, uint32_t size) {
    if (!path || !size) {
        return 0;
    }

    char executable[4096];

    if (!MacPaths::ExecutablePath(executable, sizeof(executable))) {
        path[0] = '\0';
        return 0;
    }

    char* bundle = strstr(executable, ".app/Contents/MacOS/");

    if (bundle) {
        bundle[4] = '\0';
    }

    SStrCopy(path, executable, size);

    return static_cast<uint32_t>(strlen(path));
}

CommandLineUTF8::CommandLineUTF8() {
    const char* commandLine = MacCommandLine::Get();
    uint32_t size = static_cast<uint32_t>(strlen(commandLine) + 1);
    m_commandLine = static_cast<char*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    memcpy(m_commandLine, commandLine, size);
}

CommandLineUTF8::~CommandLineUTF8() {
    SMemFree(m_commandLine, __FILE__, __LINE__, 0);
}
