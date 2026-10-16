#include <cstdint>
#include <cstdlib>
#include <CoreFoundation/CoreFoundation.h>
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"

// The Storm registry on macOS lives in the user's preferences (CFPreferences), in the domain the
// original Mac client used. A value is stored under its full Windows key path joined with the value
// name, e.g. "Software\Blizzard Entertainment\World of Warcraft\InstallPath". Strings are stored as
// CFString and DWORDs as CFNumber, the two registry types Storm reads and writes.
//
// There is no machine-wide hive for a per-user application, so SREG_EXCLUDE_CURRENT_USER and
// SREG_EXCLUDE_LOCAL_MACHINE both address the same store.

static CFStringRef const kDomain = CFSTR("com.blizzard.World of Warcraft");

#define SREG_ERROR_INVALID_PARAMETER 87
#define SREG_ERROR_FILE_NOT_FOUND 2

int32_t STORMAPI SRegGetBaseKey(uint32_t flags, char* buffer, uint32_t bufferchars) {
    if (flags & SREG_BATTLE_NET) {
        SStrCopy(buffer, "Software\\Battle.net\\", bufferchars);
    } else {
        SStrCopy(buffer, "Software\\Blizzard Entertainment\\", bufferchars);
    }

    return 1;
}

static CFStringRef CreateValueKey(const char* keyname, const char* valuename, uint32_t flags) {
    char fullname[1024];
    fullname[0] = 0;

    if (!(flags & SREG_ABSOLUTE)) {
        SRegGetBaseKey(flags, fullname, sizeof(fullname));
    }

    SStrPack(fullname, keyname, sizeof(fullname));
    SStrPack(fullname, "\\", sizeof(fullname));
    SStrPack(fullname, valuename, sizeof(fullname));

    return CFStringCreateWithCString(kCFAllocatorDefault, fullname, kCFStringEncodingUTF8);
}

static CFPropertyListRef CopyValue(const char* keyname, const char* valuename, uint32_t flags) {
    CFStringRef key = CreateValueKey(keyname, valuename, flags);

    if (!key) {
        return nullptr;
    }

    CFPropertyListRef value = CFPreferencesCopyAppValue(key, kDomain);
    CFRelease(key);

    return value;
}

static int32_t SaveValue(const char* keyname, const char* valuename, uint32_t flags, CFPropertyListRef value) {
    CFStringRef key = CreateValueKey(keyname, valuename, flags);

    if (!key) {
        SErrSetLastError(SREG_ERROR_INVALID_PARAMETER);
        return 0;
    }

    CFPreferencesSetAppValue(key, value, kDomain);
    CFRelease(key);

    if ((flags & SREG_FLUSH_KEY) && !CFPreferencesAppSynchronize(kDomain)) {
        return 0;
    }

    return 1;
}

int32_t STORMAPI SRegLoadString(const char* keyname, const char* valuename, uint32_t flags, char* buffer, uint32_t buffersize) {
    if (!keyname || !*keyname || !valuename || !*valuename || !buffer || !buffersize) {
        SErrSetLastError(SREG_ERROR_INVALID_PARAMETER);
        return 0;
    }

    CFPropertyListRef value = CopyValue(keyname, valuename, flags);

    if (!value) {
        SErrSetLastError(SREG_ERROR_FILE_NOT_FOUND);
        return 0;
    }

    int32_t result = 1;

    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        if (!CFStringGetCString(static_cast<CFStringRef>(value), buffer, buffersize, kCFStringEncodingUTF8)) {
            // Truncate like a short registry read: keep what fits.
            CFIndex used = 0;
            CFStringRef string = static_cast<CFStringRef>(value);
            CFStringGetBytes(string, CFRangeMake(0, CFStringGetLength(string)), kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(buffer), buffersize - 1, &used);
            buffer[used] = 0;
        }
    } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        int64_t number = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number);
        SStrPrintf(buffer, buffersize, "%u", static_cast<uint32_t>(number));
    } else {
        result = 0;
    }

    CFRelease(value);

    return result;
}

int32_t STORMAPI SRegLoadValue(const char* keyname, const char* valuename, uint32_t flags, uint32_t* value) {
    if (!keyname || !*keyname || !valuename || !*valuename || !value) {
        SErrSetLastError(SREG_ERROR_INVALID_PARAMETER);
        return 0;
    }

    CFPropertyListRef stored = CopyValue(keyname, valuename, flags);

    if (!stored) {
        SErrSetLastError(SREG_ERROR_FILE_NOT_FOUND);
        return 0;
    }

    int32_t result = 1;

    if (CFGetTypeID(stored) == CFNumberGetTypeID()) {
        int64_t number = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(stored), kCFNumberSInt64Type, &number);
        *value = static_cast<uint32_t>(number);
    } else if (CFGetTypeID(stored) == CFStringGetTypeID()) {
        char buffer[256];

        if (CFStringGetCString(static_cast<CFStringRef>(stored), buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
            *value = static_cast<uint32_t>(strtoul(buffer, nullptr, 0));
        } else {
            result = 0;
        }
    } else {
        result = 0;
    }

    CFRelease(stored);

    return result;
}

int32_t STORMAPI SRegSaveString(const char* keyname, const char* valuename, uint32_t flags, const char* string) {
    if (!keyname || !*keyname || !valuename || !*valuename || !string) {
        SErrSetLastError(SREG_ERROR_INVALID_PARAMETER);
        return 0;
    }

    CFStringRef value = CFStringCreateWithCString(kCFAllocatorDefault, string, kCFStringEncodingUTF8);

    if (!value) {
        SErrSetLastError(SREG_ERROR_INVALID_PARAMETER);
        return 0;
    }

    int32_t result = SaveValue(keyname, valuename, flags, value);
    CFRelease(value);

    return result;
}

int32_t STORMAPI SRegSaveValue(const char* keyname, const char* valuename, uint32_t flags, uint32_t value) {
    if (!keyname || !*keyname || !valuename || !*valuename) {
        SErrSetLastError(SREG_ERROR_INVALID_PARAMETER);
        return 0;
    }

    int64_t number = value;
    CFNumberRef stored = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &number);
    int32_t result = SaveValue(keyname, valuename, flags, stored);
    CFRelease(stored);

    return result;
}
