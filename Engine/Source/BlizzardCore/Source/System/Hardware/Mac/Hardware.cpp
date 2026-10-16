#include <cstdint>
#include <cstring>
#include <sys/sysctl.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

Blizzard::Hardware::ProcessorFeatures System_Hardware::s_processorFeatures;
bool System_Hardware::s_initialized;
Blizzard::Lock::Atomic System_Hardware::s_initCount;

// Feature bits the Windows build derives from CPUID.
static const uint32_t kFeatureTimeStampCounter = 0x1;
static const uint32_t kFeatureMMX = 0x2;
static const uint32_t kFeatureSSE = 0x4;
static const uint32_t kFeatureSSE2 = 0x10;
static const uint32_t kFeatureMultiCore = 0x40;

static uint32_t SysctlInt(const char* name, uint32_t fallback) {
    int32_t value = 0;
    size_t size = sizeof(value);

    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value > 0 ? static_cast<uint32_t>(value) : fallback;
}

bool System_Hardware::ReadCPUID(CPUIDInfo* info) {
    // There is no CPUID on arm64.
    memset(info, 0, sizeof(*info));

    return false;
}

void System_Hardware::InitProcessorFeatures() {
    s_processorFeatures.m_processorCount = SysctlInt("hw.logicalcpu", 1);
    s_processorFeatures.m_coreCount = SysctlInt("hw.physicalcpu", 1);

    // Unknown vendor. The SSE paths of the client run through sse2neon on arm64, and the cycle
    // counter is the system counter, so report what an SSE2 processor reports.
    s_processorFeatures.m_vendor = 0;
    uint32_t features = kFeatureTimeStampCounter | kFeatureMMX | kFeatureSSE | kFeatureSSE2;

    if (s_processorFeatures.m_coreCount > 1) {
        features |= kFeatureMultiCore;
    }

    s_processorFeatures.m_features = features;
}

uint32_t Blizzard::Hardware::GetProcessorFeatures() {
    if (!System_Hardware::s_initialized) {
        if (System_Hardware::s_initCount.Increment() == 1) {
            System_Hardware::InitProcessorFeatures();
            System_Hardware::s_initialized = true;
            return System_Hardware::s_processorFeatures.m_features;
        }

        do {
            usleep(1000);
        } while (!System_Hardware::s_initialized);
    }

    return System_Hardware::s_processorFeatures.m_features;
}
