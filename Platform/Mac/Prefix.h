// Force-included into every C++ translation unit of the macOS build (see cmake/Platform.cmake).
//
// The decompiled sources were written against MSVC and its CRT. This header supplies the handful of
// MSVC keywords, CRT functions and Win32 interlocked primitives that shared (non-platform) code uses,
// so that code compiles unchanged. Anything platform-specific beyond this belongs in a Mac/ directory
// next to the matching Win32/ or Win64/ one, not here.

#ifndef PLATFORM_MAC_PREFIX_H
#define PLATFORM_MAC_PREFIX_H

#if !defined(WOWDECOMP_PLATFORM_MAC)
#error "Platform/Mac/Prefix.h is only for the macOS build"
#endif

#if defined(__OBJC__)
#import <Foundation/Foundation.h>
#endif

#if defined(__cplusplus)

#include <alloca.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc/malloc.h>

// MSVC keywords

#define __int64 long long
#define __forceinline inline __attribute__((always_inline))

#if !defined(__stdcall)
#define __stdcall
#endif
#if !defined(__cdecl)
#define __cdecl
#endif
#if !defined(__fastcall)
#define __fastcall
#endif
#if !defined(__thiscall)
#define __thiscall
#endif

// MSVC intrinsics that clang provides only on x86

#if defined(__aarch64__)
#define __nop() __asm__ __volatile__("nop")
#endif

// MSVC CRT. _rotl and friends are clang builtins under -fms-extensions.

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _alloca alloca
#define _finite isfinite
#define _isnan isnan
#define _atoi64 atoll
#define _msize malloc_size
#define _ismbcspace(c) isspace(static_cast<unsigned char>(c))
#define _strtoi64 strtoll
#define _strtoui64 strtoull

static inline int fopen_s(FILE** file, const char* name, const char* mode) {
    *file = fopen(name, mode);
    return *file ? 0 : errno;
}

static inline char* _strupr(char* string) {
    for (char* c = string; *c; c++) {
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    }

    return string;
}

static inline char* _strlwr(char* string) {
    for (char* c = string; *c; c++) {
        *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
    }

    return string;
}

// MSVC's rand() is a per-thread linear congruential generator returning 15 bits (RAND_MAX 32767).
// The client scales and compares its results against that range (cloud noise, animation variation
// frequencies, sequence repeats), so libc's 31-bit rand() must not reach it. The state lives in an
// inline function so every translation unit shares it and srand() seeds what rand() returns.
inline uint32_t& MsvcRandState() {
    thread_local uint32_t state = 1;
    return state;
}

inline int MsvcRand() {
    uint32_t& state = MsvcRandState();
    state = state * 214013u + 2531011u;
    return static_cast<int>((state >> 16) & 0x7FFF);
}

inline void MsvcSrand(unsigned int seed) {
    MsvcRandState() = seed;
}

#define rand MsvcRand
#define srand MsvcSrand
#undef RAND_MAX
#define RAND_MAX 0x7FFF

// MSVC's printf and scanf families take the I64 and I32 length modifiers ("%I64d", "%I64X"), and
// Storm's ordered printf rewrites "%lld" to "%I64d" before every call. libc does not know them: it
// prints "I64d" and leaves the argument unread, and sscanf stops matching. Formats are translated to
// the libc spelling here. A format longer than the buffer is passed through unchanged.
inline const char* MsvcTranslateFormat(const char* format, char* buffer, size_t size) {
    if (!strstr(format, "I64") && !strstr(format, "I32")) {
        return format;
    }

    size_t out = 0;

    for (const char* c = format; *c; c++) {
        if (out + 3 >= size) {
            return format;
        }

        buffer[out++] = *c;

        if (*c != '%') {
            continue;
        }

        if (c[1] == '%') {
            buffer[out++] = *++c;
            continue;
        }

        // Flags, argument index, width and precision
        while (c[1] && strchr("-+ #0123456789.*$'", c[1])) {
            if (out + 3 >= size) {
                return format;
            }
            buffer[out++] = *++c;
        }

        if (c[1] == 'I' && c[2] == '6' && c[3] == '4') {
            buffer[out++] = 'l';
            buffer[out++] = 'l';
            c += 3;
        } else if (c[1] == 'I' && c[2] == '3' && c[3] == '2') {
            c += 3;
        }
    }

    buffer[out] = '\0';
    return buffer;
}

