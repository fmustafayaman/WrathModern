#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/Paths.h"
#include "Platform/Mac/PEResources.h"

// Executable checksums for the version proof the client sends at login. On Windows the client hashes
// its loaded images (Wow.exe and DivxDecoder.dll, with relocations undone) and unicows.dll's file.
// A macOS client hashed its own executable, as the Mac clients of the time did.
//
// Servers that verify the version proof (Warmane) compare it against the checksums of the official
// 12340 files, so this build reproduces the Windows proof from the client directory's own files:
// Wow.exe and DivxDecoder.dll are mapped the way the loader would lay them out (headers, sections at
// their RVAs, relocations left at their preferred-base values) and hashed with the same algorithm
// the Windows client runs on its loaded modules. When the Windows files are absent the build falls
// back to hashing its own executable, exactly like the original Mac client.

// VERIFY: 0x00871BE0 manual (Win64/OsVersionHash.cpp, compiles on every platform)
OsVersionHash::OsVersionHash() {
    this->m_entries = static_cast<OsVersionHashEntry*>(SMemAlloc(256 * sizeof(OsVersionHashEntry), __FILE__, __LINE__, 0));
    this->m_count = 0;
    this->m_capacity = 256;
}

// VERIFY: 0x00871520 manual (Win64/OsVersionHash.cpp, compiles on every platform)
OsVersionHash::~OsVersionHash() {
    if (this->m_entries) {
        SMemFree(this->m_entries, __FILE__, __LINE__, 0);
    }
}

// VERIFY: 0x00871540 manual (Win64/OsVersionHash.cpp, compiles on every platform)
bool OsVersionHash::AddEntry(const OsVersionHashEntry& entry) {
    if (!this->m_entries) {
        return false;
    }

    if (this->m_count >= this->m_capacity) {
        this->m_capacity *= 2;
        this->m_entries = static_cast<OsVersionHashEntry*>(SMemReAlloc(this->m_entries, this->m_capacity * sizeof(OsVersionHashEntry), __FILE__, __LINE__, 0));

        if (!this->m_entries) {
            return false;
        }
    }

    this->m_entries[this->m_count] = entry;
    this->m_count++;

    return true;
}

// The PE structures the check reads. The Windows client uses windows.h types on its loaded modules;
// these mirror the same layouts for files read from disk.
namespace {

constexpr uint16_t PE_DOS_MARKER = 0x5A4D; // 'MZ'
constexpr uint32_t PE_NT_MARKER = 0x00004550; // 'PE\0\0'

constexpr uint16_t PE_OPTIONAL_HEADER_32_MAGIC = 0x010B;
constexpr uint16_t PE_OPTIONAL_HEADER_64_MAGIC = 0x020B;

constexpr uint32_t PE_DIRECTORY_IMPORT = 1;
constexpr uint32_t PE_DIRECTORY_BASERELOC = 5;
constexpr uint32_t PE_DIRECTORY_DELAY_IMPORT = 13;

constexpr uint32_t PE_RELOC_ABSOLUTE = 0;
constexpr uint32_t PE_RELOC_HIGH = 1;
constexpr uint32_t PE_RELOC_LOW = 2;
constexpr uint32_t PE_RELOC_HIGHLOW = 3;
constexpr uint32_t PE_RELOC_DIR64 = 10;

constexpr uint32_t PE_SCN_MEM_WRITE = 0x80000000;
constexpr uint32_t PE_SCN_MEM_READ = 0x40000000;
constexpr uint32_t PE_SCN_CNT_INITIALIZED_DATA = 0x00000040;

constexpr uint32_t PE_DOS_HEADER_SIZE = 64;

struct VersionDirectory {
    uint32_t m_address;
    uint32_t m_size;
};

struct VersionSectionHeader {
    char m_name[8];
    uint32_t m_virtualSize;
    uint32_t m_virtualAddress;
    uint32_t m_sizeOfRawData;
    uint32_t m_pointerToRawData;
    uint32_t m_pointerToRelocations;
    uint32_t m_pointerToLinenumbers;
    uint16_t m_numberOfRelocations;
    uint16_t m_numberOfLinenumbers;
    uint32_t m_characteristics;
};

struct VersionBaseRelocation {
    uint32_t m_virtualAddress;
    uint32_t m_sizeOfBlock;
};

struct VersionImportDescriptor {
    uint32_t m_originalFirstThunk;
    uint32_t m_timeDateStamp;
    uint32_t m_forwarderChain;
    uint32_t m_name;
    uint32_t m_firstThunk;
};

static uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// A file mapped the way the loader lays it out: headers, then each section at its RVA with zero
// padding, and relocations NOT applied. That is exactly the loaded image at its preferred base,
// which is the state whose values the Windows client's proof contains after unrelocating.
struct MappedImage {
    std::vector<uint8_t> m_data;
    uint32_t m_sizeOfHeaders = 0;
    uint32_t m_sectionAlignment = 0x1000;
    uint32_t m_fileAlignment = 0x200;
    uint32_t m_numberOfSections = 0;
    uint32_t m_sectionsOffset = 0;
    bool m_is32Bit = true;
    VersionDirectory m_importDirectory = {};
    VersionDirectory m_relocDirectory = {};

