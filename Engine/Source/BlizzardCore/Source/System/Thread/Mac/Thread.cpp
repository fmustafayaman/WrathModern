#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"
#include "Platform/Mac/Sync.h"

// ThreadRecord::m_handle holds a MacSync thread handle. Thread-local storage uses pthread keys,
// stored inverted in TLSSlot::m_key like the Win32 TLS index so that zero still means "unallocated".
//
// Windows starts a watcher thread per foreign thread (one Blizzard::Thread did not create) to run its
// TLS destructors when it exits. pthread keys already run destructors at thread exit, so here one key
// carries that duty: a foreign thread that registers a destructor gets a non-null value in
// s_externalExitKey, and the key's destructor runs the thread's registered destructors.

bool System_Thread::s_initialized;
Blizzard::Lock::Mutex System_Thread::s_mutex;
Blizzard::Lock::Mutex System_Thread::s_registryMutex;
Blizzard::Thread::TLSSlot System_Thread::s_threadRecordTLS;
Blizzard::Thread::TLSSlot System_Thread::s_stackTraceEntryPointTLS;
Blizzard::Thread::ThreadRecord* System_Thread::s_mainThread;
std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>* System_Thread::s_ThreadRegistry;
int32_t System_Thread::s_DefaultStackSize;
System_Thread::ExternalTLSRecord System_Thread::s_externalTLS[32];
int32_t System_Thread::s_externalTLSCount;

static pthread_key_t s_externalExitKey;

static HANDLE ExternalThreadHandle(uint32_t threadId) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(threadId));
}

static void ExternalThreadExit(void* value) {
    System_Thread::RunExternalTLSDestructors(ExternalThreadHandle(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value))));
}

bool Blizzard::Thread::Sleep(uint32_t milliseconds) {
    usleep(static_cast<useconds_t>(milliseconds) * 1000);
    return true;
}

bool Blizzard::Thread::IsRunning(ThreadRecord* thread) {
    return thread->m_state >= 0;
}

bool System_Thread::SetPriorityLevel(Blizzard::Thread::ThreadRecord* thread, int32_t level) {
    return MacSync::ThreadSetPriority(static_cast<MacSync::Waitable*>(thread->m_handle), level);
}

bool System_Thread::GetPriorityLevel(Blizzard::Thread::ThreadRecord* thread, int32_t* level) {
    // The 12340 client's switch falls through every case, so any recognized priority reads back as
    // level 6 and the function reports failure. Pool workers restore that level after each task.
    (void)MacSync::ThreadGetPriority(static_cast<MacSync::Waitable*>(thread->m_handle));
    *level = 6;

    return false;
}

void System_Thread::ShutdownThreadLocalStorage() {
    if (!s_initialized) {
        return;
    }

    // Windows wakes each watcher so it runs its thread's destructors now. Do the same directly.
    Blizzard::Lock::MutexEnter(s_mutex);

    while (s_externalTLSCount > 0) {
        HANDLE thread = s_externalTLS[s_externalTLSCount - 1].m_thread;
        Blizzard::Lock::MutexLeave(s_mutex);
        RunExternalTLSDestructors(thread);
        Blizzard::Lock::MutexEnter(s_mutex);
    }

    Blizzard::Lock::MutexLeave(s_mutex);
}

void System_Thread::RunExternalTLSDestructors(HANDLE thread) {
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    for (int32_t i = s_externalTLSCount; i != 0;) {
        i--;
        ExternalTLSRecord& record = s_externalTLS[i];

        if (record.m_thread == thread) {
            if (record.m_destructor) {
                record.m_destructor(record.m_value);
            }

            s_externalTLSCount--;
            record = s_externalTLS[s_externalTLSCount];
        }
    }
}

void System_Thread::AddToRegistry(Blizzard::Thread::ThreadRecord* thread) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);

    if (!s_ThreadRegistry) {
        s_ThreadRegistry = new std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>();
    }

    s_ThreadRegistry->insert(thread);
}

Blizzard::Thread::ThreadRecord* System_Thread::NewThread(uint32_t (*function)(void*), void* param, const char* name) {
    if (!name) {
        name = "";
    }

    uint32_t length = static_cast<uint32_t>(strlen(name));
    Blizzard::Thread::ThreadRecord* thread = static_cast<Blizzard::Thread::ThreadRecord*>(malloc(sizeof(Blizzard::Thread::ThreadRecord) + length + 1));
    memset(thread, 0, sizeof(Blizzard::Thread::ThreadRecord));
    thread->m_param = param;
    thread->m_function = function;
    thread->m_state = 0;
    thread->m_refCount.m_value = 2;
    Blizzard::String::Copy(thread->m_name, name, length + 1);
    AddToRegistry(thread);

    return thread;
}

void System_Thread::RemoveFromRegistry(Blizzard::Thread::ThreadRecord* thread) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);
    s_ThreadRegistry->erase(thread);

    if (s_ThreadRegistry->empty()) {
        delete s_ThreadRegistry;
        s_ThreadRegistry = nullptr;
    }
}

