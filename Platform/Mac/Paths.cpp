#include "Platform/Mac/Paths.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mach-o/dyld.h>
#include <strings.h>

namespace MacPaths {

bool ExecutablePath(char* buffer, size_t buffersize) {
    char path[4096];
    uint32_t size = sizeof(path);

    if (_NSGetExecutablePath(path, &size) != 0) {
        return false;
    }

    char resolved[PATH_MAX];

    if (!realpath(path, resolved)) {
        return false;
    }

    if (strlen(resolved) + 1 > buffersize) {
        return false;
    }

    strcpy(buffer, resolved);

    return true;
}

bool ClientDirectory(char* buffer, size_t buffersize) {
    char path[PATH_MAX];

    if (!ExecutablePath(path, sizeof(path))) {
        return false;
    }

    // .../Something.app/Contents/MacOS/executable: the client directory holds Something.app.
    char* bundle = strstr(path, ".app/Contents/MacOS/");

    if (bundle) {
        *bundle = '\0';
    }

    char* slash = strrchr(path, '/');

    if (!slash) {
        return false;
    }

    slash[1] = '\0';

    if (strlen(path) + 1 > buffersize) {
        return false;
    }

    strcpy(buffer, path);

    return true;
}

bool FindInClientDirectory(const char* name, char* buffer, size_t buffersize) {
    char directory[PATH_MAX];

    if (!ClientDirectory(directory, sizeof(directory))) {
        return false;
    }

    DIR* dir = opendir(directory);

    if (!dir) {
        return false;
    }

    bool found = false;

    while (dirent* entry = readdir(dir)) {
        if (strcasecmp(entry->d_name, name) == 0) {
            found = static_cast<size_t>(snprintf(buffer, buffersize, "%s%s", directory, entry->d_name)) < buffersize;
            break;
        }
    }

    closedir(dir);

    return found;
}

}
