#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/TempBuffer.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/Storm/H/storm.h"

// The base filesystem of the Blizzard::File stack on macOS, mirroring the Win32 one function for
// function. StreamRecord::m_fileHandle holds a POSIX file descriptor (kInvalidFile when closed).
// Paths reach this layer in the client's universal form; Windows turns them into backslash paths,
// here they become slash paths. Text is UTF-8 on both sides, so native names need no conversion.

System_File::IOFunc System_File::read_func = System_File::read_first;
System_File::IOFunc System_File::write_func = System_File::write_first;
bool System_File::s_read_init;
Blizzard::Lock::Atomic System_File::s_readInitCount;
bool System_File::s_write_init;
Blizzard::Lock::Atomic System_File::s_writeInitCount;
Blizzard::Lock::Mutex System_File::s_ioMutex;
Blizzard::Lock::Mutex* System_File::s_ioLock;

// Blizzard::File open mode bits (see ToCreateFlags)
static const uint32_t kModeRead = 0x1;
static const uint32_t kModeWrite = 0x2;
static const uint32_t kModeNoCache = 0x40;
static const uint32_t kModeTruncate = 0x100;
static const uint32_t kModeAppend = 0x200;
static const uint32_t kModeCreate = 0x400;
static const uint32_t kModeMustNotExist = 0x800;
static const uint32_t kModeMustExist = 0x1000;

// Blizzard::File attribute bits
static const int32_t kAttrReadOnly = 0x1;
static const int32_t kAttrHidden = 0x2;
static const int32_t kAttrNormal = 0x20;
static const int32_t kAttrDirectory = 0x40;

static const intptr_t kInvalidFile = -1;

static int32_t FileDescriptor(const Blizzard::File::StreamRecord* stream) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(stream->m_fileHandle));
}

static void SetFileDescriptor(Blizzard::File::StreamRecord* stream, int32_t fd) {
    stream->m_fileHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}

static bool IsOpen(const Blizzard::File::StreamRecord* stream) {
    return reinterpret_cast<intptr_t>(stream->m_fileHandle) != kInvalidFile;
}

static void NativePath(const char* path, char* buffer, int32_t buffersize) {
    Blizzard::String::MakeUnivPath(path, buffer, buffersize);
}

static Blizzard::Time::Timestamp ToTimestamp(const timespec& time) {
    // Unix epoch to 2000-01-01
    return (static_cast<int64_t>(time.tv_sec) - 946684800) * 1000000000 + time.tv_nsec;
}

static timespec FromTimestamp(Blizzard::Time::Timestamp time) {
    int64_t seconds = time / 1000000000;
    int64_t nanoseconds = time % 1000000000;

    if (nanoseconds < 0) {
        nanoseconds += 1000000000;
        seconds--;
    }

    timespec result;
    result.tv_sec = static_cast<time_t>(seconds + 946684800);
    result.tv_nsec = static_cast<long>(nanoseconds);

    return result;
}

// Blizzard::File attributes for a file, from its stat data and name. Hidden follows the Unix
// convention of a leading dot as well as the Finder hidden flag.
static int32_t AttributesFromStat(const struct stat& info, const char* path) {
    int32_t attributes = 0;

    if (!(info.st_mode & S_IWUSR)) {
        attributes |= kAttrReadOnly;
    }

    const char* name = path ? Blizzard::String::FindFilename(path) : nullptr;

    if ((info.st_flags & UF_HIDDEN) || (name && name[0] == '.' && name[1] && strcmp(name, ".."))) {
        attributes |= kAttrHidden;
    }

    if (S_ISDIR(info.st_mode)) {
        attributes |= kAttrDirectory;
    } else {
        attributes |= kAttrNormal;
    }

    return attributes;
}

static void FillInfo(Blizzard::File::FileInfo* fileInfo, const struct stat& info, const char* path, bool setSize) {
    if (setSize) {
        fileInfo->m_size = info.st_size;
    }

    fileInfo->m_createTime = ToTimestamp(info.st_birthtimespec);
    fileInfo->m_modTime = ToTimestamp(info.st_mtimespec);
    fileInfo->m_accessTime = ToTimestamp(info.st_atimespec);
    fileInfo->m_attributes = AttributesFromStat(info, path);
    fileInfo->m_regularFile = (fileInfo->m_attributes & kAttrNormal) != 0;
    fileInfo->m_existence = ((fileInfo->m_attributes & kAttrDirectory) != 0) + 1;
}

