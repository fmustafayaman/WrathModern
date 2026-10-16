// Crash-report types for the macOS build of Storm's error and machine state logging.
//
// The Windows interface (MachineState.h, SErr) passes SYSTEMTIME, CONTEXT and EXCEPTION_POINTERS.
// On macOS a CONTEXT is the arm64 machine context a signal handler receives, and the exception
// record describes the signal.

#ifndef ENGINE_SOURCE_STORM_SOURCE_MAC_MACHINESTATETYPES_H
#define ENGINE_SOURCE_STORM_SOURCE_MAC_MACHINESTATETYPES_H

#include <cstdint>
#include <sys/ucontext.h>

struct SYSTEMTIME {
    uint16_t wYear;
    uint16_t wMonth;
    uint16_t wDayOfWeek;
    uint16_t wDay;
    uint16_t wHour;
    uint16_t wMinute;
    uint16_t wSecond;
    uint16_t wMilliseconds;
};

typedef struct __darwin_mcontext64 CONTEXT;

struct EXCEPTION_RECORD {
    // Signal number and si_code, the faulting address (si_addr) and the instruction address.
    int32_t m_signal;
    int32_t m_code;
    void* m_faultAddress;
    void* m_instructionAddress;
};

struct EXCEPTION_POINTERS {
    EXCEPTION_RECORD* ExceptionRecord;
    CONTEXT* ContextRecord;
};

void GetLocalTime(SYSTEMTIME* time);

#endif
