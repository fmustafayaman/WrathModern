#include <dlfcn.h>
#include <new>
#include "WoW/Source/Battlenet/Battlenet.h"

// Battle.net client libraries through dyld. The library the client loads is the Windows
// battle.net.dll; it fails to load here as a missing DLL does on Windows, and the client goes on
// without it.

void* Battlenet::WindowsLibrary::GetProc(const char* name) {
    return name ? dlsym(this->m_module, name) : nullptr;
}

void* Battlenet::WindowsLibrary::GetProc(uint32_t ordinal) {
    // Exports by ordinal have no dyld counterpart.
    return nullptr;
}

Battlenet::Library* Battlenet::Library::Load(const char* path, Allocator& allocator) {
    void* module = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : nullptr;

    if (!module) {
        return nullptr;
    }

    void* memory = allocator.Alloc(sizeof(WindowsLibrary));

    if (!memory) {
        dlclose(module);
        return nullptr;
    }

    WindowsLibrary* library = new (memory) WindowsLibrary();
    library->m_module = module;
    library->m_allocator = &allocator;

    return library;
}

Battlenet::WindowsLibrary::~WindowsLibrary() {
    dlclose(this->m_module);
}
