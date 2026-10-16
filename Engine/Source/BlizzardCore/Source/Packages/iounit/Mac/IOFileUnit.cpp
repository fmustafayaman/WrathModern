#include <cstdint>
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/Storm/H/storm.h"

// Unbuffered reads use F_NOCACHE, the macOS counterpart of FILE_FLAG_NO_BUFFERING, with the same
// 4 KB aligned window the Windows version reads.

bool IOFileUnit::ReadNative(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    char native[1024];
    Blizzard::String::MakeUnivPath(path, native, sizeof(native));

    int32_t fd = open(native, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return false;
    }

    fcntl(fd, F_NOCACHE, 1);

    uint32_t head = offset & 0xFFF;
    uint32_t size = (head + count + 0xFFF) & ~0xFFFu;
    bool result = false;
    uint8_t* data = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    ssize_t read = pread(fd, data, size, static_cast<off_t>(offset - head));

    if (read >= 0) {
        uint32_t available = static_cast<uint32_t>(read) > head ? static_cast<uint32_t>(read) - head : 0;
        uint32_t amount = available >= count ? count : available;
        Blizzard::String::MemCopy(buffer, data + head, amount);

        if (bytesRead) {
            *bytesRead = amount;
        }

        result = true;
    }

    SMemFree(data, __FILE__, __LINE__, 0);
    close(fd);

    return result;
}

IOUnbufferedFileUnit::IOUnbufferedFileUnit(Blizzard::File::StreamRecord* stream, bool ownsMemory) : IOFileUnit(stream, ownsMemory) {
    Blizzard::File::FileInfo* info = Blizzard::File::GetFileInfo(stream);
    this->m_size = info->m_size;

    char native[1024];
    Blizzard::String::MakeUnivPath(info->m_path, native, sizeof(native));

    // The filesystem's I/O size stands in for the sector size Windows aligns to.
    struct statfs fs;

    if (statfs(native, &fs) != 0 || !fs.f_bsize || ((fs.f_bsize - 1) & fs.f_bsize)) {
        this->m_alignMask = 0xFFFF;
    } else {
        this->m_alignMask = static_cast<uint32_t>(fs.f_bsize) - 1;
    }

    this->m_buffer = Blizzard::Memory::Allocate(this->m_alignMask + 0x100000);
    this->m_alignedBuffer = reinterpret_cast<void*>(~static_cast<uintptr_t>(this->m_alignMask) & (reinterpret_cast<uintptr_t>(this->m_buffer) + this->m_alignMask));
}
