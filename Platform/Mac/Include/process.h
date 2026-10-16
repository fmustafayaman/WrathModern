// MSVC <process.h>: process control (_exit, getpid) lives in <unistd.h> on macOS.
#pragma once
#include <stdlib.h>
#include <unistd.h>
