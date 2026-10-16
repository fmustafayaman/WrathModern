// SSE intrinsics on arm64 come from sse2neon, which maps them onto NEON.
#pragma once
#if defined(__aarch64__)
#include "../../../ThirdParty/sse2neon/sse2neon.h"
#else
#include_next <xmmintrin.h>
#endif
