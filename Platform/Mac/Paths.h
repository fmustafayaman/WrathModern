// Where the client runs from on macOS.
//
// The Windows client finds Data/, WTF/, Interface/ and its logs next to Wow.exe. On macOS the
// executable lives inside an application bundle, so the client's directory is the one that
// contains the bundle: WrathModern.app is placed in the 3.3.5a game directory like Wow.exe is.
// A bare executable outside a bundle uses its own directory.

#ifndef PLATFORM_MAC_PATHS_H
#define PLATFORM_MAC_PATHS_H

#include <cstddef>

namespace MacPaths {

// Full path of the running executable. Returns false if it does not fit.
bool ExecutablePath(char* buffer, size_t buffersize);

// The client directory, with a trailing slash. Returns false if it does not fit.
bool ClientDirectory(char* buffer, size_t buffersize);

// Path of a file in the client directory whose name matches case-insensitively, the way Windows
// resolves names. Returns false if there is no such file.
bool FindInClientDirectory(const char* name, char* buffer, size_t buffersize);

}

#endif