    bool Build(const uint8_t* file, uint32_t fileSize);

    const VersionSectionHeader* Sections() const {
        return reinterpret_cast<const VersionSectionHeader*>(m_data.data() + m_sectionsOffset);
    }
};

bool MappedImage::Build(const uint8_t* file, uint32_t fileSize) {
    if (fileSize < 0x40 + 4 + 20 + 92) {
        return false;
    }

    if ((file[0] | (file[1] << 8)) != PE_DOS_MARKER) {
        return false;
    }

    uint32_t ntOffset;
    memcpy(&ntOffset, file + 0x3C, sizeof(ntOffset));

    if (!ntOffset || ntOffset > fileSize || ntOffset + 4 + 20 > fileSize) {
        return false;
    }

    const uint8_t* ntHeaders = file + ntOffset;
    uint32_t signature;
    memcpy(&signature, ntHeaders, sizeof(signature));

    if (signature != PE_NT_MARKER) {
        return false;
    }

    uint16_t numberOfSections;
    uint16_t sizeOfOptionalHeader;
    memcpy(&numberOfSections, ntHeaders + 4 + 2, sizeof(numberOfSections));
    memcpy(&sizeOfOptionalHeader, ntHeaders + 4 + 16, sizeof(sizeOfOptionalHeader));

    if (!numberOfSections || numberOfSections > 96) {
        return false;
    }

    const uint8_t* optionalHeader = ntHeaders + 4 + 20;
    uint32_t numberOfRvaAndSizes;
    uint32_t directoriesOffset;

    if (sizeOfOptionalHeader < 2 + 92) {
        return false;
    }

    uint16_t magic;
    memcpy(&magic, optionalHeader, sizeof(magic));

    if (magic == PE_OPTIONAL_HEADER_32_MAGIC) {
        if (sizeOfOptionalHeader < 96) {
            return false;
        }

        m_is32Bit = true;
        memcpy(&m_sectionAlignment, optionalHeader + 32, sizeof(m_sectionAlignment));
        memcpy(&m_fileAlignment, optionalHeader + 36, sizeof(m_fileAlignment));
        memcpy(&m_sizeOfHeaders, optionalHeader + 60, sizeof(m_sizeOfHeaders));
        memcpy(&numberOfRvaAndSizes, optionalHeader + 92, sizeof(numberOfRvaAndSizes));
        directoriesOffset = 96;
    } else if (magic == PE_OPTIONAL_HEADER_64_MAGIC) {
        if (sizeOfOptionalHeader < 112) {
            return false;
        }

        m_is32Bit = false;
        memcpy(&m_sectionAlignment, optionalHeader + 32, sizeof(m_sectionAlignment));
        memcpy(&m_fileAlignment, optionalHeader + 36, sizeof(m_fileAlignment));
        memcpy(&m_sizeOfHeaders, optionalHeader + 60, sizeof(m_sizeOfHeaders));
        memcpy(&numberOfRvaAndSizes, optionalHeader + 108, sizeof(numberOfRvaAndSizes));
        directoriesOffset = 112;
    } else {
        return false;
    }

    if (!m_sectionAlignment || !m_fileAlignment || numberOfRvaAndSizes <= PE_DIRECTORY_DELAY_IMPORT) {
        return false;
    }

    if (directoriesOffset + numberOfRvaAndSizes * sizeof(VersionDirectory) > sizeOfOptionalHeader
        || ntOffset + 4 + 20 + sizeOfOptionalHeader > fileSize
        || m_sizeOfHeaders > fileSize || m_sizeOfHeaders == 0) {
        return false;
    }

    memcpy(&m_importDirectory, optionalHeader + directoriesOffset + PE_DIRECTORY_IMPORT * sizeof(VersionDirectory),
        sizeof(VersionDirectory));
    memcpy(&m_relocDirectory, optionalHeader + directoriesOffset + PE_DIRECTORY_BASERELOC * sizeof(VersionDirectory),
        sizeof(VersionDirectory));

    m_sectionsOffset = ntOffset + 4 + 20 + sizeOfOptionalHeader;
    uint32_t sectionsSize = numberOfSections * sizeof(VersionSectionHeader);

    if (m_sectionsOffset + sectionsSize > fileSize) {
        return false;
    }

    const uint8_t* sections = file + m_sectionsOffset;

    // Layout, in mapped order: the aligned headers, then every section aligned up to its place.
    uint32_t sizeOfImage = AlignUp(m_sizeOfHeaders, m_sectionAlignment);

    for (uint32_t i = 0; i < numberOfSections; i++) {
        VersionSectionHeader section;
        memcpy(&section, sections + i * sizeof(section), sizeof(section));

        uint32_t span = AlignUp(section.m_virtualSize, m_sectionAlignment);

        if (span && section.m_virtualAddress + span < section.m_virtualAddress) {
            return false;
        }

        if (section.m_virtualAddress + span > sizeOfImage) {
            sizeOfImage = section.m_virtualAddress + span;
        }
    }

    if (sizeOfImage > 0x10000000) { // 256 MiB of mapped image is beyond any real module
        return false;
    }

    m_data.assign(sizeOfImage, 0);
    memcpy(m_data.data(), file, m_sizeOfHeaders);

    for (uint32_t i = 0; i < numberOfSections; i++) {
        const VersionSectionHeader& section = Sections()[i];

        if (section.m_pointerToRawData > fileSize
            || section.m_sizeOfRawData > fileSize - section.m_pointerToRawData) {
            return false;
        }

        uint32_t copySize = section.m_sizeOfRawData < section.m_virtualSize
            ? section.m_sizeOfRawData
            : section.m_virtualSize;

        if (copySize && section.m_virtualAddress + copySize <= sizeOfImage) {
            memcpy(&m_data[section.m_virtualAddress], file + section.m_pointerToRawData, copySize);
        }
    }

    m_numberOfSections = numberOfSections;
    return true;
}

// The entry list mirrors the Windows client: import thunks swapped to their OriginalFirstThunk
// values, import descriptors with the runtime fields zeroed, relocatable words (which carry their
// stored, preferred-base values here), and the checksum header field masked out.
struct VersionHash {
    struct Entry {
        uint32_t m_offset;
        uint32_t m_size;
        uint32_t m_type;
        uint32_t m_param;
    };

