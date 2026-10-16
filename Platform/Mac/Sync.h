// Win32-style synchronization objects for the macOS platform layer.
//
// The client's platform code is built around Win32 kernel objects: auto- and manual-reset events,
// recursive critical sections, counting semaphores, joinable thread handles and waits with
// millisecond timeouts, including waits on several objects at once. The Mac/ implementations of
// BlizzardCore, Storm and FMOD Ex use these objects so they keep the exact semantics the shared code
// was written against.
//
// Every object is reference-free and owned by whoever created it. Waits take Win32-style timeouts:
// milliseconds, with MacSync::kInfinite for no timeout.

#ifndef PLATFORM_MAC_SYNC_H
#define PLATFORM_MAC_SYNC_H

#include <cstdint>
#include <pthread.h>

namespace MacSync {

constexpr uint32_t kInfinite = 0xFFFFFFFF;

// Return values of WaitAny, matching WAIT_OBJECT_0 and WAIT_TIMEOUT.
constexpr uint32_t kWaitObject0 = 0;
constexpr uint32_t kWaitTimeout = 0x102;

class Waitable;

// Monotonic clock

uint64_t MonotonicNanoseconds();
uint64_t MonotonicMilliseconds();

// Recursive mutex with critical-section semantics

void RecursiveMutexInit(pthread_mutex_t* mutex);
void RecursiveMutexDestroy(pthread_mutex_t* mutex);

// Events

Waitable* EventCreate(bool manualReset, bool initialState);
void EventDestroy(Waitable* event);
void EventSet(Waitable* event);
void EventReset(Waitable* event);

// Mutexes with Win32 mutex semantics: owned by the thread that acquired them, reentrant, released
// by the owner. Waiting on one acquires it.

Waitable* MutexCreate(bool initialOwner);
bool MutexRelease(Waitable* mutex);

// Semaphores

Waitable* SemaphoreCreate(int32_t initialCount, int32_t maximumCount);
void SemaphoreDestroy(Waitable* semaphore);
bool SemaphoreRelease(Waitable* semaphore, int32_t count);

// Threads. The handle stays valid until ThreadClose, after the thread has finished or not; waiting
// on it returns once the thread function has returned.

typedef uint32_t (*ThreadFunction)(void* param);

Waitable* ThreadCreate(ThreadFunction function, void* param, uint32_t stackSize, uint32_t* threadId);
void ThreadClose(Waitable* thread);
bool ThreadIsCurrent(Waitable* thread);
uint32_t CurrentThreadId();
void SetCurrentThreadName(const char* name);

// Thread priority on the Win32 scale the client uses: 0 idle, 1 lowest, 2 below normal, 3 normal,
// 4 above normal, 5 highest, 6 time critical.
bool ThreadSetPriority(Waitable* thread, int32_t level);
int32_t ThreadGetPriority(Waitable* thread);

// Releases any object above, like CloseHandle.
void Close(Waitable* object);

// Waiting

// Waits for one object. Returns true when it became signaled within the timeout. Waiting consumes
// the signal of an auto-reset event or one count of a semaphore.
bool Wait(Waitable* object, uint32_t timeoutMs);

// Waits for any one of several objects, like WaitForMultipleObjects(count, objects, FALSE, timeout).
// Returns kWaitObject0 + index of the object that satisfied the wait, or kWaitTimeout.
uint32_t WaitAny(Waitable* const* objects, uint32_t count, uint32_t timeoutMs);

// Waits until all objects are signaled at once, like WaitForMultipleObjects(..., TRUE, timeout).
// Returns kWaitObject0 or kWaitTimeout.
uint32_t WaitAll(Waitable* const* objects, uint32_t count, uint32_t timeoutMs);

}

#endif
