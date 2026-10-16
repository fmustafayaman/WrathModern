#include <cstdint>
#include <ctime>
#include <sys/time.h>
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// Blizzard::Time::Timestamp counts nanoseconds since 2000-01-01 00:00:00 UTC. Windows derives it from
// FILETIME (100 ns units since 1601); here it comes from the Unix epoch. GetTimestamp and
// Milliseconds keep the Windows structure: a wall-clock anchor taken once, advanced by a monotonic
// counter so timestamps never step backwards when the system clock is adjusted.

const int32_t System_Time::s_daysBeforeMonth[13] = { 0, 31, 59, 90, 120, 151, 182, 212, 243, 273, 304, 334, 365 };
uint64_t System_Time::timeScaleCycles = 0xFFFFFFFFFFFFFFFF;
bool System_Time::s_usePerformanceCounter;
Blizzard::Lock::Atomic System_Time::s_tickLock;
int64_t System_Time::s_absBegin;
uint32_t System_Time::s_lastTickCount;
double System_Time::timeScaleMilliseconds;
Blizzard::Time::Timestamp System_Time::s_gmBegin;
uint64_t System_Time::s_tickCountHigh;
double System_Time::timeScaleNanoseconds;
double System_Time::timeScaleSeconds;
double System_Time::timeScaleMicroseconds;

// Seconds from 1970-01-01 to 2000-01-01.
static const int64_t kUnixTo2000 = 946684800;

// The FILETIME range Windows accepts, expressed as Timestamps: FromFileTime clamps to it.
static const Blizzard::Time::Timestamp kTimestampMin = static_cast<Blizzard::Time::Timestamp>(0x8000000000000000);
static const Blizzard::Time::Timestamp kTimestampMax = 0x7FFFFFFFFFFFFFFF;

static uint64_t MonotonicCounter() {
    // Includes time the machine spends asleep, like QueryPerformanceCounter.
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
}

static Blizzard::Time::Timestamp WallClock() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (static_cast<int64_t>(now.tv_sec) - kUnixTo2000) * 1000000000 + now.tv_nsec;
}

Blizzard::Time::Timestamp Blizzard::Time::FromFileTime(uint64_t fileTime) {
    if (fileTime <= 0x0077A5D6AA8BF851) {
        return kTimestampMin;
    }

    if (fileTime <= 0x030701FFA04E87AE) {
        return static_cast<Timestamp>(fileTime * 100 + 0x5143382561530000);
    }

    return kTimestampMax;
}

uint64_t System_Time::GetTickCount64() {
    return MonotonicCounter() / 1000000;
}

bool System_Time::CalibrateFrequency(LARGE_INTEGER* frequency) {
    frequency->QuadPart = 1000000000;
    return true;
}

bool System_Time::InitFrequency(LARGE_INTEGER* frequency) {
    return CalibrateFrequency(frequency);
}

double System_Time::MeasureDrift(int64_t frequency, LARGE_INTEGER* counter, DWORD* tickCount) {
    return 0.0;
}

bool System_Time::TimeInit() {
    LARGE_INTEGER frequency;
    s_usePerformanceCounter = InitFrequency(&frequency);
    timeScaleCycles = frequency.QuadPart;
    s_absBegin = static_cast<int64_t>(MonotonicCounter());
    s_gmBegin = WallClock();

    double scale = 1.0 / static_cast<double>(timeScaleCycles);
    timeScaleNanoseconds = 1000000000.0 * scale;
    timeScaleMicroseconds = 1000000.0 * scale;
    timeScaleMilliseconds = 1000.0 * scale;
    timeScaleSeconds = scale;

    return true;
}

void Blizzard::Time::BreakTime(Timestamp time, TimeRec& rec) {
    int64_t seconds = time / 1000000000;
    int32_t nanosecond = static_cast<int32_t>(time % 1000000000);

    if (nanosecond < 0) {
        nanosecond += 1000000000;
        seconds--;
    }

    time_t unixTime = static_cast<time_t>(seconds + kUnixTo2000);
    tm utc;
    gmtime_r(&unixTime, &utc);

    rec.day = utc.tm_mday;
    rec.hour = utc.tm_hour;
    rec.minute = utc.tm_min;
    rec.second = utc.tm_sec;
    rec.nanosecond = nanosecond;
    rec.weekday = utc.tm_wday;
    rec.year = utc.tm_year + 1900;
    rec.month = utc.tm_mon + 1;
    rec.yearday = utc.tm_yday;
}

Blizzard::Time::Timestamp Blizzard::Time::MakeTime(const TimeRec& rec) {
    tm utc = {};
    utc.tm_year = rec.year - 1900;
    utc.tm_mon = rec.month - 1;
    utc.tm_mday = rec.day;
    utc.tm_hour = rec.hour;
    utc.tm_min = rec.minute;
    utc.tm_sec = rec.second;

    int64_t seconds = static_cast<int64_t>(timegm(&utc)) - kUnixTo2000;

    return seconds * 1000000000 + rec.nanosecond;
}

Blizzard::Time::Timestamp Blizzard::Time::GetTimestamp() {
    if (System_Time::timeScaleCycles == 0xFFFFFFFFFFFFFFFF) {
        System_Time::TimeInit();
    }

    uint64_t elapsed = MonotonicCounter() - static_cast<uint64_t>(System_Time::s_absBegin);

    return System_Time::s_gmBegin + static_cast<Timestamp>(elapsed);
}

int64_t Blizzard::Time::Milliseconds() {
    if (System_Time::timeScaleCycles == 0xFFFFFFFFFFFFFFFF) {
        System_Time::TimeInit();
    }

    return static_cast<int64_t>(MonotonicCounter() / 1000000);
}