    bool AddEntry(const Entry& entry) {
        m_entries.push_back(entry);
        return true;
    }

    std::vector<Entry> m_entries;
};

static int CompareEntries(const void* a, const void* b) {
    const VersionHash::Entry* entryA = static_cast<const VersionHash::Entry*>(a);
    const VersionHash::Entry* entryB = static_cast<const VersionHash::Entry*>(b);

    if (entryA->m_offset < entryB->m_offset) {
        return -1;
    }

    return entryA->m_offset > entryB->m_offset;
}

static void HashZeroes(IOsVersionHashCallback& callback, uint32_t size) {
    uint8_t zeroes[4096];
    memset(zeroes, 0, sizeof(zeroes));

    while (size) {
        uint32_t chunk = size > sizeof(zeroes) ? sizeof(zeroes) : size;
        callback.Process(zeroes, chunk);
        size -= chunk;
    }
}

// VERIFY: 0x00871630 manual (the image sits at its preferred base, so no entries are needed)
static bool AddRelocationEntries(VersionHash& hash, const MappedImage& image) {
    (void)hash;
    (void)image;
    return true;
}

// VERIFY: 0x00871740 manual
static bool AddHeaderEntries(VersionHash& hash, const MappedImage& image) {
    if (image.m_sizeOfHeaders < PE_DOS_HEADER_SIZE) {
        return false;
    }

    // e_csum is masked out of the header hash by zeroing its two bytes.
    VersionHash::Entry entry;
    entry.m_offset = 0x0C; // offsetof(IMAGE_DOS_HEADER, e_csum)
    entry.m_size = 2;
    entry.m_type = 3;
    entry.m_param = 0;

    return hash.AddEntry(entry);
}

// VERIFY: 0x00871C20 manual
static bool AddImportEntries(VersionHash& hash, const MappedImage& image) {
    uint32_t importAddress = image.m_importDirectory.m_address;
    uint32_t importSize = image.m_importDirectory.m_size;

    if (!importAddress) {
        return true;
    }

    if (importSize < sizeof(VersionImportDescriptor)) {
        return true;
    }

    uint32_t imageEnd = static_cast<uint32_t>(image.m_data.size());

    if (importAddress > imageEnd) {
        return true;
    }

    if (importSize > imageEnd - importAddress) {
        importSize = imageEnd - importAddress;
    }

    uint32_t descriptorAddress = importAddress;

    while (descriptorAddress + sizeof(VersionImportDescriptor) <= importAddress + importSize
        && descriptorAddress + sizeof(VersionImportDescriptor) <= imageEnd) {
        VersionImportDescriptor descriptor;
        memcpy(&descriptor, &image.m_data[descriptorAddress], sizeof(descriptor));

        if (!descriptor.m_name || !descriptor.m_firstThunk) {
            break;
        }

        if (descriptor.m_firstThunk >= imageEnd) {
            break;
        }

        uint32_t thunkSize;

        if (image.m_is32Bit) {
            thunkSize = 4;

            while (descriptor.m_firstThunk + thunkSize <= imageEnd) {
                uint32_t thunk;
                memcpy(&thunk, &image.m_data[descriptor.m_firstThunk + thunkSize - 4], sizeof(thunk));

                if (!thunk) {
                    break;
                }

                thunkSize += 4;
            }
        } else {
            thunkSize = 8;

            while (descriptor.m_firstThunk + thunkSize <= imageEnd) {
                uint64_t thunk;
                memcpy(&thunk, &image.m_data[descriptor.m_firstThunk + thunkSize - 8], sizeof(thunk));

                if (!thunk) {
                    break;
                }

                thunkSize += 8;
            }
        }

        VersionHash::Entry thunks;
        thunks.m_offset = descriptor.m_firstThunk;
        thunks.m_size = thunkSize;
        thunks.m_type = 0;
        thunks.m_param = descriptor.m_originalFirstThunk;

        if (!hash.AddEntry(thunks)) {
            return false;
        }

        VersionHash::Entry entry;
        entry.m_offset = descriptorAddress;
        entry.m_size = sizeof(VersionImportDescriptor);
        entry.m_type = 1;
        entry.m_param = 0;

        if (!hash.AddEntry(entry)) {
            return false;
        }

        descriptorAddress += sizeof(VersionImportDescriptor);
    }

    return true;
}

// VERIFY: 0x00871980 manual (Delta is always zero for a file-mapped image, so type 2 entries hash
// their stored values; those are exactly the unrelocated values the Windows client would hash.)
static bool HashEntry(IOsVersionHashCallback& callback, const VersionHash::Entry& entry,
    const MappedImage& image) {
    uint32_t imageEnd = static_cast<uint32_t>(image.m_data.size());

    switch (entry.m_type) {
        case 0: {
            if (entry.m_param) {
                if (entry.m_param >= imageEnd || entry.m_size > imageEnd - entry.m_param) {
                    return false;
                }

                callback.Process(image.m_data.data() + entry.m_param, entry.m_size);
            } else {
                HashZeroes(callback, entry.m_size);
            }

            return true;
        }

        case 1: {
            if (entry.m_offset >= imageEnd || sizeof(VersionImportDescriptor) > imageEnd - entry.m_offset) {
                return false;
            }

            VersionImportDescriptor descriptor;
            memcpy(&descriptor, &image.m_data[entry.m_offset], sizeof(descriptor));
            descriptor.m_timeDateStamp = 0;
            descriptor.m_forwarderChain = 0;
            callback.Process(&descriptor, sizeof(descriptor));

            return true;
        }

        case 2: {
            if (entry.m_offset >= imageEnd || entry.m_size > imageEnd - entry.m_offset) {
                return false;
            }

            callback.Process(image.m_data.data() + entry.m_offset, entry.m_size);
            return true;
        }

        case 3: {
            HashZeroes(callback, entry.m_size);
            return true;
        }

        default: {
            return false;
        }
    }
}

// VERIFY: 0x00871980 manual - HashSection, faithful to the byte stream the Windows client feeds
// its HMAC: the section's real bytes with the special entries spliced in, then the alignment
// padding as zeroes with any entries falling into the padding spliced in as well.
static bool HashSection(IOsVersionHashCallback& callback, VersionHash& hash, const MappedImage& image,
    uint32_t address, uint32_t size, uint32_t characteristics) {
    uint32_t alignment = image.m_sectionAlignment;
    uint32_t padding = AlignUp(size, alignment) - size;

    if (characteristics & PE_SCN_MEM_WRITE) {
        HashZeroes(callback, size + padding);
        return true;
    }

    uint32_t imageEnd = static_cast<uint32_t>(image.m_data.size());
    size_t index = 0;

    while (index < hash.m_entries.size() && hash.m_entries[index].m_offset < address) {
        index++;
    }

    if (size) {
        if (address >= imageEnd || size > imageEnd - address) {
            return false;
        }

        const uint8_t* start = image.m_data.data() + address;
        const uint8_t* data = start;

        do {
            const uint8_t* next = index < hash.m_entries.size()
                ? start + (hash.m_entries[index].m_offset - address)
                : nullptr;
            uint32_t chunk = static_cast<uint32_t>(start - data) + size; // remaining to the end

            if (next && static_cast<uint32_t>(next - data) < chunk) {
                chunk = static_cast<uint32_t>(next - data);
            }

            if (chunk) {
                callback.Process(data, chunk);
                data += chunk;
            } else {
                const VersionHash::Entry& entry = hash.m_entries[index];

                if (!HashEntry(callback, entry, image)) {
                    return false;
                }

                index++;
                data += entry.m_size;
            }
        } while (static_cast<uint32_t>(data - start) < size);
    }

    if (!padding) {
        return true;
    }

    std::vector<uint8_t> zeroes(padding, 0);
    const uint8_t* data = zeroes.data();

    do {
        const uint8_t* next = index < hash.m_entries.size()
            ? zeroes.data() + (hash.m_entries[index].m_offset - size - address)
            : nullptr;
        uint32_t chunk = static_cast<uint32_t>(zeroes.data() - data) + padding;

        if (next && static_cast<uint32_t>(next - data) < chunk) {
            chunk = static_cast<uint32_t>(next - data);
        }

        if (chunk) {
            callback.Process(data, chunk);
            data += chunk;
        } else {
            const VersionHash::Entry& entry = hash.m_entries[index];

            if (!HashEntry(callback, entry, image)) {
                return false;
            }

            index++;
            data += entry.m_size;
        }
    } while (static_cast<uint32_t>(data - zeroes.data()) < padding);

    return true;
}

static bool ComputeChecksum(IOsVersionHashCallback& callback, const MappedImage& image) {
    VersionHash hash;

    if (!AddHeaderEntries(hash, image)) {
        return false;
    }

    if (!AddRelocationEntries(hash, image)) {
        return false;
    }

    if (!AddImportEntries(hash, image)) {
        return false;
    }

    qsort(hash.m_entries.data(), hash.m_entries.size(), sizeof(VersionHash::Entry), CompareEntries);

    // The headers run as one read-only section covering SizeOfHeaders.
    if (!HashSection(callback, hash, image, 0, image.m_sizeOfHeaders,
            PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ)) {
        return false;
    }

    for (uint32_t i = 0; i < image.m_numberOfSections; i++) {
        const VersionSectionHeader& section = image.Sections()[i];

        if (!HashSection(callback, hash, image, section.m_virtualAddress, section.m_virtualSize,
                section.m_characteristics)) {
            return false;
        }
    }

    return true;
}

static bool HashFileBytes(IOsVersionHashCallback& callback, const char* path) {
    HOSFILE file = OsCreateFile(path, OS_GENERIC_READ, 1, OS_OPEN_EXISTING, OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);

    if (file == OS_INVALID_FILE) {
        return false;
    }

    bool result = false;
    uint64_t size = OsGetFileSize(file);

    if (size == static_cast<uint32_t>(size)) {
        void* buffer = SMemAlloc(static_cast<uint32_t>(size), __FILE__, __LINE__, 0);
        uint32_t bytesRead;

        if (buffer && OsReadFile(file, buffer, static_cast<uint32_t>(size), &bytesRead) && bytesRead == size) {
            callback.Process(buffer, static_cast<uint32_t>(size));
            result = true;
        }

        if (buffer) {
            SMemFree(buffer, __FILE__, __LINE__, 0);
        }
    }

    OsCloseFile(file);

    return result;
}

static bool HashLoadedFileImage(IOsVersionHashCallback& callback, const char* path) {
    HOSFILE file = OsCreateFile(path, OS_GENERIC_READ, 1, OS_OPEN_EXISTING, OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);

    if (file == OS_INVALID_FILE) {
        return false;
    }

    bool result = false;
    uint64_t size = OsGetFileSize(file);

    if (size && size == static_cast<uint32_t>(size) && size <= 0x10000000) {
        uint8_t* data = static_cast<uint8_t*>(SMemAlloc(static_cast<uint32_t>(size), __FILE__, __LINE__, 0));
        uint32_t bytesRead;

        if (data && OsReadFile(file, data, static_cast<uint32_t>(size), &bytesRead) && bytesRead == size) {
            MappedImage image;

            if (image.Build(data, bytesRead)) {
                result = ComputeChecksum(callback, image);
            }

            SMemFree(data, __FILE__, __LINE__, 0);
        } else if (data) {
            SMemFree(data, __FILE__, __LINE__, 0);
        }
    }

    OsCloseFile(file);
    return result;
}

} // namespace

