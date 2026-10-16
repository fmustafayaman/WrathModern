#include "Platform/Mac/CommandLine.h"
#include <cstring>
#include <string>

namespace MacCommandLine {

static std::string s_commandLine;
static int s_argc;
static char** s_argv;

void SetArguments(int argc, char** argv) {
    s_argc = argc;
    s_argv = argv;
    s_commandLine.clear();

    for (int i = 0; i < argc; i++) {
        if (i) {
            s_commandLine.push_back(' ');
        }

        const char* arg = argv[i];
        bool quote = !*arg || strpbrk(arg, " \t");

        if (quote) {
            s_commandLine.push_back('"');
        }

        s_commandLine.append(arg);

        if (quote) {
            s_commandLine.push_back('"');
        }
    }
}

const char* Get() {
    return s_commandLine.c_str();
}

int Count() {
    return s_argc;
}

const char* Argument(int index) {
    return index >= 0 && index < s_argc ? s_argv[index] : nullptr;
}

}
