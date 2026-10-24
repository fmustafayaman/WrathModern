#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>
#include "Engine/Source/Os/OsMemory.h"

// Allocations within 1.5 GB of an anchor, which the x64 client needs where it keeps 32-bit offsets
// between blocks (M2 vertex data next to its model). The search follows the Windows version: free
// regions above the anchor first, then below, each candidate allocated at its exact address without
// disturbing existing mappings.

static const uint64_t s_nearRange = 0x60000000;

// The kernel may coalesce mappings, while malloc owns reserved VM ranges that cannot be
// allocated with mach_vm_allocate. Track both the size and the allocator used for each block.
struct NearAllocation {
    uint64_t size;
    bool heap;
};

static std::mutex s_allocationsLock;
static std::unordered_map<uint64_t, NearAllocation> s_allocations;

static bool AllocateAt(mach_vm_address_t address, mach_vm_size_t size) {
    mach_vm_address_t target = address;

    if (mach_vm_allocate(mach_task_self(), &target, size, VM_FLAGS_FIXED) != KERN_SUCCESS || target != address) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_allocationsLock);
    s_allocations[address] = { size, false };

    return true;
}

// The mapped region containing or following address. Returns false past the last region.
static bool NextRegion(mach_vm_address_t address, mach_vm_address_t* base, mach_vm_size_t* size) {
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    *base = address;

    kern_return_t result = mach_vm_region(mach_task_self(), base, size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);

    if (result == KERN_SUCCESS && MACH_PORT_VALID(object)) {
        mach_port_deallocate(mach_task_self(), object);
    }

    return result == KERN_SUCCESS;
}

void* OsAllocNear(const void* anchor, size_t size) {
    const uint64_t granularity = 0x10000;
    const uint64_t addressLimit = std::numeric_limits<uintptr_t>::max();

    if (!anchor || !size || size > addressLimit - (granularity - 1)) {
        return nullptr;
    }

    uint64_t bytes = (size + granularity - 1) & ~(granularity - 1);
    uint64_t center = reinterpret_cast<uintptr_t>(anchor);
    uint64_t minimum = center > 0x10000 + s_nearRange ? center - s_nearRange : 0x10000;
    uint64_t maximum = center > addressLimit - s_nearRange ? addressLimit : center + s_nearRange;

    if (maximum <= minimum || bytes > maximum - minimum) {
        return nullptr;
    }

    // Upward: the gaps between mapped regions above the anchor.
    mach_vm_address_t address = center;

    while (address < maximum) {
        mach_vm_address_t base;
        mach_vm_size_t regionSize;
        bool more = NextRegion(address, &base, &regionSize);
        uint64_t gapEnd = more ? base : maximum;
        if (address > addressLimit - (granularity - 1)) {
            break;
        }

        uint64_t candidate = (address + granularity - 1) & ~(granularity - 1);

        if (gapEnd > candidate && candidate <= maximum && bytes <= gapEnd - candidate && bytes <= maximum - candidate && AllocateAt(candidate, bytes)) {
            return reinterpret_cast<void*>(candidate);
        }

        if (!more) {
            break;
        }

        address = base + regionSize;
    }

    // Downward: walk the regions from the bottom of the window, keeping the highest gap below the
    // anchor that fits.
    uint64_t best = 0;
    address = minimum;

    while (address < center) {
        mach_vm_address_t base;
        mach_vm_size_t regionSize;
        bool more = NextRegion(address, &base, &regionSize);
        uint64_t gapEnd = more ? (base < center ? base : center) : center;

        if (gapEnd > address && gapEnd - address >= bytes) {
            uint64_t candidate = (gapEnd - bytes) & ~(granularity - 1);

            if (candidate >= address && candidate >= minimum) {
                best = candidate;
            }
        }

        if (!more || base >= center) {
            break;
        }

        address = base + regionSize;
    }

    if (best && AllocateAt(best, bytes)) {
        return reinterpret_cast<void*>(best);
    }

    // Modern macOS malloc reserves large address ranges around model data. These appear
    // occupied to Mach even when the heap has free space, so a VM-only search can reject
    // every external .anim file. Try the heap without ever accepting an out-of-range block.
    // calloc preserves the zero-filled, >=16-byte-aligned storage expected by M2 callers.
    void* buffer = std::calloc(1, bytes);
    uint64_t addressValue = reinterpret_cast<uintptr_t>(buffer);

    if (!buffer || addressValue < minimum || addressValue > maximum || bytes > maximum - addressValue) {
        std::free(buffer);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(s_allocationsLock);
        s_allocations[addressValue] = { bytes, true };
    }

    return buffer;
}

void OsFreeNear(void* ptr) {
    if (!ptr) {
        return;
    }

    uint64_t address = reinterpret_cast<uintptr_t>(ptr);
    NearAllocation allocation = {};

    {
        std::lock_guard<std::mutex> lock(s_allocationsLock);
        auto it = s_allocations.find(address);

        if (it == s_allocations.end()) {
            return;
        }

        allocation = it->second;
        s_allocations.erase(it);
    }

    if (allocation.heap) {
        std::free(ptr);
    } else {
        mach_vm_deallocate(mach_task_self(), address, allocation.size);
    }
}
