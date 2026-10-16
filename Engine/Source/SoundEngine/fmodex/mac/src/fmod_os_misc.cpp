/*
    FMOD Ex operating system layer for macOS: memory, files, time, threads, critical sections,
    semaphores, libraries and device change detection, with the behaviour of the Win32 layer.
*/

#include "fmod_settings.h"
#include "fmod.h"
#include "fmod_os_misc.h"
#include "fmod_memory.h"

#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int gSizeofCriticalSection = sizeof(pthread_mutex_t);
int gSizeofSemaphore       = 0;

void *FMOD_OS_Memory_Alloc(int size, FMOD_MEMORY_TYPE type)
{
    return malloc(size);
}

void *FMOD_OS_Memory_Realloc(void *ptr, int size, FMOD_MEMORY_TYPE type)
{
    return realloc(ptr, size);
}

void FMOD_OS_Memory_Free(void *ptr, FMOD_MEMORY_TYPE type)
{
    free(ptr);
}

FMOD_RESULT FMOD_OS_File_DriveStatus()
{
    return FMOD_OK;
}

/*
    File handles are POSIX descriptors offset by one, so that a valid handle is never null. Names are
    UTF-8; the client never opens files by wide name.
*/
static void *FileHandle(int fd)
{
    return (void *)(intptr_t)(fd + 1);
}

static int FileDescriptor(void *handle)
{
    return (int)((intptr_t)handle - 1);
}

