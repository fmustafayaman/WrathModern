#include "Platform/Mac/PEResources.h"
#include "Platform/Mac/Paths.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// A minimal PE resource directory walker: DOS header, NT headers, section table, then the three-level
// resource tree (type, name, language) of IMAGE_DIRECTORY_ENTRY_RESOURCE.

namespace MacPEResources {

template <class T>
static bool ReadAt(const std::vector<uint8_t>& data, size_t offset, T& value) {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }

    memcpy(&value, data.data() + offset, sizeof(T));

    return true;
}

static bool IsIntResource(const char* id) {
    return reinterpret_cast<uintptr_t>(id) <= 0xFFFF;
}

bool Image::Load(const char* path) {
    FILE* file = fopen(path, "rb");

    if (!file) {
        return false;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (length <= 0) {
        fclose(file);
        return false;
    }

    this->m_data.resize(static_cast<size_t>(length));
    bool read = fread(this->m_data.data(), 1, this->m_data.size(), file) == this->m_data.size();
    fclose(file);

    uint32_t peOffset;
    uint32_t signature;
    uint16_t sectionCount;
    uint16_t optionalSize;
    uint16_t magic;

    if (!read || !ReadAt(this->m_data, 0x3C, peOffset) || !ReadAt(this->m_data, peOffset, signature) || signature != 0x00004550 || !ReadAt(this->m_data, peOffset + 6, sectionCount) || !ReadAt(this->m_data, peOffset + 20, optionalSize) || !ReadAt(this->m_data, peOffset + 24, magic)) {
        this->m_data.clear();
        return false;
    }

    // Data directory 2 (resources) sits 96 bytes into a PE32 optional header's data directories and
    // 112 into a PE32+ one, 8 bytes per entry.
    size_t optional = peOffset + 24;
    size_t directory = optional + (magic == 0x20B ? 112 : 96) + 2 * 8;
    uint32_t rva;

    if (!ReadAt(this->m_data, directory, rva) || !rva) {
        this->m_data.clear();
        return false;
    }

    size_t sections = optional + optionalSize;

    for (uint16_t i = 0; i < sectionCount; i++) {
        size_t section = sections + i * 40;
        uint32_t virtualSize;
        uint32_t virtualAddress;
        uint32_t rawSize;
        uint32_t rawOffset;

        if (!ReadAt(this->m_data, section + 8, virtualSize) || !ReadAt(this->m_data, section + 12, virtualAddress) || !ReadAt(this->m_data, section + 16, rawSize) || !ReadAt(this->m_data, section + 20, rawOffset)) {
            break;
        }

        uint32_t extent = virtualSize > rawSize ? virtualSize : rawSize;

        if (rva >= virtualAddress && rva < virtualAddress + extent) {
            this->m_resourceOffset = rawOffset + (rva - virtualAddress);
            this->m_resourceRva = rva;
            return true;
        }
    }

    this->m_data.clear();

    return false;
}

// Compares a resource directory entry name (a length-prefixed UTF-16 string) with an ASCII id,
// ignoring case as the Win32 resource functions do.
bool Image::NameMatches(uint32_t nameOffset, const char* id) const {
    uint16_t length;

    if (!ReadAt(this->m_data, this->m_resourceOffset + nameOffset, length) || strlen(id) != length) {
        return false;
    }

    for (uint16_t i = 0; i < length; i++) {
        uint16_t c;

        if (!ReadAt(this->m_data, this->m_resourceOffset + nameOffset + 2 + i * 2, c) || c > 0x7F || toupper(c) != toupper(static_cast<unsigned char>(id[i]))) {
            return false;
        }
    }

    return true;
}

// Looks up an id in the resource directory at directoryOffset (relative to the resource section) and
// returns the entry's OffsetToData field, or 0 when it is not there. With any set, takes the first
// entry.
uint32_t Image::FindEntry(uint32_t directoryOffset, const char* id, bool any) const {
    uint16_t namedCount;
    uint16_t idCount;

    if (!ReadAt(this->m_data, this->m_resourceOffset + directoryOffset + 12, namedCount) || !ReadAt(this->m_data, this->m_resourceOffset + directoryOffset + 14, idCount)) {
        return 0;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(namedCount) + idCount; i++) {
        size_t entry = this->m_resourceOffset + directoryOffset + 16 + i * 8;
        uint32_t name;
        uint32_t data;

        if (!ReadAt(this->m_data, entry, name) || !ReadAt(this->m_data, entry + 4, data)) {
            return 0;
        }

        if (any) {
            return data;
        }

        bool named = (name & 0x80000000) != 0;

        if (IsIntResource(id)) {
            if (!named && (name & 0xFFFF) == reinterpret_cast<uintptr_t>(id)) {
                return data;
            }
        } else if (named && this->NameMatches(name & 0x7FFFFFFF, id)) {
            return data;
        }
    }

    return 0;
}

const void* Image::Find(const char* type, const char* name, uint16_t language, uint32_t* size) const {
    if (this->m_data.empty()) {
        return nullptr;
    }

    uint32_t typeEntry = this->FindEntry(0, type, false);

    if (!(typeEntry & 0x80000000)) {
        return nullptr;
    }

    uint32_t nameEntry = this->FindEntry(typeEntry & 0x7FFFFFFF, name, false);

    if (!(nameEntry & 0x80000000)) {
        return nullptr;
    }

    uint32_t languageEntry = language ? this->FindEntry(nameEntry & 0x7FFFFFFF, reinterpret_cast<const char*>(static_cast<uintptr_t>(language)), false) : 0;

    if (!languageEntry) {
        languageEntry = this->FindEntry(nameEntry & 0x7FFFFFFF, nullptr, true);
    }

    if (!languageEntry || (languageEntry & 0x80000000)) {
        return nullptr;
    }

    uint32_t dataRva;
    uint32_t dataSize;

    if (!ReadAt(this->m_data, this->m_resourceOffset + languageEntry, dataRva) || !ReadAt(this->m_data, this->m_resourceOffset + languageEntry + 4, dataSize) || dataRva < this->m_resourceRva) {
        return nullptr;
    }

    size_t offset = this->m_resourceOffset + (dataRva - this->m_resourceRva);

    if (offset + dataSize > this->m_data.size()) {
        return nullptr;
    }

    *size = dataSize;

    return this->m_data.data() + offset;
}

static std::once_flag s_clientOnce;
static Image* s_client;

const void* Find(const char* type, const char* name, uint16_t language, uint32_t* size) {
    std::call_once(s_clientOnce, [] {
        char path[4096];
        Image* image = new Image();

        if (MacPaths::FindInClientDirectory("Wow.exe", path, sizeof(path)) && image->Load(path)) {
            s_client = image;
        } else {
            delete image;
        }
    });

    return s_client ? s_client->Find(type, name, language, size) : nullptr;
}

bool ReadFileVersion(const char* path, uint32_t* fileMS, uint32_t* fileLS, uint32_t* productMS, uint32_t* productLS) {
    Image image;

    if (!image.Load(path)) {
        return false;
    }

    // RT_VERSION (16), resource 1. The VS_VERSIONINFO block holds VS_FIXEDFILEINFO right after its
    // key; find it by its signature.
    uint32_t size = 0;
    const uint8_t* data = static_cast<const uint8_t*>(image.Find(reinterpret_cast<const char*>(16), reinterpret_cast<const char*>(1), 0, &size));

    if (!data) {
        return false;
    }

    for (uint32_t offset = 0; offset + 52 <= size; offset += 4) {
        uint32_t signature;
        memcpy(&signature, data + offset, 4);

        if (signature == 0xFEEF04BD) {
            memcpy(fileMS, data + offset + 8, 4);
            memcpy(fileLS, data + offset + 12, 4);
            memcpy(productMS, data + offset + 16, 4);
            memcpy(productLS, data + offset + 20, 4);
            return true;
        }
    }

    return false;
}

}
