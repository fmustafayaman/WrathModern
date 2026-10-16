#include <cstdint>
#include <cstring>
#include <Security/SecRandom.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include "Engine/Source/Storm/H/storm.h"

// Random bytes from the system generator, with the Windows version's entropy-hash fallback should it
// ever fail.

struct OsSecureRandomEntropy {
    uint64_t m_monotonic;
    uint64_t m_wallClock;
    int32_t m_processId;
    uint64_t m_threadId;
    int32_t m_counter;
    void* m_heapPointer;
};

static volatile int32_t s_counter;

void IOsSecureRandom(uint8_t* buffer, uint32_t size) {
    OsSecureRandomEntropy entropy;
    entropy.m_monotonic = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    entropy.m_wallClock = clock_gettime_nsec_np(CLOCK_REALTIME);
    entropy.m_processId = getpid();
    pthread_threadid_np(nullptr, &entropy.m_threadId);
    entropy.m_heapPointer = SMemAlloc(1, __FILE__, __LINE__, 0);

    if (entropy.m_heapPointer) {
        SMemFree(entropy.m_heapPointer, __FILE__, __LINE__, 0);
    }

    entropy.m_counter = __atomic_add_fetch(&s_counter, 1, __ATOMIC_SEQ_CST);

    SHA1_CONTEXT context;
    uint8_t digest[20];
    SHA1_Init(&context);
    SHA1_Update(&context, buffer, size);
    SHA1_Update(&context, reinterpret_cast<const uint8_t*>(&entropy), sizeof(entropy));
    SHA1_Final(digest, &context);

    while (true) {
        uint32_t count = size > 10 ? 10 : size;
        memcpy(buffer, digest, count);
        size -= count;
        buffer += count;

        if (!size) {
            break;
        }

        SHA1_Init(&context);
        SHA1_Update(&context, digest, sizeof(digest));
        SHA1_Final(digest, &context);
    }
}

void OsSecureRandom(uint8_t* buffer, uint32_t size) {
    if (SecRandomCopyBytes(kSecRandomDefault, size, buffer) == errSecSuccess) {
        return;
    }

    IOsSecureRandom(buffer, size);
}
