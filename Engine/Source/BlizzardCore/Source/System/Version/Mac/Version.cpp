#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/Version/Version.h"
#include "Platform/Mac/PEResources.h"

// The files the client asks versions of are Windows executables and libraries from the game
// directory; their version record is read straight from the PE resources, as version.dll does.
bool Blizzard::Version::GetFileVersion(const char* path, FileVersion& version) {
    version.m_major = 0;
    version.m_minor = 0;
    version.m_build = 0;
    version.m_revision = 0;

    if (Blizzard::File::Exists(path) != 1) {
        return false;
    }

    char native[1024];
    Blizzard::String::MakeUnivPath(path, native, sizeof(native));

    uint32_t fileMS;
    uint32_t fileLS;
    uint32_t productMS;
    uint32_t productLS;

    if (!MacPEResources::ReadFileVersion(native, &fileMS, &fileLS, &productMS, &productLS)) {
        return false;
    }

    version.m_major = fileMS >> 16;
    version.m_minor = fileMS & 0xFFFF;
    version.m_build = fileLS >> 16;
    version.m_revision = fileLS & 0xFFFF;

    return true;
}
