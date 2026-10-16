#include "Platform/Mac/DebugOutput.h"
#include <cstdio>
#include <cstring>
#include <os/log.h>

namespace MacDebugOutput {

void Write(const char* text) {
    if (!text || !*text) {
        return;
    }

    static os_log_t log = os_log_create("com.wrathmodern.client", "debug");
    os_log(log, "%{public}s", text);

    fputs(text, stderr);
}

}
