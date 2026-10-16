#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Platform/Mac/Sync.h"

// Mutex is a CRITICAL_SECTION, which the macOS build defines around a recursive pthread mutex.

void Blizzard::Lock::MutexCreate(Mutex& mutex) {
    MacSync::RecursiveMutexInit(&mutex.m_mutex);
    mutex.DebugInfo = nullptr;
}

void Blizzard::Lock::MutexEnter(Mutex& mutex) {
    pthread_mutex_lock(&mutex.m_mutex);
}

void Blizzard::Lock::MutexLeave(Mutex& mutex) {
    pthread_mutex_unlock(&mutex.m_mutex);
}

void Blizzard::Lock::MutexDestroy(Mutex& mutex) {
    MacSync::RecursiveMutexDestroy(&mutex.m_mutex);
    mutex.DebugInfo = nullptr;
}

Blizzard::Lock::ScopedMutex::ScopedMutex(Mutex& mutex) {
    this->m_mutex = &mutex;
    MutexEnter(mutex);
}

Blizzard::Lock::ScopedMutex::~ScopedMutex() {
    MutexLeave(*this->m_mutex);
}

Blizzard::Lock::RMutex::RMutex() {
    MutexCreate(this->m_mutex);
}

Blizzard::Lock::RMutex::~RMutex() {
    MutexDestroy(this->m_mutex);
}