FMOD_RESULT FMOD_OS_File_Open(const char *name, char *mode, int unicode, unsigned int *filesize, void **handle)
{
    int fd = open(name, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
    {
        *handle = (void *)-1;

        if (errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
        {
            return FMOD_ERR_FILE_BAD;
        }

        return FMOD_ERR_FILE_NOTFOUND;
    }

    struct stat info;

    if (fstat(fd, &info) != 0)
    {
        close(fd);
        *handle = (void *)-1;
        return FMOD_ERR_FILE_BAD;
    }

    *handle = FileHandle(fd);
    *filesize = (unsigned int)info.st_size;

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Close(void *handle)
{
    if (handle == (void *)-1 || !handle)
    {
        return FMOD_ERR_FILE_BAD;
    }

    close(FileDescriptor(handle));

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Cancel(void *handle)
{
    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Read(void *handle, void *buf, unsigned int count, unsigned int *read)
{
    unsigned int total = 0;

    while (total < count)
    {
        ssize_t result = ::read(FileDescriptor(handle), (char *)buf + total, count - total);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            *read = total;
            return FMOD_ERR_FILE_BAD;
        }

        if (!result)
        {
            break;
        }

        total += (unsigned int)result;
    }

    *read = total;

    if (total != count)
    {
        return FMOD_ERR_FILE_EOF;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_File_Seek(void *handle, unsigned int offset)
{
    if (lseek(FileDescriptor(handle), offset, SEEK_SET) < 0)
    {
        return FMOD_ERR_FILE_BAD;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Debug_OutputStr(const char *s)
{
    fputs(s, stderr);

    return FMOD_OK;
}

/*
    "Nanoseconds" are microseconds here as on Windows (1000th of a millisecond), truncated to 32 bits.
*/
FMOD_RESULT FMOD_OS_Time_GetNs(unsigned int *ns)
{
    *ns = (unsigned int)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Time_GetMs(unsigned int *ms)
{
    *ms = (unsigned int)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Time_Sleep(unsigned int ms)
{
    if (!ms)
    {
        sched_yield();
        return FMOD_OK;
    }

    usleep(ms * 1000);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Thread_GetCurrentID(FMOD_UINT_NATIVE *id)
{
    uint64_t threadid;
    pthread_threadid_np(NULL, &threadid);
    *id = (FMOD_UINT_NATIVE)threadid;

    return FMOD_OK;
}

typedef struct
{
    THREAD_RETURNTYPE (THREAD_CALLCONV *func)(void *param);
    void                *param;
    FMOD_THREAD_PRIORITY priority;
    char                 name[64];
} FMOD_OS_THREADSTART;

static void *ThreadStart(void *data)
{
    FMOD_OS_THREADSTART start = *(FMOD_OS_THREADSTART *)data;
    free(data);

    if (start.name[0])
    {
        pthread_setname_np(start.name);
    }

    /*
        The Windows layer maps priorities onto thread priorities, and critical onto the "Pro Audio"
        multimedia class. The mixer is the critical thread; it gets the interactive QoS class that
        macOS schedules audio-adjacent work in.
    */
    switch (start.priority)
    {
        case FMOD_THREAD_PRIORITY_VERYLOW:
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
            break;
        case FMOD_THREAD_PRIORITY_LOW:
            pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, -8);
            break;
        case FMOD_THREAD_PRIORITY_NORMAL:
            pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0);
            break;
        case FMOD_THREAD_PRIORITY_HIGH:
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
            break;
        case FMOD_THREAD_PRIORITY_VERYHIGH:
        case FMOD_THREAD_PRIORITY_CRITICAL:
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            break;
        default:
            break;
    }

    start.func(start.param);

    return NULL;
}

FMOD_RESULT FMOD_OS_Thread_Create(const char *name, THREAD_RETURNTYPE (THREAD_CALLCONV *func)(void *param), void *param, FMOD_THREAD_PRIORITY priority, void *stack, int stacksize, void **handle)
{
    if (!handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    FMOD_OS_THREADSTART *start = (FMOD_OS_THREADSTART *)malloc(sizeof(FMOD_OS_THREADSTART));

    if (!start)
    {
        return FMOD_ERR_MEMORY;
    }

    start->func = func;
    start->param = param;
    start->priority = priority;
    start->name[0] = 0;

    if (name)
    {
        strncpy(start->name, name, sizeof(start->name) - 1);
        start->name[sizeof(start->name) - 1] = 0;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, stacksize > (int)PTHREAD_STACK_MIN ? (size_t)((stacksize + 0xFFF) & ~0xFFF) : 1024 * 1024);

    pthread_t thread;
    int result = pthread_create(&thread, &attr, ThreadStart, start);
    pthread_attr_destroy(&attr);

    if (result)
    {
        free(start);
        *handle = 0;
        return FMOD_ERR_MEMORY;
    }

    *handle = (void *)thread;

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Thread_Destroy(void *handle)
{
    /* Threads are detached; FMOD's Thread class waits for its thread to finish before this. */
    return FMOD_OK;
}

static pthread_mutex_t gMemoryCrit;

static void InitRecursive(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

FMOD_RESULT FMOD_OS_CriticalSection_Create(FMOD_OS_CRITICALSECTION **crit, bool memorycrit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (memorycrit)
    {
        *crit = (FMOD_OS_CRITICALSECTION *)&gMemoryCrit;    /* Can't use alloc on the critical section used in the memory system. */
    }
    else
    {
        *crit = (FMOD_OS_CRITICALSECTION *)FMOD_Memory_Alloc(sizeof(pthread_mutex_t));

        if (!*crit)
        {
            return FMOD_ERR_MEMORY;
        }
    }

    InitRecursive((pthread_mutex_t *)(*crit));

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Free(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_destroy((pthread_mutex_t *)crit);

    if (crit != (FMOD_OS_CRITICALSECTION *)&gMemoryCrit)
    {
        FMOD_Memory_Free(crit);
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Enter(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock((pthread_mutex_t *)crit);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_CriticalSection_Leave(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_unlock((pthread_mutex_t *)crit);

    return FMOD_OK;
}

/*
    Counting semaphores, starting at zero, as the metered sections of the Win32 layer.
*/
FMOD_RESULT FMOD_OS_Semaphore_Create(FMOD_OS_SEMAPHORE **sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *sema = (FMOD_OS_SEMAPHORE *)dispatch_semaphore_create(0);

    return *sema ? FMOD_OK : FMOD_ERR_MEMORY;
}

FMOD_RESULT FMOD_OS_Semaphore_Free(FMOD_OS_SEMAPHORE *sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_release((dispatch_semaphore_t)sema);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Semaphore_Wait(FMOD_OS_SEMAPHORE *sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_semaphore_wait((dispatch_semaphore_t)sema, DISPATCH_TIME_FOREVER);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Semaphore_Signal(FMOD_OS_SEMAPHORE *sema, bool interrupt)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    dispatch_semaphore_signal((dispatch_semaphore_t)sema);

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Library_Load(const char *dllname, FMOD_OS_LIBRARY **handle)
{
    if (!dllname || !handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *handle = (FMOD_OS_LIBRARY *)dlopen(dllname, RTLD_NOW | RTLD_LOCAL);

    if (!*handle)
    {
        return FMOD_ERR_FILE_NOTFOUND;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Library_GetProcAddress(FMOD_OS_LIBRARY *handle, const char *procname, void **address)
{
    if (!handle || !address)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *address = dlsym((void *)handle, procname);

    if (!*address)
    {
        return FMOD_ERR_FORMAT;
    }

    return FMOD_OK;
}

FMOD_RESULT FMOD_OS_Library_Free(FMOD_OS_LIBRARY *handle)
{
    if (!handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (dlclose((void *)handle))
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    return FMOD_OK;
}

/*
    Reports when audio devices come or go, as the Win32 layer counts wave devices.
*/
FMOD_RESULT FMOD_OS_CheckDriverList(bool *devicelistchanged)
{
    static int lastNumDrivers = -1;

    AudioObjectPropertyAddress address = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    int numDrivers = -1;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) == noErr)
    {
        numDrivers = (int)(size / sizeof(AudioDeviceID));
    }

    *devicelistchanged = (numDrivers != lastNumDrivers && lastNumDrivers != -1);
    lastNumDrivers = numDrivers;

    return FMOD_OK;
}
