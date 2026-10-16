#include "Platform/Mac/Sync.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

// All waitable objects share one lock and one condition variable, the way a kernel serializes
// object state. That makes waits on several objects atomic (WaitAll takes every object in one step,
// WaitAny never consumes two signals) at a cost that does not matter at the client's thread counts.

namespace MacSync {

enum WaitableKind {
    WAITABLE_EVENT,
    WAITABLE_SEMAPHORE,
    WAITABLE_THREAD,
    WAITABLE_MUTEX,
};

class Waitable {
    public:
    WaitableKind m_kind;
    std::atomic<int32_t> m_refCount;

    // Event and thread: signaled state. Thread handles are manual-reset events set on exit.
    bool m_manualReset;
    bool m_signaled;

    // Semaphore count, or mutex recursion count
    int32_t m_count;
    int32_t m_maximum;

    // Mutex owner thread id, 0 when free
    uint32_t m_owner;

    // Thread
    pthread_t m_thread;
    uint32_t m_threadId;
    ThreadFunction m_function;
    void* m_param;
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static std::atomic<uint32_t> s_nextThreadId { 1 };
static pthread_key_t s_threadIdKey;
static pthread_once_t s_threadIdOnce = PTHREAD_ONCE_INIT;

static void Release(Waitable* object) {
    if (object->m_refCount.fetch_sub(1) == 1) {
        free(object);
    }
}

static Waitable* NewWaitable(WaitableKind kind) {
    Waitable* object = static_cast<Waitable*>(calloc(1, sizeof(Waitable)));
    object->m_kind = kind;
    object->m_refCount.store(1);

    return object;
}

// Caller holds s_lock.
static bool IsSignaled(const Waitable* object, uint32_t thread) {
    switch (object->m_kind) {
        case WAITABLE_SEMAPHORE:
            return object->m_count > 0;
        case WAITABLE_MUTEX:
            return !object->m_owner || object->m_owner == thread;
        default:
            return object->m_signaled;
    }
}

// Caller holds s_lock and the object is signaled.
static void Consume(Waitable* object, uint32_t thread) {
    switch (object->m_kind) {
        case WAITABLE_MUTEX:
            object->m_owner = thread;
            object->m_count++;
            break;
        case WAITABLE_SEMAPHORE:
            object->m_count--;
            break;
        case WAITABLE_EVENT:
            if (!object->m_manualReset) {
                object->m_signaled = false;
            }
            break;
        case WAITABLE_THREAD:
            break;
    }
}

// Caller holds s_lock. Waits for a change until the deadline; returns false once it has passed.
static bool WaitForChange(uint64_t deadlineNs, bool infinite) {
    if (infinite) {
        pthread_cond_wait(&s_changed, &s_lock);
        return true;
    }

    uint64_t now = MonotonicNanoseconds();

    if (now >= deadlineNs) {
        return false;
    }

    uint64_t remaining = deadlineNs - now;
    timespec relative;
    relative.tv_sec = static_cast<time_t>(remaining / 1000000000ull);
    relative.tv_nsec = static_cast<long>(remaining % 1000000000ull);
    pthread_cond_timedwait_relative_np(&s_changed, &s_lock, &relative);

    return true;
}

static uint64_t Deadline(uint32_t timeoutMs) {
    return MonotonicNanoseconds() + static_cast<uint64_t>(timeoutMs) * 1000000ull;
}

uint64_t MonotonicNanoseconds() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t MonotonicMilliseconds() {
    return MonotonicNanoseconds() / 1000000ull;
}

void RecursiveMutexInit(pthread_mutex_t* mutex) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void RecursiveMutexDestroy(pthread_mutex_t* mutex) {
    pthread_mutex_destroy(mutex);
}

Waitable* EventCreate(bool manualReset, bool initialState) {
    Waitable* event = NewWaitable(WAITABLE_EVENT);
    event->m_manualReset = manualReset;
    event->m_signaled = initialState;

    return event;
}

void EventDestroy(Waitable* event) {
    if (event) {
        Release(event);
    }
}

void EventSet(Waitable* event) {
    pthread_mutex_lock(&s_lock);
    event->m_signaled = true;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
}

void EventReset(Waitable* event) {
    pthread_mutex_lock(&s_lock);
    event->m_signaled = false;
    pthread_mutex_unlock(&s_lock);
}

Waitable* MutexCreate(bool initialOwner) {
    Waitable* mutex = NewWaitable(WAITABLE_MUTEX);

    if (initialOwner) {
        mutex->m_owner = CurrentThreadId();
        mutex->m_count = 1;
    }

    return mutex;
}

bool MutexRelease(Waitable* mutex) {
    uint32_t thread = CurrentThreadId();
    pthread_mutex_lock(&s_lock);

    if (mutex->m_owner != thread) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }

