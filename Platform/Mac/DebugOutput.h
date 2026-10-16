// OutputDebugString for macOS: debugger and log output for diagnostics.
//
// Text goes to the unified log (visible in Console.app and `log stream`) under the
// com.wrathmodern.client subsystem, and to stderr, where a debugger or terminal shows it.

#ifndef PLATFORM_MAC_DEBUGOUTPUT_H
#define PLATFORM_MAC_DEBUGOUTPUT_H

namespace MacDebugOutput {

void Write(const char* text);

}

#endif
