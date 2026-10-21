#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <new>
#include <unistd.h>
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

// The client clock on macOS. Windows calibrates QueryPerformanceCounter against GetTickCount and
// falls back to the tick count when it misbehaves; the macOS monotonic clock is always reliable, so
// it is the performance counter here (nanoseconds, including time asleep like QPC) and calibration
// always succeeds. The tick count method stays available for clients that ask for it.
// St0ny's precise timer patch (#5, skipping the 250 ms tick count comparison) therefore has nothing to change on
// macOS: the precise clock is always chosen unless timingMethod asks for the tick count.

static OsTimeManager* s_OsTimeMgr;

static int64_t PerformanceCounter() {
    return static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
}

static int64_t TickCount() {
    return PerformanceCounter() / 1000000;
}

OsTimeManager::TimingMethod OsTimeManager::Calibrate() {
    this->m_performanceFrequency = 1000000000;
    this->m_timingTestError = 0;

    return SystemMethod2;
}

uint32_t OsTimeGetTestError() {
    return s_OsTimeMgr->m_timingTestError;
}

OsTimeManager::TimingMethod OsTimeGetTimingMethod() {
    return s_OsTimeMgr->m_timingMethod;
}

const char* GetTimingMethodName(OsTimeManager::TimingMethod method) {
    switch (method) {
        case OsTimeManager::NotSet:
            return "[Not Set]";
        case OsTimeManager::BestAvailable:
            return "[Best Available]";
        case OsTimeManager::SystemMethod1:
            return "Tick Count";
        case OsTimeManager::SystemMethod2:
            return "Monotonic Clock";
        default:
            return "[Unknown]";
    }
}

uint64_t OsTimeManager::Snapshot() {
    if (this->m_timingMethod == SystemMethod2) {
        return static_cast<int64_t>(static_cast<double>(PerformanceCounter()) * this->m_scaleToMs + this->m_timeBegin);
    }

    return static_cast<int64_t>(static_cast<double>(TickCount()) * this->m_scaleToMs + this->m_timeBegin);
}

uint32_t OsGetAsyncTimeMs() {
    return static_cast<uint32_t>(s_OsTimeMgr->Snapshot());
}

uint64_t OsGetAsyncTimeClocks() {
    if (s_OsTimeMgr->m_timingMethod == OsTimeManager::SystemMethod2) {
        return PerformanceCounter();
    }

    return TickCount();
}

int64_t OsGetAsyncClocksPerSecond() {
    if (s_OsTimeMgr->m_timingMethod == OsTimeManager::SystemMethod2) {
        return s_OsTimeMgr->m_performanceFrequency;
    }

    return 1000;
}

OsTimeManager::OsTimeManager(OsTimeManager::TimingMethod method) {
    this->m_timingMethod = NotSet;
    OsTimeManager::TimingMethod calibrated = this->Calibrate();

    if (method != BestAvailable && calibrated != method) {
        calibrated = method;
    }

    this->m_timingMethod = calibrated;
    int64_t clocksPerSecond = calibrated == SystemMethod2 ? this->m_performanceFrequency : 1000;
    this->m_scaleToMs = 1000.0 / static_cast<double>(clocksPerSecond);
    this->m_timeBegin = 0.0;
}

void OsTimeManagerCreate(OsTimeManager::TimingMethod method) {
    s_OsTimeMgr = new (SMemAlloc(sizeof(OsTimeManager), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY)) OsTimeManager(method);
}

void OsTimeManagerDestroy() {
    if (s_OsTimeMgr) {
        delete s_OsTimeMgr;
        s_OsTimeMgr = nullptr;
    }
}

void OsSleep(uint32_t milliseconds) {
    if (!milliseconds) {
        sched_yield();
        return;
    }

    usleep(static_cast<useconds_t>(milliseconds) * 1000);
}
