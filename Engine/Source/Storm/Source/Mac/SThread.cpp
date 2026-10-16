#include <windows.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <vector>
#include "Engine/Source/Storm/Source/SThread.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/Sync.h"

extern char** environ;

struct PROCESSCOMPLETION {
    void (*callback)(void*);
    void* data;
    pid_t process;
};

struct THREADLAUNCH {
    uint32_t (STORMAPI* threadProc)(void*);
    void* param;
    HANDLE threadH;
};

namespace S_Thread {
    SThreadTrack s_threads[s_maxthreads];
    int32_t s_numthreads;
    CInitCritSect s_threadCrit;
}

uint32_t STORMAPI SGetCurrentThreadId() {
    return GetCurrentThreadId();
}

uint32_t STORMAPI SGetCurrentProcessId() {
    return GetCurrentProcessId();
}

int32_t STORMAPI SGetCurrentThreadPriority() {
    return GetThreadPriority(GetCurrentThread());
}

int32_t STORMAPI SSetCurrentThreadPriority(int32_t priority) {
    return SetThreadPriority(GetCurrentThread(), priority);
}

int32_t STORMAPI SSetCurrentProcessAffinityMask(size_t mask) {
    // macOS does not bind processes to processors; report success as the call has no effect to fail.
    return 1;
}

int32_t CInitCritSect::Enter() {
    int32_t result = 0;

    if (!this->m_critsect) {
        while (InterlockedExchange(&this->m_spinLock, 1)) {
        }

        if (!this->m_critsect) {
            this->m_critsect = reinterpret_cast<CCritSect*>(this->m_critsectData);
            new (this->m_critsectData) CCritSect();
            result = 1;
        }

        this->m_spinLock = 0;
    }

    this->m_critsect->Enter();
    return result;
}

void CInitCritSect::Leave() {
    this->m_critsect->Leave();
}

static uint32_t STORMAPI ProcessCompletionCallbackThread(void* vdata) {
    PROCESSCOMPLETION* completion = static_cast<PROCESSCOMPLETION*>(vdata);
    int32_t status;

    while (waitpid(completion->process, &status, 0) < 0 && errno == EINTR) {
    }

    completion->callback(completion->data);
    SMemFree(completion, __FILE__, __LINE__, 0);

    return 0;
}

uint32_t STORMAPI S_Thread::s_SLaunchThread(void* threadParam) {
    s_threadCrit.Enter();

    THREADLAUNCH* launch = static_cast<THREADLAUNCH*>(threadParam);
    uint32_t (STORMAPI* threadProc)(void*) = launch->threadProc;
    void* param = launch->param;
    uint32_t threadId = GetCurrentThreadId();
    const char* threadName = nullptr;

    for (int32_t i = 0; i < s_numthreads; i++) {
        if (s_threads[i].threadId == threadId) {
            threadName = s_threads[i].name;
            break;
        }
    }

    SMemFree(launch, __FILE__, __LINE__, 0);

    // Windows names the thread for the debugger through MS_VC_EXCEPTION; macOS names it directly.
    MacSync::SetCurrentThreadName(threadName);
    s_threadCrit.Leave();

    uint32_t threadVal = threadProc(param);

    s_threadCrit.Enter();

    int32_t numthreads = s_numthreads;

    for (int32_t i = 0; i < numthreads; i++) {
        if (s_threads[i].live && s_threads[i].threadId == threadId) {
            // The ranges overlap; the Windows version's forward memcpy is a memmove
            memmove(&s_threads[i], &s_threads[i + 1], (s_maxthreads - 1 - i) * sizeof(SThreadTrack));
            numthreads--;
        }
    }

    s_numthreads = numthreads;
    s_threadCrit.Leave();

    return threadVal;
}