bool OsMacMapPeImage(const char* path, std::vector<uint8_t>& image, uint32_t& preferredBase) {
    HOSFILE file = OsCreateFile(path, OS_GENERIC_READ, 1, OS_OPEN_EXISTING, OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);

    if (file == OS_INVALID_FILE) {
        return false;
    }

    bool result = false;
    uint64_t size = OsGetFileSize(file);

    if (size && size == static_cast<uint32_t>(size) && size <= 0x10000000) {
        std::vector<uint8_t> data(static_cast<size_t>(size));
        uint32_t bytesRead;
        MappedImage mapped;

        if (OsReadFile(file, data.data(), static_cast<uint32_t>(size), &bytesRead) && bytesRead == size
            && mapped.Build(data.data(), bytesRead) && mapped.m_is32Bit) {
            // ImageBase sits at optional header + 28 in PE32; Build validated the headers.
            uint32_t ntOffset;
            memcpy(&ntOffset, mapped.m_data.data() + 0x3C, sizeof(ntOffset));
            memcpy(&preferredBase, mapped.m_data.data() + ntOffset + 4 + 20 + 28, sizeof(preferredBase));
            image.swap(mapped.m_data);
            result = true;
        }
    }

    OsCloseFile(file);
    return result;
}

int32_t IOsVersionChecksum(IOsVersionHashCallback& callback, const char* path, int32_t loaded) {
    if (!path && !loaded) {
        return 0;
    }

    if (loaded) {
        bool hashed = false;
        char mapped[4096];

        if (path) {
            // A loaded Windows module (DivxDecoder.dll): hash the client directory's copy the way
            // the loaded image would be hashed. A Windows-only DLL that is missing contributes
            // nothing, matching the Windows client's GetModuleHandle failure path.
            hashed = HashLoadedFileImage(callback, path);
        } else {
            // The client's own image. The original Mac clients hashed their own executable, but a
            // Windows-style client directory carries the original Wow.exe, and this client's proof
            // must match what that directory produces on Windows.
            hashed = MacPaths::FindInClientDirectory("Wow.exe", mapped, sizeof(mapped))
                && HashLoadedFileImage(callback, mapped);

            if (!hashed) {
                hashed = MacPaths::ExecutablePath(mapped, sizeof(mapped))
                    && HashFileBytes(callback, mapped);
            }
        }

        return hashed ? 1 : 0;
    }

    return HashFileBytes(callback, path) ? 1 : 0;
}

OsFileVersionInfo::OsFileVersionInfo(const char* filename) : m_fileVersion(), m_productVersion() {
    // Version records exist for Windows executables (the client's Wow.exe when no file is given).
    char path[4096];

    if (!filename) {
        if (!MacPaths::FindInClientDirectory("Wow.exe", path, sizeof(path))) {
            return;
        }

        filename = path;
    }

    uint32_t fileMS;
    uint32_t fileLS;
    uint32_t productMS;
    uint32_t productLS;

    if (!MacPEResources::ReadFileVersion(filename, &fileMS, &fileLS, &productMS, &productLS)) {
        return;
    }

    m_fileVersion[0] = static_cast<uint16_t>(fileMS >> 16);
    m_fileVersion[1] = static_cast<uint16_t>(fileMS);
    m_fileVersion[2] = static_cast<uint16_t>(fileLS >> 16);
    m_fileVersion[3] = static_cast<uint16_t>(fileLS);
    m_productVersion[0] = static_cast<uint16_t>(productMS >> 16);
    m_productVersion[1] = static_cast<uint16_t>(productMS);
    m_productVersion[2] = static_cast<uint16_t>(productLS >> 16);
    m_productVersion[3] = static_cast<uint16_t>(productLS);
}
