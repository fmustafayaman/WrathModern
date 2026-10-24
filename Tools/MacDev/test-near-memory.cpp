// Regression coverage for the production macOS near-memory allocator.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "Engine/Source/Os/OsMemory.h"

#define CHECK(test) do { if (!(test)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
    std::exit(1); \
} } while (0)

#if defined(WRATHMODERN_TEST_NEAR_VM)
// Compile the complete production file against these Mach entry points. A busy
// region models malloc's reserved cage; a gap models successful VM ownership.
static bool s_vmGap;
static std::atomic<unsigned> s_queries, s_portReleases, s_vmReleases;
static uint64_t s_lastVmReleaseSize;
extern "C" kern_return_t TestNearVmRegion(vm_map_read_t, mach_vm_address_t* base,
        mach_vm_size_t* size, vm_region_flavor_t, vm_region_info_t,
        mach_msg_type_number_t*, mach_port_t* object) {
    ++s_queries;
    *object = 42; // An owned send right, not a real port in this simulated backend.
    if (s_vmGap) {
        *base += 0x200000;
        *size = 0x10000;
    } else {
        *base = 0x10000;
        *size = std::numeric_limits<uint64_t>::max() - 0x20000;
    }
    return KERN_SUCCESS;
}
extern "C" kern_return_t TestNearVmAllocate(vm_map_t, mach_vm_address_t*, mach_vm_size_t, int) {
    return s_vmGap ? KERN_SUCCESS : KERN_NO_SPACE;
}
extern "C" kern_return_t TestNearVmDeallocate(vm_map_t, mach_vm_address_t, mach_vm_size_t size) {
    ++s_vmReleases;
    s_lastVmReleaseSize = size;
    return KERN_SUCCESS;
}
extern "C" kern_return_t TestNearPortDeallocate(ipc_space_t, mach_port_name_t) {
    ++s_portReleases;
    return KERN_SUCCESS;
}
#endif

static std::atomic<unsigned> s_successes;
static void CheckAllocation(const void* anchor, size_t size, bool required, bool duplicateRelease = false) {
    void* buffer = OsAllocNear(anchor, size);
    CHECK(!required || buffer);
    if (!buffer) {
        return; // An out-of-range heap candidate must fail closed.
    }
    ++s_successes;
    const auto center = reinterpret_cast<uintptr_t>(anchor);
    const auto address = reinterpret_cast<uintptr_t>(buffer);
    const auto bytes = (size + 0xffff) & ~size_t(0xffff);
    CHECK(address % 16 == 0);
    CHECK(address >= center - 0x60000000);
    CHECK(address <= center + 0x60000000);
    CHECK(bytes <= center + 0x60000000 - address);
    auto* data = static_cast<unsigned char*>(buffer);
    for (size_t i = 0; i < size; ++i) {
        CHECK(data[i] == 0);
        data[i] = static_cast<unsigned char>(i * 31 + 7);
    }
    OsFreeNear(buffer);
    if (duplicateRelease) {
        // Only in serial tests: another thread could otherwise reuse the same address.
        OsFreeNear(buffer);
    }
}

int main() {
    CHECK(!OsAllocNear(nullptr, 1));
    CHECK(!OsAllocNear(reinterpret_cast<void*>(0x10000), 0));
    CHECK(!OsAllocNear(reinterpret_cast<void*>(0x10000), std::numeric_limits<size_t>::max()));
    CHECK(!OsAllocNear(reinterpret_cast<void*>(0x10000), size_t(0xc0000001)));
    OsFreeNear(nullptr);
    int notOwned = 0;
    OsFreeNear(&notOwned);

    void* anchor = std::calloc(1, 0x10000);
    CHECK(anchor);
    // Equal allocation classes also make the forced-heap test deterministic under ASan.
    for (size_t bytes : {size_t(1), size_t(16), size_t(8960), size_t(12160),
                         size_t(12608), size_t(14720), size_t(25248), size_t(65536)}) {
        CheckAllocation(anchor, bytes, true, true);
    }
    puts("PASS near memory: zero-filled aligned heap fallback, all observed animation sizes");

    for (size_t bytes : {size_t(65537), size_t(131072), size_t(1048576)}) {
        CheckAllocation(anchor, bytes, false);
    }
    CHECK(!OsAllocNear(reinterpret_cast<void*>(0x10000), 1));
    puts("PASS near memory: invalid sizes, overflow, out-of-range rejection and ownership");

    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) {
        threads.emplace_back([anchor] {
            for (unsigned j = 0; j < 100; ++j) {
                CheckAllocation(anchor, 12160 + j % 3, true);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::free(anchor);
    puts("PASS near memory: 400 concurrent allocation/free pairs");

#if defined(WRATHMODERN_TEST_NEAR_VM)
    CHECK(s_vmReleases == 0); // Heap blocks must never reach mach_vm_deallocate.
    s_vmGap = true;
    // Simulated VM addresses are deliberately not dereferenced: this case tests
    // the registry and the release size, including coalesced-region independence.
    void* vm = OsAllocNear(reinterpret_cast<void*>(0x100000000), 1000);
    CHECK(vm);
    OsFreeNear(vm);
    OsFreeNear(vm);
    CHECK(s_vmReleases == 1);
    CHECK(s_lastVmReleaseSize == 0x10000);
    CHECK(s_queries == s_portReleases);
    printf("PASS near memory: VM/heap release dispatch, %u region send rights released\n",
           s_portReleases.load());
#else
    // The real model allocation that reproduces the original failure on modern
    // macOS, without any game, credentials, graphics context or network traffic.
    void* modelData = std::malloc(1613264 + 32);
    CHECK(modelData);
    for (size_t bytes : {size_t(8960), size_t(12608), size_t(14720), size_t(25248)}) {
        CheckAllocation(modelData, bytes, true);
    }
    std::free(modelData);
    puts("PASS near memory: real Mach VM + 1.6 MiB model allocation");
#endif
    printf("PASS near memory: %u checked allocations\n", s_successes.load());
}
