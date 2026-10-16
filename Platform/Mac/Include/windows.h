// A curated subset of <windows.h> for the macOS build.
//
// Some Win64/ platform files are portable logic that only touches Win32 for kernel objects (events,
// critical sections, semaphores, waits, sleeping) and a few process queries. The macOS build compiles
// those files unchanged against this header instead of keeping a second copy of the logic; see
// cmake/Platform.cmake for the list. Everything declared here is implemented with real Win32
// semantics on top of Platform/Mac/Sync.h. It is not a general Win32 layer: an API gets added only
// when a shared Win64/ file needs it, and platform code that is genuinely Windows-specific (windows,
// input, sockets, crash handling) is written for macOS in Mac/ directories instead.

#ifndef PLATFORM_MAC_INCLUDE_WINDOWS_H
#define PLATFORM_MAC_INCLUDE_WINDOWS_H

#include <cstddef>
#include <cstdint>

// Types (the basic storage types come from Platform/Mac/Prefix.h)

typedef int32_t BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t UINT;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef size_t SIZE_T;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef DWORD* LPDWORD;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HGLOBAL;
typedef void* HRSRC;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID param);

#define WINAPI
#define CALLBACK
#define APIENTRY

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0x00000000
#define WAIT_TIMEOUT 0x00000102
#define WAIT_FAILED 0xFFFFFFFF
#define MAXIMUM_WAIT_OBJECTS 64
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
#define CREATE_SUSPENDED 0x00000004

#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080

#define ERROR_SUCCESS 0
#define ERROR_INVALID_HANDLE 6
#define ERROR_INVALID_PARAMETER 87

#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF

typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture;
    WORD wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO;

// Kernel objects

HANDLE CreateEventA(void* attributes, BOOL manualReset, BOOL initialState, LPCSTR name);
#define CreateEvent CreateEventA
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);

HANDLE CreateMutexA(void* attributes, BOOL initialOwner, LPCSTR name);
#define CreateMutex CreateMutexA
BOOL ReleaseMutex(HANDLE mutex);

HANDLE CreateSemaphoreA(void* attributes, LONG initialCount, LONG maximumCount, LPCSTR name);
#define CreateSemaphore CreateSemaphoreA
BOOL ReleaseSemaphore(HANDLE semaphore, LONG releaseCount, LONG* previousCount);

HANDLE CreateThread(void* attributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD threadId);

BOOL CloseHandle(HANDLE object);
DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* objects, BOOL waitAll, DWORD milliseconds);

// Critical sections (CRITICAL_SECTION is a recursive pthread mutex, see Prefix.h)

void InitializeCriticalSection(CRITICAL_SECTION* section);
BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* section, DWORD spinCount);
void DeleteCriticalSection(CRITICAL_SECTION* section);
void EnterCriticalSection(CRITICAL_SECTION* section);
void LeaveCriticalSection(CRITICAL_SECTION* section);
BOOL TryEnterCriticalSection(CRITICAL_SECTION* section);

// Per-thread last error

void SetLastError(DWORD error);
DWORD GetLastError();

// Threads and processes

void Sleep(DWORD milliseconds);
BOOL SwitchToThread();
DWORD GetTickCount();
DWORD GetCurrentThreadId();
DWORD GetCurrentProcessId();
HANDLE GetCurrentThread();
HANDLE GetCurrentProcess();
BOOL SetThreadPriority(HANDLE thread, int32_t priority);
int32_t GetThreadPriority(HANDLE thread);
void GetSystemInfo(SYSTEM_INFO* info);

// Executable resources. macOS executables carry no PE resources; these read the resource section
// of the original client executable in the game directory, which carries the same data.

HRSRC FindResourceExA(HMODULE module, LPCSTR type, LPCSTR name, WORD language);
HGLOBAL LoadResource(HMODULE module, HRSRC resource);
LPVOID LockResource(HGLOBAL data);
DWORD SizeofResource(HMODULE module, HRSRC resource);

#endif