void System_Thread::InitMainThread() {
    s_mainThread = NewThread(nullptr, nullptr, nullptr);
    s_mainThread->m_threadId = MacSync::CurrentThreadId();
    InternalSetLocalStorage(&s_threadRecordTLS, s_mainThread);
    s_mainThread->m_state = 1;
    s_mainThread->m_refCount.m_value = 1;
}

void System_Thread::InitThreadSystem() {
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    Blizzard::Lock::MutexCreate(s_mutex);
    Blizzard::Lock::MutexCreate(s_registryMutex);
    pthread_key_create(&s_externalExitKey, ExternalThreadExit);
    AllocateLocalStorage(&s_threadRecordTLS, nullptr);
    AllocateLocalStorage(&s_stackTraceEntryPointTLS, nullptr);
    InitMainThread();
}

void System_Thread::SetStackTraceEntryPoint(void* entryPoint) {
    InitThreadSystem();
    InternalSetLocalStorage(&s_stackTraceEntryPointTLS, entryPoint);
}

Blizzard::Thread::ThreadRecord* System_Thread::GetThreadRecord() {
    InitThreadSystem();

    return static_cast<Blizzard::Thread::ThreadRecord*>(pthread_getspecific(static_cast<pthread_key_t>(~s_threadRecordTLS.m_key)));
}

bool System_Thread::AllocateTLSSlot(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    InitThreadSystem();
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    return slot->m_key || AllocateLocalStorage(slot, destructor);
}

void System_Thread::InitStackTraceEntryPoint() {
    void* frames[3];

    // Frame 0 is this function and frame 1 ThreadStarter; frame 2 is what started the thread, as
    // RtlCaptureStackBackTrace(2, 1, ...) reports on Windows.
    if (backtrace(frames, 3) == 3) {
        SetStackTraceEntryPoint(frames[2]);
    }
}

uint32_t System_Thread::ExternalTLSWatcher(void* param) {
    // No watcher threads on macOS; see the comment at the top of this file.
    return 0;
}

uint32_t __stdcall System_Thread::ThreadStarter(void* param) {
    Blizzard::Thread::ThreadRecord* thread = static_cast<Blizzard::Thread::ThreadRecord*>(param);

    MacSync::SetCurrentThreadName(thread->m_name);
    InternalSetLocalStorage(&s_threadRecordTLS, thread);
    InitStackTraceEntryPoint();

    while (!thread->m_state) {
        usleep(1000);
    }

    uint32_t result = thread->m_function(thread->m_param);
    thread->m_result = result;
    RunTLSDestructors();
    thread->m_state = -1;
    Blizzard::Thread::Close(thread);

    return result;
}

bool Blizzard::Thread::Create(uint32_t (*function)(void*), void* param, ThreadRecord** thread, const char* name, int32_t flags, int32_t stackSize) {
    System_Thread::InitThreadSystem();
    ThreadRecord* record = System_Thread::NewThread(function, param, name);
    *thread = record;

    if (!stackSize) {
        stackSize = System_Thread::s_DefaultStackSize;
    }

    // A thread created with flag 1 (CREATE_SUSPENDED on Windows) is never resumed by the client; its
    // state stays 0, and ThreadStarter holds it before the thread function the same way.
    record->m_handle = MacSync::ThreadCreate(System_Thread::ThreadStarter, record, stackSize > 0 ? static_cast<uint32_t>(stackSize) : 0, &record->m_threadId);
    (*thread)->m_state = (flags & 1) == 0;

    return true;
}

void System_Thread::RegisterExternalTLSDestructor(void* value, Blizzard::Thread::TLSDestructor destructor) {
    uint32_t threadId = MacSync::CurrentThreadId();
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    if (s_externalTLSCount >= 32) {
        return;
    }

    ExternalTLSRecord& record = s_externalTLS[s_externalTLSCount++];
    record.m_thread = ExternalThreadHandle(threadId);
    record.m_watcher = nullptr;
    record.m_event = nullptr;
    record.m_threadId = threadId;
    record.m_value = value;
    record.m_destructor = destructor;

    pthread_setspecific(s_externalExitKey, reinterpret_cast<void*>(static_cast<uintptr_t>(threadId)));
}

void System_Thread::InternalSetLocalStorage(const Blizzard::Thread::TLSSlot* slot, const void* value) {
    pthread_setspecific(static_cast<pthread_key_t>(~slot->m_key), value);

    if (slot->m_destructor && value) {
        Blizzard::Thread::ThreadRecord* thread = GetThreadRecord();

        if (thread && thread->m_worker) {
            thread = thread->m_worker->m_task;
        }

        if (!thread) {
            RegisterExternalTLSDestructor(const_cast<void*>(value), slot->m_destructor);
        }
    }
}