// MSVC's _vsnprintf fills all count characters without a terminator when the output does not fit
// below count, returning count on an exact fit and -1 when it truncates. Storm relies on both.
inline int MsvcVsnprintf(char* dest, size_t count, const char* format, va_list args) {
    char translated[512];
    format = MsvcTranslateFormat(format, translated, sizeof(translated));

    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(dest, count, format, copy);
    va_end(copy);

    if (length < 0 || static_cast<size_t>(length) < count) {
        return length;
    }

    char* full = static_cast<char*>(malloc(static_cast<size_t>(length) + 1));
    vsnprintf(full, static_cast<size_t>(length) + 1, format, args);
    memcpy(dest, full, count);
    free(full);

    return static_cast<size_t>(length) == count ? length : -1;
}

inline int MsvcSnprintf(char* dest, size_t count, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = MsvcVsnprintf(dest, count, format, args);
    va_end(args);
    return result;
}

inline int MsvcSscanf(const char* string, const char* format, ...) {
    char translated[512];
    va_list args;
    va_start(args, format);
    int result = vsscanf(string, MsvcTranslateFormat(format, translated, sizeof(translated)), args);
    va_end(args);
    return result;
}

#define _snprintf MsvcSnprintf
#define _vsnprintf MsvcVsnprintf
#define sscanf MsvcSscanf

// Win32 storage types that shared headers declare members and parameters with. Only the Mac/
// platform implementations look inside them; shared code treats them as opaque. Win32 LONG and DWORD
// are 32 bits on every Windows target, so they stay 32 bits here instead of following the LP64 long.

#include <limits.h>
#include <pthread.h>

typedef int32_t LONG;
typedef uint32_t DWORD;
typedef void* HANDLE;
typedef void* HWND;
// A Win32 critical section: a recursive pthread mutex plus the DebugInfo field the client clears
// after deleting one.
typedef struct _CRITICAL_SECTION {
    pthread_mutex_t m_mutex;
    void* DebugInfo;
} CRITICAL_SECTION;

typedef union _LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER;

typedef struct _OVERLAPPED {
    uintptr_t Internal;
    uintptr_t InternalHigh;
    uint32_t Offset;
    uint32_t OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED;

typedef struct tagRECT {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} RECT;

typedef struct tagPOINT {
    int32_t x;
    int32_t y;
} POINT;

#define MAX_PATH PATH_MAX

// The ANSI code page the client reports with IME events. 3.3.5a clients ran on code page 1252
// for the western locales; the Mac input layer has no ANSI code page of its own.
static inline uint32_t GetACP() {
    return 1252;
}

// Win32 interlocked primitives. The client calls them on LONG and on long counters, so they take any
// integer type here rather than only the 32-bit LONG.

template <class T>
static inline T InterlockedIncrement(volatile T* addend) {
    return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

template <class T>
static inline T InterlockedDecrement(volatile T* addend) {
    return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

template <class T, class V>
static inline T InterlockedExchange(volatile T* target, V value) {
    return __atomic_exchange_n(target, static_cast<T>(value), __ATOMIC_SEQ_CST);
}

template <class T, class V>
static inline T InterlockedExchangeAdd(volatile T* addend, V value) {
    return __atomic_fetch_add(addend, static_cast<T>(value), __ATOMIC_SEQ_CST);
}

template <class T, class V, class C>
static inline T InterlockedCompareExchange(volatile T* destination, V exchange, C comparand) {
    T expected = static_cast<T>(comparand);
    __atomic_compare_exchange_n(destination, &expected, static_cast<T>(exchange), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

static inline void* InterlockedCompareExchangePointer(void* volatile* destination, void* exchange, void* comparand) {
    __atomic_compare_exchange_n(destination, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

#include <typeinfo>

// std::type_info::raw_name() is MSVC-only. The client only passes it to the Storm allocators as a
// debug tag, so the portable name() is an equivalent stand-in.
#define raw_name name

#endif

#endif
