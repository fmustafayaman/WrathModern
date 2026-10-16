// Implementation of the curated <windows.h> subset in Platform/Mac/Include/windows.h.

#include <windows.h>
#include <cstdint>
#include <sched.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "Platform/Mac/PEResources.h"
#include "Platform/Mac/Sync.h"

// Pseudo handles, with the values Windows uses for them.
static HANDLE const kCurrentProcess = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
static HANDLE const kCurrentThread = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2));

static MacSync::Waitable* ToWaitable(HANDLE handle) {
    return static_cast<MacSync::Waitable*>(handle);
}

HANDLE CreateEventA(void* attributes, BOOL manualReset, BOOL initialState, LPCSTR name) {
    return MacSync::EventCreate(manualReset != FALSE, initialState != FALSE);
}

BOOL SetEvent(HANDLE event) {
    MacSync::EventSet(ToWaitable(event));
    return TRUE;
}

BOOL ResetEvent(HANDLE event) {
    MacSync::EventReset(ToWaitable(event));
    return TRUE;
}

HANDLE CreateMutexA(void* attributes, BOOL initialOwner, LPCSTR name) {
    return MacSync::MutexCreate(initialOwner != FALSE);
}

BOOL ReleaseMutex(HANDLE mutex) {
    return MacSync::MutexRelease(ToWaitable(mutex)) ? TRUE : FALSE;
}

HANDLE CreateSemaphoreA(void* attributes, LONG initialCount, LONG maximumCount, LPCSTR name) {
    if (maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount) {
        return nullptr;
    }

    return MacSync::SemaphoreCreate(initialCount, maximumCount);
}

BOOL ReleaseSemaphore(HANDLE semaphore, LONG releaseCount, LONG* previousCount) {
    // The previous count is not tracked by any caller in the client; report it as unknown (0).
    if (previousCount) {
        *previousCount = 0;
    }

    return MacSync::SemaphoreRelease(ToWaitable(semaphore), releaseCount) ? TRUE : FALSE;
}

struct ThreadStart {
    LPTHREAD_START_ROUTINE m_start;
    LPVOID m_param;
};

static uint32_t ThreadStartTrampoline(void* param) {
    ThreadStart start = *static_cast<ThreadStart*>(param);
    delete static_cast<ThreadStart*>(param);

    return start.m_start(start.m_param);
}

HANDLE CreateThread(void* attributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD threadId) {
    // No caller in the client creates a suspended thread with this API and resumes it later.
    ThreadStart* trampoline = new ThreadStart { start, param };
    uint32_t id = 0;
    MacSync::Waitable* thread = MacSync::ThreadCreate(ThreadStartTrampoline, trampoline, static_cast<uint32_t>(stackSize), &id);

    if (!thread) {
        delete trampoline;
        return nullptr;
    }

    if (threadId) {
        *threadId = id;
    }

    return thread;
}

BOOL CloseHandle(HANDLE object) {
    if (!object || object == kCurrentProcess || object == kCurrentThread) {
        return object ? TRUE : FALSE;
    }

    MacSync::Close(ToWaitable(object));

    return TRUE;
}

DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds) {
    if (!object || object == kCurrentProcess || object == kCurrentThread) {
        return WAIT_FAILED;
    }

    return MacSync::Wait(ToWaitable(object), milliseconds) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* objects, BOOL waitAll, DWORD milliseconds) {
    if (!count || count > MAXIMUM_WAIT_OBJECTS) {
        return WAIT_FAILED;
    }

    MacSync::Waitable* waitables[MAXIMUM_WAIT_OBJECTS];

    for (DWORD i = 0; i < count; i++) {
        waitables[i] = ToWaitable(objects[i]);
    }

    return waitAll ? MacSync::WaitAll(waitables, count, milliseconds) : MacSync::WaitAny(waitables, count, milliseconds);
}

void InitializeCriticalSection(CRITICAL_SECTION* section) {
    MacSync::RecursiveMutexInit(&section->m_mutex);
    section->DebugInfo = nullptr;
}

BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* section, DWORD spinCount) {
    MacSync::RecursiveMutexInit(&section->m_mutex);
    section->DebugInfo = nullptr;
    return TRUE;
}

void DeleteCriticalSection(CRITICAL_SECTION* section) {
    MacSync::RecursiveMutexDestroy(&section->m_mutex);
}

void EnterCriticalSection(CRITICAL_SECTION* section) {
    pthread_mutex_lock(&section->m_mutex);
}

void LeaveCriticalSection(CRITICAL_SECTION* section) {
    pthread_mutex_unlock(&section->m_mutex);
}

BOOL TryEnterCriticalSection(CRITICAL_SECTION* section) {
    return pthread_mutex_trylock(&section->m_mutex) == 0 ? TRUE : FALSE;
}

static thread_local DWORD s_lastError;

void SetLastError(DWORD error) {
    s_lastError = error;
}

DWORD GetLastError() {
    return s_lastError;
}