void* SCreateThread(uint32_t dwStackSize, uint32_t (STORMAPI* lpStartAddress)(void*), void* lpParameter, uint32_t dwCreationFlags, uint32_t* lpThreadId, char* threadName) {
    if (!threadName) {
        threadName = const_cast<char*>("");
    }

    S_Thread::s_threadCrit.Enter();

    if (!S_Thread::s_numthreads) {
        S_Thread::SThreadTrack* track = &S_Thread::s_threads[S_Thread::s_numthreads];
        track->threadId = GetCurrentThreadId();
        track->threadH = nullptr;
        track->live = 1;
        track->suspended = 0;
        SStrCopy(track->name, "main", sizeof(track->name));
        S_Thread::s_numthreads++;
    }

    S_Thread::s_threadCrit.Leave();

    THREADLAUNCH* launch = static_cast<THREADLAUNCH*>(SMemAlloc(sizeof(THREADLAUNCH), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
    launch->threadProc = lpStartAddress;
    launch->param = lpParameter;

    // The new thread looks itself up in s_threads before it runs, so hold the lock until its entry
    // is in place, as the Windows version does.
    S_Thread::s_threadCrit.Enter();

    uint32_t threadId = 0;
    HANDLE hThread = CreateThread(nullptr, dwStackSize, reinterpret_cast<LPTHREAD_START_ROUTINE>(S_Thread::s_SLaunchThread), launch, dwCreationFlags, reinterpret_cast<LPDWORD>(&threadId));
    launch->threadH = hThread;

    if (lpThreadId) {
        *lpThreadId = threadId;
    }

    S_Thread::SThreadTrack* track = &S_Thread::s_threads[S_Thread::s_numthreads];
    track->threadId = threadId;
    track->threadH = launch->threadH;
    track->live = 1;
    track->suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    SStrCopy(track->name, threadName, sizeof(track->name));

    S_Thread::s_numthreads++;
    S_Thread::s_threadCrit.Leave();

    return hThread;
}

int32_t STORMAPI SGetThreadName(uint32_t threadId, char* buffer, int32_t bufferchars) {
    bool found = false;

    if (!buffer || bufferchars < 1) {
        return 0;
    }

    if (bufferchars > static_cast<int32_t>(sizeof(S_Thread::s_threads[0].name))) {
        bufferchars = sizeof(S_Thread::s_threads[0].name);
    }

    *buffer = 0;

    S_Thread::s_threadCrit.Enter();

    for (int32_t i = 0; i < S_Thread::s_numthreads; i++) {
        if (S_Thread::s_threads[i].threadId == threadId) {
            SStrCopy(buffer, S_Thread::s_threads[i].name, bufferchars);
            found = true;
            break;
        }
    }

    S_Thread::s_threadCrit.Leave();

    return found;
}

void* STORMAPI SCreateThread(uint32_t (STORMAPI* lpStartAddress)(void*), void* lpParameter, uint32_t* lpThreadId, void* linuxData, char* threadName, uint32_t stackSize) {
    return SCreateThread(stackSize, lpStartAddress, lpParameter, 0, lpThreadId, threadName);
}

// Splits a Windows command line into arguments with the CommandLineToArgvW rules: whitespace
// separates arguments, double quotes group, 2n backslashes before a quote give n backslashes and
// a quote toggle, 2n+1 give n backslashes and a literal quote.
static std::vector<std::string> SplitCommandLine(const char* commandLine) {
    std::vector<std::string> args;
    const char* c = commandLine;

    while (*c) {
        while (*c == ' ' || *c == '\t') {
            c++;
        }

        if (!*c) {
            break;
        }

        std::string arg;
        bool quoted = false;

        while (*c && (quoted || (*c != ' ' && *c != '\t'))) {
            if (*c == '\\') {
                size_t backslashes = 0;

                while (*c == '\\') {
                    backslashes++;
                    c++;
                }

                if (*c == '"') {
                    arg.append(backslashes / 2, '\\');

                    if (backslashes % 2) {
                        arg.push_back('"');
                        c++;
                    }
                } else {
                    arg.append(backslashes, '\\');
                }
            } else if (*c == '"') {
                quoted = !quoted;
                c++;
            } else {
                arg.push_back(*c++);
            }
        }

        args.push_back(arg);
    }

    return args;
}

int32_t STORMAPI SCreateProcess(const char* appName, char* commandLine, void (*callbackWhenProcessCompletes)(void*), void* callbackData) {
    std::vector<std::string> args = SplitCommandLine(commandLine ? commandLine : "");

    if (args.empty() && !appName) {
        return 0;
    }

    std::string path = appName ? appName : args[0];

    if (args.empty()) {
        args.push_back(path);
    }

    std::vector<char*> argv;

    for (std::string& arg : args) {
        argv.push_back(&arg[0]);
    }

    argv.push_back(nullptr);

    pid_t pid;

    if (posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return 0;
    }

    if (callbackWhenProcessCompletes) {
        PROCESSCOMPLETION* completion = static_cast<PROCESSCOMPLETION*>(SMemAlloc(sizeof(PROCESSCOMPLETION), __FILE__, __LINE__, 0));
        completion->data = callbackData;
        completion->callback = callbackWhenProcessCompletes;
        completion->process = pid;

        uint32_t threadId;
        SCreateThread(ProcessCompletionCallbackThread, completion, &threadId, nullptr, nullptr, 0);
    }

    return 1;
}