int32_t System_File::ToNativeAttributes(int32_t attributes) {
    // Only read-only maps onto a permission; the remaining bits have no POSIX counterpart.
    return attributes & kAttrReadOnly;
}

int32_t System_File::FromNativeAttributes(int32_t attributes) {
    return attributes;
}

bool System_File::ReadP(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<uint32_t>(parms->m_pos);
    overlapped.OffsetHigh = static_cast<uint32_t>(parms->m_pos >> 32);

    return read_func(parms->m_stream, parms->m_buffer, &overlapped, &parms->m_amount);
}

bool System_File::WriteP(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<uint32_t>(parms->m_pos);
    overlapped.OffsetHigh = static_cast<uint32_t>(parms->m_pos >> 32);

    return write_func(parms->m_stream, parms->m_buffer, &overlapped, &parms->m_amount);
}

bool System_File::Close(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (IsOpen(stream)) {
        close(FileDescriptor(stream));
    }

    SMemFree(stream, __FILE__, __LINE__, 0);

    return true;
}

bool System_File::GetRootChars(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    // The root of a macOS path is its leading slash.
    parms->m_pos = (path && path[0] == '/') ? 1 : 0;

    return true;
}

void System_File::GetFileInfoByFile(Blizzard::File::StreamRecord* file) {
    if (file->m_infoValid) {
        return;
    }

    file->m_infoValid = false;
    struct stat info;

    if (fstat(FileDescriptor(file), &info) != 0) {
        return;
    }

    bool setSize = !file->m_streaming || !file->m_streaming->m_entry;
    FillInfo(&file->m_info, info, file->m_pathname, setSize);
    file->m_infoValid = true;
}

bool System_File::Exists(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char path[1024];
    NativePath(parms->m_path, path, sizeof(path));

    struct stat info;

    if (stat(path, &info) != 0) {
        parms->m_info->m_existence = 0;
    } else if (S_ISDIR(info.st_mode)) {
        parms->m_info->m_existence = 2;
    } else {
        parms->m_info->m_existence = 1;
    }

    return true;
}

// Absolute form of a path without requiring it to exist, like _fullpath: relative paths are taken
// from the working directory, then "." and ".." components are resolved lexically.
static void FullPath(const char* path, char* buffer, size_t buffersize) {
    char combined[2048];

    if (path[0] == '/') {
        Blizzard::String::Copy(combined, path, sizeof(combined));
    } else {
        char cwd[1024];

        if (!getcwd(cwd, sizeof(cwd))) {
            cwd[0] = '\0';
        }

        Blizzard::String::Format(combined, sizeof(combined), "%s/%s", cwd, path);
    }

    const char* components[256];
    size_t lengths[256];
    int32_t count = 0;

    for (const char* c = combined; *c;) {
        while (*c == '/') {
            c++;
        }

        const char* start = c;

        while (*c && *c != '/') {
            c++;
        }

        size_t length = static_cast<size_t>(c - start);

        if (!length || (length == 1 && start[0] == '.')) {
            continue;
        }

        if (length == 2 && start[0] == '.' && start[1] == '.') {
            if (count > 0) {
                count--;
            }

            continue;
        }

        if (count < 256) {
            components[count] = start;
            lengths[count] = length;
            count++;
        }
    }

    size_t used = 0;

    for (int32_t i = 0; i < count && used + lengths[i] + 2 < buffersize; i++) {
        buffer[used++] = '/';
        memcpy(buffer + used, components[i], lengths[i]);
        used += lengths[i];
    }

    if (!used) {
        buffer[used++] = '/';
    }

    buffer[used] = '\0';
}

bool System_File::MakeAbsolutePath(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char path[1024];
    NativePath(parms->m_path, path, sizeof(path));

    char buffer[1024];
    FullPath(path, buffer, sizeof(buffer));

    FromNativeName(parms->m_cwdbuf, parms->m_cwdbuflen, buffer);
    Blizzard::String::MakeUnivPath(parms->m_cwdbuf, parms->m_cwdbuf, parms->m_cwdbuflen);

    const char* original = parms->m_path;
    size_t length = original ? strlen(original) : 0;

    if (length && (original[length - 1] == '/' || original[length - 1] == '\\')) {
        Blizzard::String::ForceTrailingSeparator(parms->m_cwdbuf, parms->m_cwdbuflen, '/');
    }

    return true;
}

