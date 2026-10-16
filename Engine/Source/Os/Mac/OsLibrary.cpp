#include <dlfcn.h>
#include "Engine/Source/Os/OsLibrary.h"

// Dynamic libraries through dyld. The libraries the client asks for by name are Windows DLLs
// (Scan.dll, Survey.dll); like a missing DLL on Windows, they simply fail to load.

void* OsLoadLibrary(const char* fileName) {
    return fileName ? dlopen(fileName, RTLD_NOW | RTLD_LOCAL) : nullptr;
}

void OsFreeLibrary(void* module) {
    if (module) {
        dlclose(module);
    }
}

void* OsGetLibraryEntryPoint(void* module, const char* name) {
    // Ordinal lookups (names below 0x10000) have no dyld equivalent.
    if (!module || reinterpret_cast<uintptr_t>(name) <= 0xFFFF) {
        return nullptr;
    }

    return dlsym(module, name);
}