void Sleep(DWORD milliseconds) {
    if (!milliseconds) {
        // Sleep(0) gives up the rest of the time slice.
        sched_yield();
        return;
    }

    usleep(static_cast<useconds_t>(milliseconds) * 1000);
}

BOOL SwitchToThread() {
    sched_yield();
    return TRUE;
}

DWORD GetTickCount() {
    return static_cast<DWORD>(MacSync::MonotonicMilliseconds());
}

DWORD GetCurrentThreadId() {
    return MacSync::CurrentThreadId();
}

DWORD GetCurrentProcessId() {
    return static_cast<DWORD>(getpid());
}

HANDLE GetCurrentThread() {
    return kCurrentThread;
}

HANDLE GetCurrentProcess() {
    return kCurrentProcess;
}

// SetThreadPriority and GetThreadPriority are only called on the current thread through the
// GetCurrentThread pseudo handle.

static int32_t LevelForPriority(int32_t priority) {
    switch (priority) {
        case THREAD_PRIORITY_IDLE:
            return 0;
        case THREAD_PRIORITY_LOWEST:
            return 1;
        case THREAD_PRIORITY_BELOW_NORMAL:
            return 2;
        case THREAD_PRIORITY_NORMAL:
            return 3;
        case THREAD_PRIORITY_ABOVE_NORMAL:
            return 4;
        case THREAD_PRIORITY_HIGHEST:
            return 5;
        case THREAD_PRIORITY_TIME_CRITICAL:
            return 6;
        default:
            return -1;
    }
}

static const int32_t kPriorityForLevel[7] = {
    THREAD_PRIORITY_IDLE,
    THREAD_PRIORITY_LOWEST,
    THREAD_PRIORITY_BELOW_NORMAL,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_ABOVE_NORMAL,
    THREAD_PRIORITY_HIGHEST,
    THREAD_PRIORITY_TIME_CRITICAL,
};

BOOL SetThreadPriority(HANDLE thread, int32_t priority) {
    int32_t level = LevelForPriority(priority);

    if (level < 0) {
        return FALSE;
    }

    if (thread == kCurrentThread) {
        int32_t policy;
        sched_param param;
        pthread_getschedparam(pthread_self(), &policy, &param);
        int32_t low = sched_get_priority_min(policy);
        int32_t high = sched_get_priority_max(policy);
        param.sched_priority = low + (high - low) * level / 6;

        return pthread_setschedparam(pthread_self(), policy, &param) == 0 ? TRUE : FALSE;
    }

    return MacSync::ThreadSetPriority(ToWaitable(thread), level) ? TRUE : FALSE;
}

int32_t GetThreadPriority(HANDLE thread) {
    if (thread == kCurrentThread) {
        int32_t policy;
        sched_param param;

        if (pthread_getschedparam(pthread_self(), &policy, &param) != 0) {
            return THREAD_PRIORITY_ERROR_RETURN;
        }

        int32_t low = sched_get_priority_min(policy);
        int32_t high = sched_get_priority_max(policy);
        int32_t level = high > low ? ((param.sched_priority - low) * 6 + (high - low) / 2) / (high - low) : 3;

        return kPriorityForLevel[level < 0 ? 0 : (level > 6 ? 6 : level)];
    }

    int32_t level = MacSync::ThreadGetPriority(ToWaitable(thread));

    return kPriorityForLevel[level];
}

void GetSystemInfo(SYSTEM_INFO* info) {
    *info = {};

    int32_t processors = 1;
    size_t size = sizeof(processors);
    sysctlbyname("hw.logicalcpu", &processors, &size, nullptr, 0);

    // PROCESSOR_ARCHITECTURE_ARM64 is 12; the client only reads the processor count and page size.
    info->wProcessorArchitecture = 12;
    info->dwPageSize = static_cast<DWORD>(getpagesize());
    info->lpMinimumApplicationAddress = reinterpret_cast<LPVOID>(0x10000);
    info->lpMaximumApplicationAddress = reinterpret_cast<LPVOID>(0x7FFFFFFEFFFF);
    info->dwActiveProcessorMask = processors >= 64 ? ~static_cast<DWORD_PTR>(0) : ((static_cast<DWORD_PTR>(1) << processors) - 1);
    info->dwNumberOfProcessors = static_cast<DWORD>(processors);
    info->dwAllocationGranularity = 0x10000;
}

struct ResourceRecord {
    const void* m_data;
    uint32_t m_size;
};

HRSRC FindResourceExA(HMODULE module, LPCSTR type, LPCSTR name, WORD language) {
    uint32_t size = 0;
    const void* data = MacPEResources::Find(type, name, language, &size);

    if (!data) {
        return nullptr;
    }

    return new ResourceRecord { data, size };
}

HGLOBAL LoadResource(HMODULE module, HRSRC resource) {
    return resource ? const_cast<void*>(static_cast<ResourceRecord*>(resource)->m_data) : nullptr;
}

LPVOID LockResource(HGLOBAL data) {
    return data;
}

DWORD SizeofResource(HMODULE module, HRSRC resource) {
    return resource ? static_cast<ResourceRecord*>(resource)->m_size : 0;
}