bool System_File::Move(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char src[1024];
    char dst[1024];
    NativePath(parms->m_path, src, sizeof(src));
    NativePath(parms->m_newpath, dst, sizeof(dst));

    // MoveFileA fails when the destination exists; rename() would replace it.
    struct stat info;

    if (lstat(dst, &info) == 0) {
        return false;
    }

    return rename(src, dst) == 0;
}

bool System_File::Delete(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char path[1024];
    NativePath(parms->m_path, path, sizeof(path));

    return unlink(path) == 0;
}

bool System_File::read_overlapped(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!IsOpen(file)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (!*count) {
        return true;
    }

    int32_t fd = FileDescriptor(file);
    ssize_t result;

    if (overlapped) {
        // ReadFile with an OVERLAPPED offset on a synchronous handle also leaves the file pointer
        // after the bytes it read.
        off_t offset = (static_cast<off_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset;

        do {
            result = pread(fd, buffer, static_cast<size_t>(*count), offset);
        } while (result < 0 && errno == EINTR);

        if (result >= 0) {
            lseek(fd, offset + result, SEEK_SET);
        }
    } else {
        do {
            result = read(fd, buffer, static_cast<size_t>(*count));
        } while (result < 0 && errno == EINTR);
    }

    if (result >= 0) {
        *count = static_cast<int32_t>(result);
        return true;
    }

    *count = 0;
    Blizzard::File::SetLastError(5);
    Blizzard::File::AddToLastErrorStack(5, Blizzard::String::QuickFormat<1024>("POSIX Read - %s", file->m_pathname).m_buffer, 1);

    return false;
}

bool System_File::write_overlapped(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!IsOpen(file)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (!*count) {
        return true;
    }

    file->m_infoValid = false;

    int32_t fd = FileDescriptor(file);
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    size_t remaining = static_cast<size_t>(*count);
    size_t written = 0;
    off_t offset = overlapped ? ((static_cast<off_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset) : 0;

    // WriteFile on a synchronous handle completes the whole request unless an error occurs.
    while (remaining) {
        ssize_t result = overlapped ? pwrite(fd, data + written, remaining, offset + static_cast<off_t>(written)) : write(fd, data + written, remaining);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            *count = static_cast<int32_t>(written);
            Blizzard::File::SetLastError(6);
            Blizzard::File::AddToLastErrorStack(6, Blizzard::String::QuickFormat<1024>("POSIX Write - %s", file->m_pathname).m_buffer, 1);

            return false;
        }

        written += static_cast<size_t>(result);
        remaining -= static_cast<size_t>(result);
    }

    if (overlapped) {
        lseek(fd, offset + static_cast<off_t>(written), SEEK_SET);
    }

    *count = static_cast<int32_t>(written);

    return true;
}

// The Win32 build turns Blizzard::File modes into CreateFileA parameters here. The macOS build
// derives open(2) flags from the same bits instead (OpenFlags below); this keeps the Win32 values
// for callers that inspect them.
bool System_File::ToCreateFlags(uint32_t flags, DWORD* sharemode, DWORD* desiredaccess, DWORD* creationdisposition, DWORD* flagsandattributes) {
    *sharemode = (flags >> 2) & 3;
    *desiredaccess = ((flags * 4) | (flags & 2)) << 29;

    if ((flags & kModeTruncate) || (flags & kModeCreate)) {
        *creationdisposition = (flags & kModeMustNotExist) ? 1 : 2;
    } else if (flags & kModeMustNotExist) {
        *creationdisposition = 1;
    } else {
        *creationdisposition = 4 - ((flags & kModeMustExist) != 0);
    }

    *flagsandattributes = (2 * ((flags & 0x80) | 0x40)) | ((flags & kModeNoCache) ? 0xA0000000 : 0);

    return true;
}

// open(2) flags for a Blizzard::File mode, following the Win32 creation dispositions: truncate or
// create replaces the file (CREATE_ALWAYS) unless it must not exist (CREATE_NEW); "must not exist"
// alone is CREATE_NEW; otherwise the file is opened, and created when missing (OPEN_ALWAYS) unless
// it must exist (OPEN_EXISTING).
static int32_t OpenFlags(uint32_t mode) {
    int32_t flags;

    if ((mode & kModeRead) && (mode & kModeWrite)) {
        flags = O_RDWR;
    } else if (mode & kModeWrite) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    if ((mode & kModeTruncate) || (mode & kModeCreate)) {
        flags |= (mode & kModeMustNotExist) ? (O_CREAT | O_EXCL) : (O_CREAT | O_TRUNC);
    } else if (mode & kModeMustNotExist) {
        flags |= O_CREAT | O_EXCL;
    } else if (!(mode & kModeMustExist)) {
        flags |= O_CREAT;
    }

    return flags | O_CLOEXEC;
}

static int32_t OpenNative(const char* path, uint32_t mode) {
    int32_t fd;

    do {
        fd = open(path, OpenFlags(mode), 0644);
    } while (fd < 0 && errno == EINTR);

    if (fd >= 0 && (mode & kModeNoCache)) {
        fcntl(fd, F_NOCACHE, 1);
    }

    return fd;
}

bool System_File::SetWorkingDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!parms->m_path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    char path[1024];
    NativePath(parms->m_path, path, sizeof(path));

    return chdir(path) == 0;
}

