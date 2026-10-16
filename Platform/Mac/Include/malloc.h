// MSVC's <malloc.h> declares malloc, free and _alloca. macOS keeps them in <stdlib.h> and <alloca.h>.
#pragma once
#include <alloca.h>
#include <stdlib.h>
