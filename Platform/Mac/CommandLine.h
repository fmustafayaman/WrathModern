// The process command line in the form Windows hands it to a program.
//
// The client parses its command line as one string (GetCommandLineA), with the program name first
// and arguments separated by spaces, quoted when they contain spaces. main() records argv here and
// the macOS platform layer serves that string.

#ifndef PLATFORM_MAC_COMMANDLINE_H
#define PLATFORM_MAC_COMMANDLINE_H

namespace MacCommandLine {

void SetArguments(int argc, char** argv);

// The command line as a single string. Valid after SetArguments, for the life of the process.
const char* Get();

int Count();
const char* Argument(int index);

}

#endif
