#ifndef ENGINE_SOURCE_OS_MAC_CLIENTIMAGERUNTIME_H
#define ENGINE_SOURCE_OS_MAC_CLIENTIMAGERUNTIME_H

#include <cstdint>
#include <cstring>

// Original 12340 startup state shared by the Warden and buck-data machines. Mapping an exe
// from disk does not run its startup; writable globals must reflect the functions that the
// native client actually ran, not stay at the on-disk zero initialization.
namespace ClientImageRuntime {

constexpr uint32_t TEXT_START_GLOBAL = 0x00D415B8;
constexpr uint32_t TEXT_END_GLOBAL = 0x00D415BC;

// OsFindTextSectionBounds (0x0086B510..0x0086B591): find the exact 8-byte '.text' section
// name, then ImageBase + VirtualAddress .. + VirtualSize. OsValidateFunctionPointer invokes
// this during client startup. Values come only from the supplied PE, never captured replies.
inline bool TextBounds(const uint8_t* image, uint32_t size, uint32_t& start, uint32_t& end) {
    start = end = 0;
    if (!image || size < 0x40) return false;
    auto word = [](const uint8_t* p) {
        uint16_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    };
    auto dword = [](const uint8_t* p) {
        uint32_t value;
        memcpy(&value, p, sizeof(value));
        return value;
    };
    uint32_t nt = dword(image + 0x3C);
    if (nt > size || size - nt < 0x58 || dword(image + nt) != 0x4550
        || word(image + nt + 0x18) != 0x10B) return false;
    uint16_t count = word(image + nt + 6);
    uint16_t optionalSize = word(image + nt + 20);
    uint64_t table = static_cast<uint64_t>(nt) + 0x18 + optionalSize;
    if (table + static_cast<uint64_t>(count) * 40 > size) return false;
    uint32_t base = dword(image + nt + 0x34);

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* section = image + table + i * 40;
        if (memcmp(section, ".text\0\0\0", 8) != 0) continue;
        uint64_t first = static_cast<uint64_t>(base) + dword(section + 12);
        uint64_t last = first + dword(section + 8);
        if (last > UINT32_MAX || last <= first) return false;
        start = static_cast<uint32_t>(first);
        end = static_cast<uint32_t>(last);
        return true;
    }

    return false;
}

} // namespace ClientImageRuntime

#endif
