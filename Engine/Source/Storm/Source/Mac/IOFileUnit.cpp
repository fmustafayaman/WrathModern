#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "Engine/Source/Storm/Source/IOUnit/IOFileUnit.h"
#include "Engine/Source/Storm/H/storm.h"

// Storm file units on macOS. IOFileParam carries Win32 CreateFile values, which callers pass
// literally; they map onto open(2) here. IOWin32FileUnit keeps its name and holds a POSIX file
// descriptor in m_handle.

namespace Storm {

// Win32 CreateFile values
static const uint32_t kGenericRead = 0x80000000;
static const uint32_t kGenericWrite = 0x40000000;
static const uint32_t kCreateNew = 1;
static const uint32_t kCreateAlways = 2;
static const uint32_t kOpenExisting = 3;
static const uint32_t kOpenAlways = 4;
static const uint32_t kTruncateExisting = 5;
static const uint32_t kFlagNoBuffering = 0x20000000;

static void* ToHandle(int32_t fd) {
    // Descriptor 0 must not read as "no handle", so handles carry fd + 1.
    return reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
}

static int32_t ToDescriptor(void* handle) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(handle) - 1);
}

static void* OpenFileHandle(const IOFileParam* param) {
    int32_t flags;

    if ((param->m_access & kGenericRead) && (param->m_access & kGenericWrite)) {
        flags = O_RDWR;
    } else if (param->m_access & kGenericWrite) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    switch (param->m_creation) {
        case kCreateNew:
            flags |= O_CREAT | O_EXCL;
            break;
        case kCreateAlways:
            flags |= O_CREAT | O_TRUNC;
            break;
        case kOpenAlways:
            flags |= O_CREAT;
            break;
        case kTruncateExisting:
            flags |= O_TRUNC;
            break;
        case kOpenExisting:
        default:
            break;
    }

    char path[1024];
    SStrCopy(path, param->m_name, sizeof(path));

    for (char* c = path; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }

    int32_t fd;

    do {
        fd = open(path, flags | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        return nullptr;
    }

    if (param->m_flags & kFlagNoBuffering) {
        fcntl(fd, F_NOCACHE, 1);
    }

    return ToHandle(fd);
}

IOUnit::IOUnit() {
    this->m_refcount = nullptr;
    this->m_count = 1;
}

void IOUnit::Release() {
    IORefcount* refcount = this->m_refcount;

    if (--this->m_count == 0) {
        delete this;
    }

    if (refcount) {
        refcount->Release();
    }
}

IOWin32FileUnit::IOWin32FileUnit(void* handle) {
    this->m_handle = handle;
    this->m_position = -1;
    this->Seek(0);
}

bool IOWin32FileUnit::Seek(int64_t offset) {
    if (this->m_position != offset) {
        if (lseek(ToDescriptor(this->m_handle), offset, SEEK_SET) < 0) {
            this->m_position = -1;
            return false;
        }

        this->m_position = offset;
    }

    return true;
}

bool IOWin32FileUnit::Read(void* buffer, int64_t offset, uint32_t count, uint32_t* bytesRead) {
    uint32_t minimum = 1;
    uint32_t localRead;

    if (!bytesRead) {
        minimum = count;
        bytesRead = &localRead;
    }

    *bytesRead = 0;

    for (int32_t tries = 0; tries < 3; tries++) {
        if (!this->Seek(offset)) {
            return false;
        }

        ssize_t result = read(ToDescriptor(this->m_handle), buffer, count);

        if (result >= 0) {
            *bytesRead = static_cast<uint32_t>(result);
            this->m_position = offset + result;
            return *bytesRead >= minimum;
        }

        this->m_position = -1;
        usleep(10 * 1000);
    }

    return false;
}

bool IOWin32FileUnit::Write(const void* buffer, int64_t offset, uint32_t count) {
    if (!this->Seek(offset)) {
        return false;
    }

    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    uint32_t written = 0;

    while (written < count) {
        ssize_t result = write(ToDescriptor(this->m_handle), data + written, count - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            this->m_position = -1;
            return false;
        }

        written += static_cast<uint32_t>(result);
    }

    this->m_position = offset + written;

    return true;
}

bool IOWin32FileUnit::Flush() {
    int32_t fd = ToDescriptor(this->m_handle);

    return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
}

bool IOWin32FileUnit::GetFileSize(int64_t* size) {
    struct stat info;

    if (fstat(ToDescriptor(this->m_handle), &info) != 0) {
        *size = 0;
        return false;
    }

    *size = info.st_size;

    return true;
}

bool IOWin32FileUnit::SetFileSize(int64_t size) {
    if (!this->Seek(size)) {
        return false;
    }

    return ftruncate(ToDescriptor(this->m_handle), size) == 0;
}

void* IOWin32FileUnit::Win32Handle() {
    return this->m_handle;
}

bool IOUnbufferedFileUnit::Read(void* buffer, int64_t offset, uint32_t count, uint32_t* bytesRead) {
    if (!(this->m_alignMask & reinterpret_cast<uintptr_t>(buffer)) && !(this->m_alignMask & count)) {
        return this->m_unit->Read(buffer, offset, count, bytesRead);
    }

    if (!this->m_buffer) {
        return false;
    }

    uint32_t total = 0;
    bool result = true;

    while (count) {
        uint32_t chunk = count >= 0x100000 ? 0x100000 : count;
        uint32_t size = chunk;

        if (chunk & this->m_alignMask) {
            size = (this->m_alignMask + chunk) & ~this->m_alignMask;
        }

        uint32_t read;
        result = this->m_unit->Read(this->m_buffer, offset, size, &read);

        if (!result) {
            break;
        }

        read = size < chunk ? size : chunk;
        memcpy(buffer, this->m_buffer, read);
        buffer = static_cast<uint8_t*>(buffer) + read;
        offset += read;
        count -= read;
        total += read;

        if (read < chunk) {
            break;
        }
    }

    if (!bytesRead) {
        return count == 0;
    }

    *bytesRead = total;

    return result;
}

bool IOUnbufferedFileUnit::Write(const void* buffer, int64_t offset, uint32_t count) {
    if (!(this->m_alignMask & reinterpret_cast<uintptr_t>(buffer)) && !(this->m_alignMask & count)) {
        return this->m_unit->Write(buffer, offset, count);
    }

    if (!this->m_buffer) {
        return false;
    }

    bool result = true;

    while (count) {
        uint32_t chunk = count >= 0x100000 ? 0x100000 : count;
        memcpy(this->m_buffer, buffer, chunk);
        buffer = static_cast<const uint8_t*>(buffer) + chunk;

        uint32_t size = chunk;

        if (chunk & this->m_alignMask) {
            size = (this->m_alignMask + chunk) & ~this->m_alignMask;
        }

        result = this->m_unit->Write(this->m_buffer, offset, size);

        if (!result) {
            break;
        }

        offset += chunk;
        count -= chunk;
    }

    if (offset > this->m_size) {
        this->m_size = offset;
    }

    return result;
}

bool IOUnbufferedFileUnit::Flush() {
    return this->m_unit->Flush();
}

bool IOUnbufferedFileUnit::SetFileSize(int64_t size) {
    this->m_size = size;

    return this->m_unit->SetFileSize((size + this->m_alignMask) & ~this->m_alignMask);
}

bool IOUnbufferedFileUnit::GetFileSize(int64_t* size) {
    *size = this->m_size;

    return true;
}

IOUnit::~IOUnit() {
}

IOWin32FileUnit::~IOWin32FileUnit() {
    close(ToDescriptor(this->m_handle));
    this->m_handle = nullptr;
}

IOUnbufferedFileUnit::IOUnbufferedFileUnit(void* handle, const IOFileParam* param) {
    this->m_unit = new IOWin32FileUnit(handle);
    this->m_size = 0;
    this->m_unit->GetFileSize(&this->m_size);
    SStrCopy(this->m_path, param->m_name, sizeof(this->m_path));
    this->m_buffer = SMemAlloc(0x100000, __FILE__, __LINE__, 0);

    struct statfs fs;

    if (fstatfs(ToDescriptor(handle), &fs) != 0 || !fs.f_bsize || ((fs.f_bsize - 1) & fs.f_bsize)) {
        this->m_alignMask = 0xFFFF;
    } else {
        this->m_alignMask = static_cast<uint32_t>(fs.f_bsize) - 1;
    }
}

void* IOUnbufferedFileUnit::Win32Handle() {
    return this->m_unit->Win32Handle();
}

IOUnbufferedFileUnit* IOUnbufferedFileUnit::Open(const IOFileParam* param) {
    void* handle = OpenFileHandle(param);

    if (!handle) {
        return nullptr;
    }

    return new IOUnbufferedFileUnit(handle, param);
}

IOWin32FileUnit* IOWin32FileUnit::Open(const IOFileParam* param) {
    void* handle = OpenFileHandle(param);

    if (!handle) {
        return nullptr;
    }

    return new IOWin32FileUnit(handle);
}

IOUnit* IOFileUnit::Create(const IOFileParam& param, IOUnit* next) {
    if (next) {
        return nullptr;
    }

    if (param.m_flags & kFlagNoBuffering) {
        return IOUnbufferedFileUnit::Open(&param);
    }

    return IOWin32FileUnit::Open(&param);
}

IOUnbufferedFileUnit::~IOUnbufferedFileUnit() {
    SMemFree(this->m_buffer, __FILE__, __LINE__, 0);

    if (this->m_unit) {
        delete this->m_unit;
    }

    // Writes went out in whole aligned blocks; trim the file to its real size.
    IOFileParam param;
    param.m_name = this->m_path;
    param.m_access = kGenericWrite;
    param.m_share = 1;
    param.m_creation = kOpenExisting;
    param.m_flags = 0x80;

    IOUnit* unit = IOWin32FileUnit::Open(&param);

    if (unit) {
        unit->SetFileSize(this->m_size);
        unit->Release();
    }
}

}
