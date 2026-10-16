#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/Paths.h"
#include "Platform/Mac/PEResources.h"

// Executable checksums for the version proof the client sends at login. On Windows the client hashes
// its loaded images (Wow.exe and DivxDecoder.dll, with relocations undone) and unicows.dll's file. A
// macOS client hashes its own executable, as the Mac clients of the time did; Windows-only modules do
// not exist and contribute nothing.

OsVersionHash::~OsVersionHash() {
    if (this->m_entries) {
        SMemFree(this->m_entries, __FILE__, __LINE__, 0);
    }
}

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

static int32_t HashFile(IOsVersionHashCallback& callback, const char* path) {
    HOSFILE file = OsCreateFile(path, OS_GENERIC_READ, 1, OS_OPEN_EXISTING, OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);

    if (file == OS_INVALID_FILE) {
        return 0;
    }

    int32_t result = 0;
    uint64_t size = OsGetFileSize(file);
    void* buffer = nullptr;

    if (size == static_cast<uint32_t>(size)) {
        buffer = SMemAlloc(static_cast<uint32_t>(size), __FILE__, __LINE__, 0);

        uint32_t bytesRead;

        if (buffer && OsReadFile(file, buffer, static_cast<uint32_t>(size), &bytesRead) && bytesRead == size) {
            callback.Process(buffer, static_cast<uint32_t>(size));
            result = 1;
        }
    }

    if (buffer) {
        SMemFree(buffer, __FILE__, __LINE__, 0);
    }

    OsCloseFile(file);

    return result;
}

int32_t IOsVersionChecksum(IOsVersionHashCallback& callback, const char* path, int32_t loaded) {
    if (!path && !loaded) {
        return 0;
    }

    if (loaded) {
        // A loaded module: the client itself, or a DLL that only exists on Windows.
        if (path) {
            return 0;
        }

        char executable[4096];

        return MacPaths::ExecutablePath(executable, sizeof(executable)) ? HashFile(callback, executable) : 0;
    }

    return HashFile(callback, path);
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