    if (--mutex->m_count == 0) {
        mutex->m_owner = 0;
        pthread_cond_broadcast(&s_changed);
    }

    pthread_mutex_unlock(&s_lock);

    return true;
}

Waitable* SemaphoreCreate(int32_t initialCount, int32_t maximumCount) {
    Waitable* semaphore = NewWaitable(WAITABLE_SEMAPHORE);
    semaphore->m_count = initialCount;
    semaphore->m_maximum = maximumCount;

    return semaphore;
}

void SemaphoreDestroy(Waitable* semaphore) {
    if (semaphore) {
        Release(semaphore);
    }
}

bool SemaphoreRelease(Waitable* semaphore, int32_t count) {
    pthread_mutex_lock(&s_lock);

    // ReleaseSemaphore fails without changing the count when it would pass the maximum.
    if (count <= 0 || semaphore->m_count > semaphore->m_maximum - count) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }

    semaphore->m_count += count;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);

    return true;
}

static void CreateThreadIdKey() {
    pthread_key_create(&s_threadIdKey, nullptr);
}

static void* ThreadEntry(void* param) {
    Waitable* thread = static_cast<Waitable*>(param);

    pthread_once(&s_threadIdOnce, CreateThreadIdKey);
    pthread_setspecific(s_threadIdKey, reinterpret_cast<void*>(static_cast<uintptr_t>(thread->m_threadId)));

    thread->m_function(thread->m_param);

    pthread_mutex_lock(&s_lock);
    thread->m_signaled = true;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);

    Release(thread);

    return nullptr;
}

Waitable* ThreadCreate(ThreadFunction function, void* param, uint32_t stackSize, uint32_t* threadId) {
    Waitable* thread = NewWaitable(WAITABLE_THREAD);
    thread->m_manualReset = true;
    thread->m_function = function;
    thread->m_param = param;
    thread->m_threadId = s_nextThreadId.fetch_add(1);

    // One reference for the handle the caller gets, one for the running thread.
    thread->m_refCount.store(2);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // On Windows the requested size is the initial commit; every thread still reserves at least
    // the executable's default of 1 MB, and code relies on that depth. macOS stacks are fixed at
    // creation, so the stack is the larger of the two, in pages.
    size_t stack = stackSize > 1024 * 1024 ? stackSize : 1024 * 1024;
    size_t page = static_cast<size_t>(getpagesize());
    stack = (stack + page - 1) / page * page;

    if (stack < PTHREAD_STACK_MIN) {
        stack = PTHREAD_STACK_MIN;
    }

    pthread_attr_setstacksize(&attr, stack);

    if (threadId) {
        *threadId = thread->m_threadId;
    }

    int32_t result = pthread_create(&thread->m_thread, &attr, ThreadEntry, thread);
    pthread_attr_destroy(&attr);

    if (result != 0) {
        free(thread);
        return nullptr;
    }

    return thread;
}

void ThreadClose(Waitable* thread) {
    if (thread) {
        Release(thread);
    }
}

