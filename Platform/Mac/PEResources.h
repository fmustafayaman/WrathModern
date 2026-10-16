// Reads resources from the original client executable (Wow.exe) in the client directory.
//
// The Windows client embeds data the client reads at runtime (the BLIZZARDCOMPONENT version
// record, for one) as PE resources. The macOS executable has no PE resources, so the macOS build
// reads them from the Wow.exe that every 3.3.5a game directory has.

#ifndef PLATFORM_MAC_PERESOURCES_H
#define PLATFORM_MAC_PERESOURCES_H

#include <cstdint>

#include <vector>

namespace MacPEResources {

// A PE image read into memory, with access to its resource tree.
class Image {
    public:
    // Reads a PE file. Returns false if it cannot be read or has no resource section.
    bool Load(const char* path);

    // Finds a resource by type and name (strings, or MAKEINTRESOURCE integers), in the given
    // language or any language when 0. The pointer stays valid while the image lives.
    const void* Find(const char* type, const char* name, uint16_t language, uint32_t* size) const;

    private:
    uint32_t FindEntry(uint32_t directoryOffset, const char* id, bool any) const;
    bool NameMatches(uint32_t nameOffset, const char* id) const;

    std::vector<uint8_t> m_data;
    uint32_t m_resourceOffset = 0;
    uint32_t m_resourceRva = 0;
};

// Finds a resource in the client's Wow.exe. The pointer stays valid for the life of the process.
const void* Find(const char* type, const char* name, uint16_t language, uint32_t* size);

// LoadStringA for the client's Wow.exe: copies string id of its string table (RT_STRING), as UTF-8.
// Returns the length copied, 0 if there is no such string.
uint32_t LoadString(uint32_t id, char* buffer, uint32_t buffersize);

// FormatMessageA(FORMAT_MESSAGE_FROM_HMODULE) for the client's Wow.exe: copies message id of its
// message table (RT_MESSAGETABLE), as UTF-8. Returns the length copied, 0 if there is none.
uint32_t FormatMessage(uint32_t id, char* buffer, uint32_t buffersize);

// Reads the fixed version record (VS_FIXEDFILEINFO) of a PE file: file version and product version,
// each as most and least significant 32 bits. Returns false if the file has none.
bool ReadFileVersion(const char* path, uint32_t* fileMS, uint32_t* fileLS, uint32_t* productMS, uint32_t* productLS);

}

#endif
