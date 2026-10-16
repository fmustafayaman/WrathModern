#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "Engine/Source/Storm/Source/SLog.h"
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/DebugOutput.h"
#include "Platform/Mac/Paths.h"

// Storm logs on macOS. The logic follows the Windows version; _LOG::file holds a POSIX descriptor
// (kNoFile when closed), paths use slashes, and lines end in "\n" like other macOS text files.

static void* const kNoFile = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

static int32_t s_logsysteminit;
static uintptr_t s_sequence;
static char s_defaultdir[MAX_PATH];
static CRITICAL_SECTION s_defaultdir_critsect;
static _LOG* s_loghead[4];
static CRITICAL_SECTION s_critsect[4];

static int32_t Descriptor(void* file) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(file));
}

static void Lock(CRITICAL_SECTION* section) {
    pthread_mutex_lock(&section->m_mutex);
}

static void Unlock(CRITICAL_SECTION* section) {
    pthread_mutex_unlock(&section->m_mutex);
}

static void InitSection(CRITICAL_SECTION* section) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&section->m_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    section->DebugInfo = nullptr;
}

static void FlushLog(_LOG* logptr) {
    if (logptr->bufferused) {
        const char* data = logptr->buffer;
        size_t remaining = logptr->bufferused;

        while (remaining) {
            ssize_t written = write(Descriptor(logptr->file), data, remaining);

            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }

                break;
            }

            data += written;
            remaining -= static_cast<size_t>(written);
        }

        fsync(Descriptor(logptr->file));
        logptr->bufferused = 0;
        logptr->pendpoint = 0;
    }
}

