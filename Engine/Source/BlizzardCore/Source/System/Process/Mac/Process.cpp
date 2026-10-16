#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/TempBuffer.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Platform/Mac/Paths.h"

extern char** environ;

// The process path is the client's path as the shared code expects it: the executable that sits in
// the client directory. Inside an application bundle that is the bundle itself (see Platform/Mac/
// Paths.h), so GetProcessDirectory yields the directory that holds Data/ and WTF/.

bool Blizzard::Process::GetProcessPath(char* buffer, int32_t buffersize) {
    if (!buffer || buffersize <= 0) {
        return false;
    }

    char path[4096];

    if (!MacPaths::ExecutablePath(path, sizeof(path))) {
        *buffer = '\0';
        return false;
    }

    char* bundle = strstr(path, ".app/Contents/MacOS/");

    if (bundle) {
        bundle[4] = '\0';
    }

    System_File::FromNativeName(buffer, buffersize, path);

    return true;
}

bool Blizzard::Process::GetProcessDirectory(char* buffer, int32_t buffersize) {
    Blizzard::Memory::TempBuffer<char, 1024> path(buffersize);

    if (!GetProcessPath(path.m_data, buffersize)) {
        return false;
    }

    if (path.m_data) {
        if (buffer) {
            int32_t length = static_cast<int32_t>(Blizzard::String::FindFilename(path.m_data) - path.m_data + 1);
            Blizzard::String::Copy(buffer, path.m_data, std::min(length, buffersize));
        }
    } else if (buffer && buffersize > 0) {
        *buffer = '\0';
    }

    return true;
}

struct ChildProcess {
    pid_t m_process;
    void (*m_callback)(void*);
    void* m_param;
};

// Windows polls finished children from a timer on the thread that launched them. Here a detached
// thread waits for the child and then runs the callback.
static uint32_t WaitForChild(void* param) {
    ChildProcess* child = static_cast<ChildProcess*>(param);
    int32_t status;

    while (waitpid(child->m_process, &status, 0) < 0 && errno == EINTR) {
    }

    child->m_callback(child->m_param);
    delete child;

    return 0;
}

bool Blizzard::Process::LaunchChildProcessWithArguments(const char* path, const char* arguments, void (*callback)(void*), void* param) {
    if (!arguments) {
        arguments = "";
    }

    // Arguments arrive one per line, as the Windows version reads them.
    std::vector<std::string> args;
    args.push_back(path);

    while (*arguments) {
        size_t length = strcspn(arguments, "\n");

        if (length) {
            args.push_back(std::string(arguments, length));
        }

        arguments += length;
        arguments += strspn(arguments, "\n");
    }

    std::vector<char*> argv;

    for (std::string& arg : args) {
        argv.push_back(&arg[0]);
    }

    argv.push_back(nullptr);

    pid_t pid;

    if (posix_spawn(&pid, path, nullptr, nullptr, argv.data(), environ) != 0) {
        return false;
    }

    if (!callback) {
        return true;
    }

    ChildProcess* child = new ChildProcess { pid, callback, param };

    return Blizzard::Thread::CreateDetached(WaitForChild, child, "Child Process Watcher");
}

void Blizzard::Process::Sleep(uint32_t milliseconds) {
    usleep(static_cast<useconds_t>(milliseconds) * 1000);
}
