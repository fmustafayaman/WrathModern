#include <cstdint>
#include "WoW/Source/Glue/CGlueMgr.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"

// Downloaded patches are launched like any child process. Windows elevates them with "runas"; macOS
// installers ask for privileges themselves when they need them.
bool CGlueMgr::PatchDownloadExecuteCommand(const char* path, const char* arguments) {
    return Blizzard::Process::LaunchChildProcessWithArguments(path, arguments, nullptr, nullptr);
}