static _LOG* LockLog(HSLOG log, HLOCKEDLOG* lockedhandle, int32_t createifnecessary) {
    if (!log) {
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    uintptr_t index = reinterpret_cast<uintptr_t>(log) & 3;
    Lock(&s_critsect[index]);
    *lockedhandle = reinterpret_cast<HLOCKEDLOG>(index);

    _LOG** prevptr = &s_loghead[index];

    for (_LOG* logptr = s_loghead[index]; logptr; logptr = logptr->next) {
        if (logptr->log == log) {
            return logptr;
        }

        prevptr = &logptr->next;
    }

    if (!createifnecessary) {
        Unlock(&s_critsect[index]);
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    _LOG* logptr = static_cast<_LOG*>(calloc(1, sizeof(_LOG)));
    *prevptr = logptr;

    if (!logptr) {
        Unlock(&s_critsect[index]);
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    logptr->log = log;
    logptr->next = nullptr;
    logptr->filename[0] = 0;
    logptr->file = kNoFile;
    logptr->bufferused = 0;
    logptr->pendpoint = 0;

    return logptr;
}

static void OutputIndent(_LOG* logptr) {
    int32_t indent = logptr->indent;

    if (indent > 0) {
        if (indent >= 128) {
            indent = 128;
        }

        memset(&logptr->buffer[logptr->bufferused], ' ', indent);
        logptr->buffer[logptr->bufferused + indent] = 0;
        logptr->bufferused += indent;
    }
}

static void OutputReturn(_LOG* logptr) {
    strcpy(&logptr->buffer[logptr->bufferused], "\n");
    logptr->bufferused += 1;
}

static void OutputTime(_LOG* logptr, int32_t show) {
    static uint64_t lasttime;
    static char timestr[64];
    static uint32_t timestrlen;

    if (!logptr->timeStamp) {
        return;
    }

    timeval now;
    gettimeofday(&now, nullptr);
    uint64_t time = static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;

    if (time != lasttime) {
        lasttime = time;

        time_t seconds = now.tv_sec;
        tm local;
        localtime_r(&seconds, &local);
        snprintf(timestr, sizeof(timestr), "%u/%u %02u:%02u:%02u.%03u  ", local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<uint32_t>(now.tv_usec / 1000));
        timestrlen = SStrLen(timestr);
    }

    if (show) {
        memcpy(&logptr->buffer[logptr->bufferused], timestr, timestrlen + 1);
    } else {
        memset(&logptr->buffer[logptr->bufferused], ' ', timestrlen);
        logptr->buffer[logptr->bufferused + timestrlen] = 0;
    }

    logptr->bufferused += timestrlen;
}

static void UnlockDeleteLog(_LOG* logptr, HLOCKEDLOG lockedhandle) {
    uintptr_t index = reinterpret_cast<uintptr_t>(lockedhandle);
    _LOG** prevptr = &s_loghead[index];

    for (_LOG* curr = s_loghead[index]; curr; curr = curr->next) {
        if (curr == logptr) {
            *prevptr = curr->next;
            free(curr);
            break;
        }

        prevptr = &curr->next;
    }

    Unlock(&s_critsect[index]);
}

static void UnlockLog(HLOCKEDLOG lockedhandle) {
    Unlock(&s_critsect[reinterpret_cast<uintptr_t>(lockedhandle)]);
}

// A bare file name goes into the default directory, or the client directory when none is set; a
// name with a path is used as given, with backslashes turned into slashes.
static const char* PrependDefaultDir(char* newfilename, uint32_t newfilenamesize, const char* filename) {
    if (!filename || !*filename) {
        return filename;
    }

    if (filename[0] == '/' || SStrChr(filename, '\\') || SStrChr(filename, '/')) {
        SStrCopy(newfilename, filename, newfilenamesize);

        for (char* c = newfilename; *c; c++) {
            if (*c == '\\') {
                *c = '/';
            }
        }

        return newfilename;
    }

    Lock(&s_defaultdir_critsect);

    if (s_defaultdir[0]) {
        size_t chars = SStrCopy(newfilename, s_defaultdir, newfilenamesize);
        SStrCopy(&newfilename[chars], filename, newfilenamesize - chars);
    } else {
        if (!MacPaths::ClientDirectory(newfilename, newfilenamesize)) {
            newfilename[0] = 0;
        }

        SStrPack(newfilename, filename, newfilenamesize);
    }

    Unlock(&s_defaultdir_critsect);

    return newfilename;
}

void STORMAPI SLogClose(HSLOG log) {
    if (!s_logsysteminit) {
        return;
    }

    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (!logptr) {
        return;
    }

    if (logptr->file != kNoFile) {
        FlushLog(logptr);
        close(Descriptor(logptr->file));
    }

    UnlockDeleteLog(logptr, lockedhandle);
}

void STORMAPI SLogFlush(HSLOG log) {
    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (logptr) {
        if (logptr->file != kNoFile) {
            FlushLog(logptr);
        }

        UnlockLog(lockedhandle);
    }
}

void STORMAPI SLogFlushAll() {
    for (int32_t i = 0; i < 4; i++) {
        Lock(&s_critsect[i]);

        for (_LOG* logptr = s_loghead[i]; logptr; logptr = logptr->next) {
            if (logptr->file != kNoFile) {
                FlushLog(logptr);
            }
        }

        Unlock(&s_critsect[i]);
    }
}

void STORMAPI SLogInitialize() {
    if (s_logsysteminit) {
        return;
    }

    for (int32_t i = 0; i < 4; i++) {
        InitSection(&s_critsect[i]);
    }

    InitSection(&s_defaultdir_critsect);
    s_logsysteminit = 1;
}

// Creates the directories leading to a file.
static void CreateFileDirectory(const char* path) {
    char buffer[MAX_PATH];
    SStrCopy(buffer, path, sizeof(buffer));

    char* slash = strrchr(buffer, '/');

    if (!slash || slash == buffer) {
        return;
    }

    *slash = 0;

    for (char* c = buffer + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            mkdir(buffer, 0755);
            *c = '/';
        }
    }

    mkdir(buffer, 0755);
}

static int32_t OpenLogFile(const char* filename, void** file, uint32_t flags) {
    if (!filename || !*filename) {
        *file = kNoFile;
        return 0;
    }

    char newfilename[MAX_PATH];
    const char* path = PrependDefaultDir(newfilename, sizeof(newfilename), filename);
    int32_t append = (flags & SLOG_FLAG_APPEND) != 0;
    CreateFileDirectory(path);

    int32_t fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0644);
    *file = fd >= 0 ? reinterpret_cast<void*>(static_cast<intptr_t>(fd)) : kNoFile;

    return fd >= 0;
}

int32_t STORMAPI SLogCreate(const char* filename, uint32_t flags, HSLOG* log) {
    if (!filename || !*filename || !log) {
        SErrSetLastError(87);
        return 0;
    }

    *log = nullptr;

    if (flags & SLOG_FLAG_NOFILE) {
        filename = "";
        flags &= ~SLOG_FLAG_OPENFILE;
    }

    void* file = kNoFile;

    if ((flags & SLOG_FLAG_OPENFILE) && !OpenLogFile(filename, &file, flags)) {
        return 0;
    }

    *log = reinterpret_cast<HSLOG>(++s_sequence);

    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(*log, &lockedhandle, 1);

    if (!logptr) {
        *log = nullptr;
        return 0;
    }

    logptr->file = file;
    SStrCopy(logptr->filename, filename, sizeof(logptr->filename));
    logptr->flags = flags;
    logptr->timeStamp = 1;
    logptr->indent = 0;
    UnlockLog(lockedhandle);

    return 1;
}

void STORMAPI SLogDestroy() {
    SLogFlushAll();

    for (int32_t i = 0; i < 4; i++) {
        Lock(&s_critsect[i]);

        while (s_loghead[i]) {
            char filename[MAX_PATH];
            SStrCopy(filename, s_loghead[i]->filename, STORM_MAX_STR);
            SLogClose(s_loghead[i]->log);
            SErrReportNamedResourceLeak("HSLOG", filename);
        }

        Unlock(&s_critsect[i]);
    }

    s_logsysteminit = 0;

    for (int32_t i = 0; i < 4; i++) {
        pthread_mutex_destroy(&s_critsect[i].m_mutex);
    }

    pthread_mutex_destroy(&s_defaultdir_critsect.m_mutex);
}

static int32_t PrepareLog(_LOG* logptr) {
    if (logptr->file != kNoFile) {
        return 1;
    }

    if (OpenLogFile(logptr->filename, &logptr->file, logptr->flags)) {
        return 1;
    }

    logptr->filename[0] = 0;
    return 0;
}

void STORMAPI SLogVWrite(HSLOG log, const char* format, va_list arglist) {
    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (!logptr) {
        return;
    }

    if (PrepareLog(logptr)) {
        OutputTime(logptr, 1);
        OutputIndent(logptr);
        SStrVPrintf(&logptr->buffer[logptr->bufferused], sizeof(logptr->buffer) - logptr->bufferused, format, arglist);
        logptr->bufferused += SStrLen(&logptr->buffer[logptr->bufferused]);
        OutputReturn(logptr);

        if (g_opt.echotooutputdebugstring) {
            MacDebugOutput::Write(&logptr->buffer[logptr->pendpoint]);
        }

        logptr->pendpoint = logptr->bufferused;

        if (logptr->bufferused >= 0xC000) {
            FlushLog(logptr);
        }
    }

    UnlockLog(lockedhandle);
}

void SLogWrite(HSLOG log, const char* format, ...) {
    va_list arglist;
    va_start(arglist, format);
    SLogVWrite(log, format, arglist);
    va_end(arglist);
}