bool ThreadIsCurrent(Waitable* thread) {
    return thread && pthread_equal(thread->m_thread, pthread_self());
}

uint32_t CurrentThreadId() {
    pthread_once(&s_threadIdOnce, CreateThreadIdKey);
    uintptr_t id = reinterpret_cast<uintptr_t>(pthread_getspecific(s_threadIdKey));

    // Threads this layer did not start (the main thread, system threads) get an id on first use.
    if (!id) {
        id = s_nextThreadId.fetch_add(1);
        pthread_setspecific(s_threadIdKey, reinterpret_cast<void*>(id));
    }

    return static_cast<uint32_t>(id);
}

void SetCurrentThreadName(const char* name) {
    if (name && *name) {
        pthread_setname_np(name);
    }
}

// macOS schedules SCHED_OTHER threads by priority within the range the policy reports; spread the
// seven Win32 levels across it with normal in the middle.
static int32_t PriorityForLevel(int32_t level) {
    int32_t low = sched_get_priority_min(SCHED_OTHER);
    int32_t high = sched_get_priority_max(SCHED_OTHER);

    return low + (high - low) * level / 6;
}

bool ThreadSetPriority(Waitable* thread, int32_t level) {
    if (!thread || level < 0 || level > 6) {
        return false;
    }

    sched_param param;
    int32_t policy;

    if (pthread_getschedparam(thread->m_thread, &policy, &param) != 0) {
        return false;
    }

    param.sched_priority = PriorityForLevel(level);

    return pthread_setschedparam(thread->m_thread, policy, &param) == 0;
}

int32_t ThreadGetPriority(Waitable* thread) {
    sched_param param;
    int32_t policy;

    if (!thread || pthread_getschedparam(thread->m_thread, &policy, &param) != 0) {
        return 3;
    }

    int32_t best = 0;

    for (int32_t level = 1; level <= 6; level++) {
        if (abs(PriorityForLevel(level) - param.sched_priority) < abs(PriorityForLevel(best) - param.sched_priority)) {
            best = level;
        }
    }

    return best;
}

void Close(Waitable* object) {
    if (object) {
        Release(object);
    }
}

bool Wait(Waitable* object, uint32_t timeoutMs) {
    return WaitAny(&object, 1, timeoutMs) == kWaitObject0;
}

uint32_t WaitAny(Waitable* const* objects, uint32_t count, uint32_t timeoutMs) {
    bool infinite = timeoutMs == kInfinite;
    uint64_t deadline = infinite ? 0 : Deadline(timeoutMs);
    uint32_t thread = CurrentThreadId();

    pthread_mutex_lock(&s_lock);

    while (true) {
        for (uint32_t i = 0; i < count; i++) {
            if (IsSignaled(objects[i], thread)) {
                Consume(objects[i], thread);
                pthread_mutex_unlock(&s_lock);
                return kWaitObject0 + i;
            }
        }

        if (!WaitForChange(deadline, infinite)) {
            pthread_mutex_unlock(&s_lock);
            return kWaitTimeout;
        }
    }
}

uint32_t WaitAll(Waitable* const* objects, uint32_t count, uint32_t timeoutMs) {
    bool infinite = timeoutMs == kInfinite;
    uint64_t deadline = infinite ? 0 : Deadline(timeoutMs);
    uint32_t thread = CurrentThreadId();

    pthread_mutex_lock(&s_lock);

    while (true) {
        bool all = true;

        for (uint32_t i = 0; i < count && all; i++) {
            all = IsSignaled(objects[i], thread);
        }

        if (all) {
            for (uint32_t i = 0; i < count; i++) {
                Consume(objects[i], thread);
            }

            pthread_mutex_unlock(&s_lock);
            return kWaitObject0;
        }

        if (!WaitForChange(deadline, infinite)) {
            pthread_mutex_unlock(&s_lock);
            return kWaitTimeout;
        }
    }
}

}