bool System_File::Create(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::SetLastError(9);

    return false;
}

bool System_File::GetWorkingDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!parms->m_cwdbuf || parms->m_cwdbuflen <= 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    *parms->m_cwdbuf = '\0';
    char buffer[1024];

    if (!getcwd(buffer, sizeof(buffer))) {
        buffer[0] = '\0';
    }

    FromNativeName(parms->m_cwdbuf, parms->m_cwdbuflen, buffer);

    return true;
}

bool System_File::ProcessDirFast(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    void* cookie = parms->m_cookie;
    const char* path = parms->m_path;
    Blizzard::File::ProcessDirCallback callback = parms->m_dircallback;

    char native[1024];
    NativePath(path, native, sizeof(native));

    DIR* dir = opendir(native);

    if (!dir) {
        Blizzard::Debug::Printf(Blizzard::Debug::StderrOutput, "failed to open %s\n", path);
        Blizzard::File::SetLastError(8);
        return false;
    }

    Blizzard::File::ProcessDirParms dirParms;
    dirParms.m_pathRoot = path;
    dirParms.m_cookie = cookie;

    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;

        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) {
            continue;
        }

        bool isDir;

        if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
            char full[2048];
            Blizzard::String::Format(full, sizeof(full), "%s/%s", native, name);
            struct stat info;
            isDir = stat(full, &info) == 0 && S_ISDIR(info.st_mode);
        } else {
            isDir = entry->d_type == DT_DIR;
        }

        char filename[1024];
        FromNativeName(filename, sizeof(filename), name);
        dirParms.m_filename = filename;
        dirParms.m_isDir = isDir;

        if (!callback(dirParms)) {
            closedir(dir);
            return false;
        }
    }

    closedir(dir);

    return true;
}

bool System_File::Flush(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!IsOpen(parms->m_stream)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    // FlushFileBuffers reaches the disk; on macOS that takes F_FULLFSYNC, with fsync as fallback
    // for filesystems that do not support it.
    int32_t fd = FileDescriptor(parms->m_stream);

    return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
}

