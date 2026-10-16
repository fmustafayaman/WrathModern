#include "Engine/Source/Storm/H/storm.h"

// Windows looks up TryEnterCriticalSection in kernel32 here, for systems that lacked it; the macOS
// critical sections always support it.

void SServerInitialize() {
}

void SServerDestroy() {
}
