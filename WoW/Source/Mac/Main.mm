#include <csignal>
#include <pthread/qos.h>
#include <cstdlib>
#include <unistd.h>
#import <Foundation/Foundation.h>
#include "Engine/Source/Os/Mac/MacOs.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Platform/Mac/CommandLine.h"
#include "Platform/Mac/Paths.h"
#include "Platform/Mac/EpochRelay.h"

// The macOS entry point, doing what the Windows start() and WinMain do before CommonMain:
//
// - Storm's runtime is initialized first.
// - The client runs on the main thread, as AppKit requires; its 8 MB stack matches the fiber Windows
//   creates for it.
// - The working directory is the client directory, where Wow.exe's shortcut starts it on Windows; an
//   application opened from the Finder starts in "/".
// - SIGPIPE is ignored: a write to a closed socket reports an error, as on Windows.
// - The main thread, which updates and renders the world, runs at the user-interactive quality of
//   service, so the scheduler keeps it on the performance cores whatever started the process.
//
// The floating-point environment needs no setup: arm64 already masks all exceptions, rounds to
// nearest and keeps denormals, the state the Windows client sets with _control87.

// From WoW/Source/Client.h, which also declares the Windows main() that this one replaces.
int32_t CommonMain();

int main(int argc, char** argv) {
    MacCommandLine::SetArguments(argc, argv);
    StormRtlInitialize();

    signal(SIGPIPE, SIG_IGN);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    char directory[4096];

    if (MacPaths::ClientDirectory(directory, sizeof(directory))) {
        chdir(directory);
    }

    @autoreleasepool {
        OsMacInitApplication();

        int32_t result = CommonMain();
        EpochRelay::Shutdown();
        return result;
    }
}