bool System_File::GetFileInfo(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (parms->m_path) {
        char path[1024];
        NativePath(parms->m_path, path, sizeof(path));

        struct stat info;

        if (stat(path, &info) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        FillInfo(parms->m_info, info, path, true);
        parms->m_info->m_path = nullptr;

        return true;
    }

    if (parms->m_stream) {
        GetFileInfoByFile(parms->m_stream);
        Blizzard::File::StreamRecord* stream = parms->m_stream;
        parms->m_info = &stream->m_info;

        if (!stream->m_info.m_path) {
            stream->m_info.m_path = stream->m_pathname;
        }

        return parms->m_stream->m_infoValid;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::GetFreeSpace(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (path && *path) {
        char native[1024];
        NativePath(path, native, sizeof(native));

        char directory[1024];
        Blizzard::String::Copy(directory, native, std::min<int32_t>(static_cast<int32_t>(Blizzard::String::FindFilename(native) - native + 1), sizeof(directory)));

        if (!directory[0]) {
            Blizzard::String::Copy(directory, ".", sizeof(directory));
        }

        struct statfs info;

        if (statfs(directory, &info) == 0) {
            parms->m_pos = static_cast<int64_t>(info.f_bavail) * static_cast<int64_t>(info.f_bsize);
            return true;
        }
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::GetPos(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    off_t pos;

    if (!IsOpen(stream) || (pos = lseek(FileDescriptor(stream), 0, SEEK_CUR)) < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    parms->m_pos = pos;

    return true;
}

bool System_File::IsAbsolutePath(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (path && (path[0] == '/' || path[0] == '\\') && path[1]) {
        return true;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::IsReadOnly(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    char path[1024];
    NativePath(parms->m_path, path, sizeof(path));

    struct stat info;

    if (stat(path, &info) == 0) {
        return !(info.st_mode & S_IWUSR);
    }

    Blizzard::File::SetLastError(8);

    return false;
}

static bool MakeDirectory(const char* path, bool grantEveryone) {
    if (mkdir(path, grantEveryone ? 0777 : 0755) == 0) {
        if (grantEveryone) {
            chmod(path, 0777);
        }

        return true;
    }

    // Like the Win32 version, a directory that already exists counts as success.
    struct stat info;

    return (errno == EEXIST || errno == EACCES || errno == EISDIR) && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool System_File::CreateDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    bool recursive = parms->m_recursive;
    const char* path = parms->m_path;
    bool grantEveryone = parms->m_grantEveryone;

    if (!path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    Blizzard::Memory::TempBuffer<char, 1024> buffer(static_cast<uint32_t>(strlen(path)) + 1);
    NativePath(path, buffer.m_data, buffer.m_count);

    // Without a trailing separator, the last component is a directory to create too.
    size_t length = strlen(buffer.m_data);

    while (length > 1 && buffer.m_data[length - 1] == '/') {
        buffer.m_data[--length] = '\0';
    }

    if (!length) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (recursive) {
        for (char* c = buffer.m_data + 1; *c; c++) {
            if (*c != '/') {
                continue;
            }

            *c = '\0';
            bool created = MakeDirectory(buffer.m_data, grantEveryone);
            *c = '/';

            if (!created) {
                Blizzard::File::SetLastError(8);
                return false;
            }
        }
    }

    if (MakeDirectory(buffer.m_data, grantEveryone)) {
        return true;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::Copy(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    bool overwrite = parms->m_overwrite;
    char src[1024];
    char dst[1024];
    NativePath(parms->m_path, src, sizeof(src));
    NativePath(parms->m_newpath, dst, sizeof(dst));

    Blizzard::File::StreamRecord* input;

    if (!Blizzard::File::Open(src, 0x1005, input)) {
        Blizzard::File::SetLastError(2);
        return false;
    }

    Blizzard::File::StreamRecord* output;

    if (!Blizzard::File::Open(dst, (overwrite ? 0x400 : 0xC00) | 0x2, output)) {
        Blizzard::File::Close(input);
        Blizzard::File::SetLastError(4);
        return false;
    }

    int64_t size = Blizzard::File::GetFileInfo(input)->m_size;
    int64_t offset = 0;
    bool result = true;
    void* buffer = Blizzard::Memory::Allocate(size >= 0xA00000 ? 0xA00000 : static_cast<uint32_t>(size));

    while (offset < size) {
        int32_t count = size - offset >= 0xA00000 ? 0xA00000 : static_cast<int32_t>(size - offset);

        if (!Blizzard::File::Read(input, buffer, offset, &count) || !Blizzard::File::Write(output, buffer, offset, &count)) {
            result = false;
            break;
        }

        offset += count;
    }

    Blizzard::Memory::Free(buffer);
    Blizzard::File::Close(input);
    Blizzard::File::Close(output);

    if (!result) {
        Blizzard::File::Delete(dst);
    }

    return result;
}

bool System_File::Open(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int32_t mode = parms->m_mode;
    char path[1024];
    NativePath(parms->m_path, path, sizeof(path));

    int32_t fd = OpenNative(path, static_cast<uint32_t>(mode));

    if (fd < 0) {
        Blizzard::File::SetLastError(4);
        Blizzard::File::AddToLastErrorStack(4, Blizzard::String::QuickFormat<1024>("POSIX Open - %s", path).m_buffer, 1);
        return false;
    }

    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (!stream) {
        uint32_t length = static_cast<uint32_t>(strlen(path));
        stream = static_cast<Blizzard::File::StreamRecord*>(SMemAlloc(sizeof(Blizzard::File::StreamRecord) + length + 1, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
        stream->m_pathname = reinterpret_cast<char*>(stream + 1);
        Blizzard::String::Copy(stream->m_pathname, path, length + 1);
    }

    SetFileDescriptor(stream, fd);
    stream->m_mode = mode;
    stream->m_infoValid = false;
    stream->m_info.m_path = stream->m_pathname;
    GetFileInfoByFile(stream);

    if (mode & kModeAppend) {
        Blizzard::File::SetPos(stream, stream->m_info.m_size, 0);
    }

    parms->m_stream = stream;

    return true;
}

bool System_File::Read(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    return read_overlapped(parms->m_stream, parms->m_buffer, nullptr, &parms->m_amount);
}

bool System_File::RemoveDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (parms->m_recursive) {
        return Blizzard::File::RemoveDirectoryAndContents(path, false);
    }

    if (!path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    char native[1024];
    NativePath(path, native, sizeof(native));

    return rmdir(native) == 0;
}

bool System_File::SetCacheMode(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    stream->m_mode = parms->m_mode | (stream->m_mode & ~kModeNoCache);

    // Windows reopens the file to change FILE_FLAG_NO_BUFFERING; F_NOCACHE switches it in place.
    if (!IsOpen(stream) || fcntl(FileDescriptor(stream), F_NOCACHE, (stream->m_mode & kModeNoCache) ? 1 : 0) != 0) {
        Blizzard::File::SetLastError(4);
        Blizzard::File::AddToLastErrorStack(4, "POSIX SetCacheMode", 1);
        return false;
    }

    return true;
}

bool System_File::SetEOF(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int64_t pos = parms->m_pos;
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    int32_t whence = parms->m_posMode;
    int64_t offset;

    if (!Blizzard::File::GetPos(stream, &offset) || !Blizzard::File::SetPos(stream, pos, whence)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    off_t end = lseek(FileDescriptor(stream), 0, SEEK_CUR);
    bool result = end >= 0 && ftruncate(FileDescriptor(stream), end) == 0;
    stream->m_infoValid = false;
    Blizzard::File::SetPos(stream, offset, 0);

    return result;
}

bool System_File::SetAttributes(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (parms->m_has.m_all & 0x10) {
        Blizzard::Time::Timestamp time = parms->m_info->m_modTime;
        timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
        times[1] = FromTimestamp(time);

        if (futimens(FileDescriptor(parms->m_stream), times) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_stream->m_info.m_modTime = time;
        parms->m_has.m_all &= ~0x10u;
    }

    if (parms->m_has.m_all & 0x4) {
        char path[1024];
        NativePath(parms->m_path, path, sizeof(path));

        struct stat info;

        if (stat(path, &info) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        mode_t permissions = info.st_mode & 07777;

        if (ToNativeAttributes(parms->m_info->m_attributes) & kAttrReadOnly) {
            permissions &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
        } else {
            permissions |= S_IWUSR;
        }

        if (chmod(path, permissions) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_has.m_all &= ~0x4u;
    }

    return parms->m_has.m_all == 0;
}

bool System_File::SetPos(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (!IsOpen(stream)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    // Blizzard::File position modes: 0 from the start, 1 from the current position, 2 from the end.
    int32_t whence;

    switch (parms->m_posMode) {
        case 0:
            whence = SEEK_SET;
            break;
        case 1:
            whence = SEEK_CUR;
            break;
        default:
            whence = SEEK_END;
            break;
    }

    return lseek(FileDescriptor(stream), parms->m_pos, whence) >= 0;
}

bool System_File::Write(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    return write_overlapped(parms->m_stream, parms->m_buffer, nullptr, &parms->m_amount);
}

bool System_File::read_locked(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    Blizzard::Lock::ScopedMutex lock(*s_ioLock);

    return read_overlapped(file, buffer, overlapped, count);
}

bool System_File::write_locked(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    Blizzard::Lock::ScopedMutex lock(*s_ioLock);

    return write_overlapped(file, buffer, overlapped, count);
}

void System_File::detect_io_mode() {
    // Positioned reads and writes are atomic on every macOS filesystem.
    read_func = read_overlapped;
    write_func = write_overlapped;
}

bool System_File::read_first(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!s_read_init) {
        if (s_readInitCount.Increment() == 1) {
            detect_io_mode();
            s_read_init = true;
        } else {
            while (!s_read_init) {
                usleep(1000);
            }
        }
    }

    return read_func(file, buffer, overlapped, count);
}

bool System_File::write_first(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!s_write_init) {
        if (s_writeInitCount.Increment() == 1) {
            detect_io_mode();
            s_write_init = true;
        } else {
            while (!s_write_init) {
                usleep(1000);
            }
        }
    }

    return write_func(file, buffer, overlapped, count);
}

void System_File::FromNativeName(char* buffer, uint32_t buffersize, const char* str) {
    Blizzard::String::Copy(buffer, str ? str : "", buffersize);
}
