#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/MachineState.h"
#include "Platform/Mac/DebugOutput.h"
#include "Platform/Mac/Paths.h"
#include "Platform/Mac/PEResources.h"

// Storm error reporting on macOS, following the Windows version: errors are logged with the machine
// state to Errors/ in the client directory, then shown in an alert; fatal signals take the place of
// unhandled exceptions. Error and dialog strings come from Wow.exe's string and message tables, as
// LoadString and FormatMessage give them to the Windows client.

#define STORM_FACILITY 0x510
#define SERR_STRING_BASE 0x5100

#define SERR_STR_ERROR 0
#define SERR_STR_CRITICAL 1
#define SERR_STR_PROGRAM 2
#define SERR_STR_FILE_LINE 3
#define SERR_STR_FUNCTION 4
#define SERR_STR_OBJECT 5
#define SERR_STR_HANDLE 6
#define SERR_STR_EXPRESSION 7
#define SERR_STR_DESCRIPTION 8
#define SERR_STR_TERMINATE 9
#define SERR_STR_KEEP_RUNNING 10
#define SERR_STR_FILE 11
#define SERR_STR_BREAK 12
#define SERR_STR_BREAK_OR_RUN 13
#define SERR_STR_EXCEPTION 14
#define SERR_STR_ACCESS_VIOLATION 15
#define SERR_STR_READ 16
#define SERR_STR_WRITTEN 17

#define SERR_ERROR_INVALID_PARAMETER 87

struct APPFATINFO {
    const char* filename;
    int32_t linenumber;
    uint32_t threadId;
};

struct HANDLER : TSLinkedNode<HANDLER> {
    int32_t (STORMAPI* handler)(uint32_t, const char*, const char*, int32_t, const char*);
};

static const char* const s_displaystr[18] = {
    "ERROR #%u (0x%08x) %s",
    "This application has encountered a critical error:\n\n%s\n",
    "Program:\t%s\n",
    "File:\t%s\nLine:\t%d\n",
    "Function:\t%s\n",
    "Object:\t%s\n",
    "Handle:\t%s\n",
    "Expr:\t%s\n\n",
    "\n%s\n\n",
    "Press OK to terminate the application.",
    "Do you wish to keep running anyway?\n\tOK:\tKeep running\n\tCancel:\tTerminate application",
    "File:\t%s\n",
    "Do you wish to break to the debugger?\n\tYes:\tBreak to debugger\n\tNo:\tTerminate application",
    "Do you wish to break to the debugger?\n\tYes:\tBreak to debugger\n\tNo:\tTerminate application\n\tCancel:\tKeep running",
    "Exception:\t%s\n",
    "The instruction at \"0x%p\" referenced memory at \"0x%p\".\nThe memory could not be \"%s\".",
    "read",
    "written",
};

static pthread_mutex_t s_critsect = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pthread_mutex_t s_exceptioncritsect = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static TSList<HANDLER> s_handlerlist;
static EXCEPTION_POINTERS* s_exceptionPointers;
static int32_t s_displaying;
static APPFATINFO s_appFatInfo;
static uint32_t s_lasterror;
static char s_logtitle[128];
static int32_t (STORMAPI* s_logCallback)(char*, uint32_t);
static uint32_t s_noMiniDumps;
static char s_logLastFile[MAX_PATH];

static const char* GetErrorString(uint32_t id) {
    static char buffer[256];

    if (!MacPEResources::LoadString(id + SERR_STRING_BASE, buffer, sizeof(buffer))) {
        return s_displaystr[id];
    }

    return buffer;
}

void SErrInitialize() {
    g_opt.serrleaksilentwarning = 1;
    SRegLoadValue("Internal", "No Minidumps", 0, &s_noMiniDumps);
}

static int32_t CanBreakToDebugger() {
    int32_t mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    kinfo_proc info = {};
    size_t size = sizeof(info);

    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) {
        return 0;
    }

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

static void WriteText(int32_t fd, const char* text, size_t length) {
    while (length) {
        ssize_t written = write(fd, text, length);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return;
        }

        text += written;
        length -= static_cast<size_t>(written);
    }
}

static void WriteLine(void* param, const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 2, format ? format : "", args);
    va_end(args);
    buffer[sizeof(buffer) - 2] = 0;
    SStrPack(buffer, "\n", sizeof(buffer));

    WriteText(static_cast<int32_t>(reinterpret_cast<intptr_t>(param)), buffer, SStrLen(buffer));
}

static void WriteMessageToLog(int32_t logfile, const char* message) {
    size_t length = SStrLen(message);
    WriteText(logfile, "\n", 1);
    WriteText(logfile, message, length);

    if (!length || message[length - 1] != '\n') {
        WriteText(logfile, "\n", 1);
    }
}

static int32_t CreateErrorLogFile(const char* suffix, const char* ext, char* logpath, uint32_t logpathchars, const SYSTEMTIME* time) {
    if (!suffix) {
        suffix = " Log";
    }

    if (!MacPaths::ClientDirectory(logpath, logpathchars)) {
        logpath[0] = 0;
    }

    size_t directoryLength = SStrLen(logpath);
    SStrPack(logpath, "Errors/", logpathchars);

    struct stat info;

    if (mkdir(logpath, 0755) != 0 && !(stat(logpath, &info) == 0 && S_ISDIR(info.st_mode))) {
        logpath[directoryLength] = 0;
    }

    char logfilename[MAX_PATH];
    snprintf(logfilename, sizeof(logfilename), "%04d-%02d-%02d %02d.%02d.%02d %s.%s", time->wYear, time->wMonth, time->wDay, time->wHour, time->wMinute, time->wSecond, suffix, ext);
    SStrPack(logpath, logfilename, logpathchars);

    return open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

static int32_t CreateErrorLog(const char* suffix, const char* message, const SYSTEMTIME* time) {
    char logpath[MAX_PATH];
    SYSTEMTIME localTime;

    if (!time) {
        GetLocalTime(&localTime);
        time = &localTime;
    }

    int32_t logfile = CreateErrorLogFile(suffix, "txt", logpath, sizeof(logpath), time);

    if (logfile < 0) {
        return logfile;
    }

    void* param = reinterpret_cast<void*>(static_cast<intptr_t>(logfile));
    LogComputerInfoHeader(LOG_OPTION_DEFAULT, WriteLine, param, s_logtitle, time);
    WriteMessageToLog(logfile, message);

    if (s_logCallback) {
        char* buffer = static_cast<char*>(alloca(5200));
        buffer[0] = 0;
        s_logCallback(buffer, 5200);

        if (strlen(buffer)) {
            WriteMessageToLog(logfile, buffer);
        }
    }

    SStrCopy(s_logLastFile, logpath, sizeof(s_logLastFile));

    return logfile;
}

static const char* SErrGetErrorText(uint32_t errorcode) {
    switch (errorcode) {
        case STORM_ERROR_ASSERTION:
            return "Assertion Failure";
        case STORM_ERROR_NOT_ARCHIVE:
            return "Not Archive";
        case STORM_ERROR_VERSION_MISMATCH:
            return "Version Mismatch";
        case STORM_ERROR_MEM_NOT_ALLOCATED:
            return "Memory Already Freed";
        case STORM_ERROR_MEM_CORRUPTED:
            return "Memory Corrupt";
        case STORM_ERROR_MEM_INVALID:
            return "Memory Invalid Block";
        case STORM_ERROR_MEM_NOT_FREED:
            return "Memory Never Released";
        case STORM_ERROR_RESOURCES_NOT_RELEASED:
            return "Handle Never Released";
        case STORM_ERROR_OUT_OF_BOUNDS:
            return "Access Out Of Bounds";
        case STORM_ERROR_FILE_CORRUPTED:
            return "File Corrupt";
        case STORM_ERROR_FATAL:
            return "Fatal Exception";
        case STORM_ERROR_FATAL_CONDITION:
            return "Fatal Condition";
        default:
            return "";
    }
}

static const char* SignalName(int32_t signal) {
    switch (signal) {
        case SIGSEGV:
            return "ACCESS_VIOLATION";
        case SIGBUS:
            return "BUS_ERROR";
        case SIGILL:
            return "ILLEGAL_INSTRUCTION";
        case SIGFPE:
            return "FLOATING_POINT_EXCEPTION";
        case SIGTRAP:
            return "BREAKPOINT";
        case SIGABRT:
            return "ABORT";
        case SIGSYS:
            return "BAD_SYSTEM_CALL";
        default:
            return "unknown exception";
    }
}

uint32_t STORMAPI SErrGetLastError() {
    return s_lasterror;
}

int32_t STORMAPI SErrIsDisplayingError() {
    return s_displaying;
}

void STORMAPI SErrPrepareAppFatal(const char* filename, int32_t linenumber) {
    pthread_mutex_lock(&s_critsect);
    s_appFatInfo.filename = filename;
    s_appFatInfo.linenumber = linenumber;
    s_appFatInfo.threadId = SGetCurrentThreadId();
    pthread_mutex_unlock(&s_critsect);
}

void STORMAPI SErrSetLastError(uint32_t errorcode) {
    s_lasterror = errorcode;
}

void STORMAPI SErrSetLogTitleString(const char* title) {
    pthread_mutex_lock(&s_critsect);
    SStrCopy(s_logtitle, title, sizeof(s_logtitle));
    pthread_mutex_unlock(&s_critsect);
}

void STORMAPI SErrSetLogCallback(int32_t (STORMAPI* callback)(char*, uint32_t)) {
    pthread_mutex_lock(&s_critsect);
    s_logCallback = callback;
    pthread_mutex_unlock(&s_critsect);
}

int32_t STORMAPI SErrGetLogLastPath(char* buf, uint32_t size) {
    pthread_mutex_lock(&s_critsect);
    int32_t result = 0;

    if (s_logLastFile[0]) {
        SStrCopy(buf, s_logLastFile, size);
        result = 1;
    }

    pthread_mutex_unlock(&s_critsect);

    return result;
}

static int32_t UndecorateObjectName(const char* source, char* dest, uint32_t destchars) {
    if (SStrLen(source) < 6 || source[0] != '.' || source[3] != 'U' || !strpbrk(source + 4, "?@")) {
        return 0;
    }

    SStrCopy(dest, source + 4, destchars);

    char* end = strpbrk(dest, "?@");

    if (!end) {
        return 0;
    }

    *end = 0;

    for (char* c = end - 1; c >= dest && *c == '_'; --c) {
        *c = 0;
    }

    SStrPack(dest, " (", destchars);
    SStrPack(dest, source, destchars);
    SStrPack(dest, ")", destchars);

    return 1;
}

int32_t STORMAPI SErrGetErrorStr(uint32_t errorcode, char* buffer, uint32_t bufferchars) {
    if (!buffer || !bufferchars) {
        SErrSetLastError(SERR_ERROR_INVALID_PARAMETER);
        return 0;
    }

    *buffer = 0;

    if (errorcode == 108) {
        SStrPrintf(buffer, bufferchars, "%s", "Invalid or corrupt archive.\n");
        return 1;
    }

    // Storm's own facility and the DirectDraw and DirectSound ones resolve to the client's message
    // table on Windows; system error codes have no text on macOS.
    return MacPEResources::FormatMessage(errorcode, buffer, bufferchars) != 0;
}

void STORMAPI SErrRegisterHandler(int32_t (STORMAPI* handler)(uint32_t, const char*, const char*, int32_t, const char*)) {
    pthread_mutex_lock(&s_critsect);
    s_handlerlist.NewNode(1, 0, 0x8000000)->handler = handler;
    pthread_mutex_unlock(&s_critsect);
}

int32_t STORMAPI SErrDestroy() {
    s_handlerlist.Clear();

    return 1;
}

// The alert answers, in Win32 MessageBox terms.
enum AlertResult {
    ALERT_OK,
    ALERT_CANCEL,
    ALERT_YES,
    ALERT_NO,
};

enum AlertButtons {
    ALERT_BUTTONS_OK,
    ALERT_BUTTONS_OK_CANCEL,
    ALERT_BUTTONS_YES_NO,
    ALERT_BUTTONS_YES_NO_CANCEL,
};

static AlertResult ShowAlert(const char* title, const char* text, AlertButtons buttons, bool error) {
    CFStringRef cfTitle = CFStringCreateWithCString(kCFAllocatorDefault, title, kCFStringEncodingUTF8);
    CFStringRef cfText = CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);

    if (!cfText) {
        cfText = CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingWindowsLatin1);
    }

    CFStringRef defaultButton = nullptr;
    CFStringRef alternateButton = nullptr;
    CFStringRef otherButton = nullptr;
    AlertResult results[3];

    switch (buttons) {
        case ALERT_BUTTONS_OK:
            defaultButton = CFSTR("OK");
            results[0] = ALERT_OK;
            break;
        case ALERT_BUTTONS_OK_CANCEL:
            defaultButton = CFSTR("Cancel");
            alternateButton = CFSTR("OK");
            results[0] = ALERT_CANCEL;
            results[1] = ALERT_OK;
            break;
        case ALERT_BUTTONS_YES_NO:
            defaultButton = CFSTR("No");
            alternateButton = CFSTR("Yes");
            results[0] = ALERT_NO;
            results[1] = ALERT_YES;
            break;
        case ALERT_BUTTONS_YES_NO_CANCEL:
            defaultButton = CFSTR("No");
            alternateButton = CFSTR("Yes");
            otherButton = CFSTR("Cancel");
            results[0] = ALERT_NO;
            results[1] = ALERT_YES;
            results[2] = ALERT_CANCEL;
            break;
    }

    CFOptionFlags response = kCFUserNotificationDefaultResponse;
    CFUserNotificationDisplayAlert(0, error ? kCFUserNotificationStopAlertLevel : kCFUserNotificationCautionAlertLevel, nullptr, nullptr, nullptr, cfTitle ? cfTitle : CFSTR("Error"), cfText ? cfText : CFSTR(""), defaultButton, alternateButton, otherButton, &response);

    if (cfTitle) {
        CFRelease(cfTitle);
    }

    if (cfText) {
        CFRelease(cfText);
    }

    switch (response & 0x3) {
        case kCFUserNotificationAlternateResponse:
            return results[1];
        case kCFUserNotificationOtherResponse:
            return results[2];
        default:
            return results[0];
    }
}

int32_t STORMAPI SErrDisplayError(uint32_t errorcode, const char* filename, int32_t linenumber, const char* description, int32_t recoverable, uint32_t exitcode, uint32_t unknown) {
    static int32_t lastlinenumber;
    static const char* lastfilename;

    if (g_opt.unk_0028) {
        if (filename == lastfilename && linenumber == lastlinenumber) {
            return 1;
        }

        lastfilename = filename;
        lastlinenumber = linenumber;
    }

    if (s_displaying) {
        return 0;
    }

    s_displaying = 1;

    static char appfilename[MAX_PATH];
    static char appname[MAX_PATH];

    if (!MacPaths::ExecutablePath(appfilename, sizeof(appfilename))) {
        appfilename[0] = 0;
    }

    const char* leaf = strrchr(appfilename, '/');
    SStrCopy(appname, leaf ? leaf + 1 : appfilename, sizeof(appname));

    static char errorstr[256];
    errorstr[0] = 0;
    SErrGetErrorStr(errorcode, errorstr, sizeof(errorstr));

    if (!errorstr[0]) {
        SStrPrintf(errorstr, sizeof(errorstr), GetErrorString(SERR_STR_ERROR), errorcode, errorcode, SErrGetErrorText(errorcode));
    }

    const char* localnameptr = filename;
    static char localnamebuffer[256];

    if (localnameptr && *localnameptr && (linenumber == SLOG_OBJECT || linenumber == SLOG_HANDLE) && UndecorateObjectName(localnameptr, localnamebuffer, sizeof(localnamebuffer))) {
        localnameptr = localnamebuffer;
    }

    static char outstr[2048];
    char* outptr = outstr;
    char* outend = outstr + sizeof(outstr);

    if (localnameptr) {
        outptr += SStrCopy(outptr, localnameptr, outend - outptr);
    }

    if (linenumber > 0) {
        SStrPrintf(outptr, outend - outptr, "(%u)", linenumber);
        outptr += SStrLen(outptr);
    }

    SStrPrintf(outptr, outend - outptr, " : error %u: ", errorcode);
    outptr += SStrLen(outptr);
    SStrCopy(outptr, errorstr, outend - outptr);
    SDebugPrintf("%s\n", outstr);

    outptr = outstr;
    SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_CRITICAL), errorstr);
    outptr += SStrLen(outptr);
    SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_PROGRAM), appfilename);
    outptr += SStrLen(outptr);

    if (localnameptr && *localnameptr) {
        switch (linenumber) {
            case SLOG_EXCEPTION:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_EXCEPTION), localnameptr);
                break;
            case SLOG_FILE:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_FILE), localnameptr);
                break;
            case SLOG_HANDLE:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_HANDLE), localnameptr);
                break;
            case SLOG_OBJECT:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_OBJECT), localnameptr);
                break;
            case SLOG_FUNCTION:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_FUNCTION), localnameptr);
                break;
            default:
                SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_FILE_LINE), localnameptr, linenumber);
                break;
        }

        outptr += SStrLen(outptr);
    }

    if (errorcode == STORM_ERROR_ASSERTION) {
        SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_EXPRESSION), description ? description : "");
    } else {
        SStrPrintf(outptr, outend - outptr, GetErrorString(SERR_STR_DESCRIPTION), description ? description : "");
    }

    outptr += SStrLen(outptr);

    if (!g_opt.serrsuppresslogs) {
        LoadMachineStateSymbols();

        pthread_mutex_lock(&s_exceptioncritsect);
        EXCEPTION_POINTERS* exceptionPointers = s_exceptionPointers;
        s_exceptionPointers = nullptr;
        pthread_mutex_unlock(&s_exceptioncritsect);

        CONTEXT* context = exceptionPointers ? exceptionPointers->ContextRecord : nullptr;

        SYSTEMTIME time;
        GetLocalTime(&time);

        const char* suffix = context ? "Crash" : "Error";
        int32_t logfile = CreateErrorLog(suffix, outstr, &time);

        if (logfile >= 0) {
            LogMachineState(LOG_OPTION_DEFAULT | LOG_OPTION_FRAMEPARAMS, WriteLine, reinterpret_cast<void*>(static_cast<intptr_t>(logfile)), context ? 0 : 2, context);
            close(logfile);
        }

        char* details = SStrStr(outstr, "Debug Details:");

        if (details) {
            outptr = details;
        }

        UnloadMachineStateSymbols();
    }

    if (g_opt.unk_0028) {
        s_displaying = 0;
        return 1;
    }

    AlertButtons buttons;
    bool error;
    AlertResult keeprunning;
    AlertResult breakresult;
    bool canKeepRunning;
    bool canBreak;

    if (!CanBreakToDebugger()) {
        canBreak = false;

        if (recoverable) {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_KEEP_RUNNING), outend - outptr);
            buttons = ALERT_BUTTONS_OK_CANCEL;
            error = false;
            keeprunning = ALERT_OK;
            canKeepRunning = true;
        } else {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_TERMINATE), outend - outptr);
            buttons = ALERT_BUTTONS_OK;
            error = true;
            keeprunning = ALERT_OK;
            canKeepRunning = false;
        }

        breakresult = ALERT_YES;
    } else {
        canBreak = true;
        breakresult = ALERT_YES;

        if (!recoverable) {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_BREAK), outend - outptr);
            buttons = ALERT_BUTTONS_YES_NO;
            error = true;
            keeprunning = ALERT_CANCEL;
            canKeepRunning = false;
        } else {
            outptr += SStrCopy(outptr, GetErrorString(SERR_STR_BREAK_OR_RUN), outend - outptr);
            buttons = ALERT_BUTTONS_YES_NO_CANCEL;
            error = false;
            keeprunning = ALERT_CANCEL;
            canKeepRunning = true;
        }
    }

    pthread_mutex_lock(&s_critsect);

    // A registered handler that returns 0 takes over the display; without one, the alert shows.
    bool handled = false;
    AlertResult result = canKeepRunning ? keeprunning : ALERT_NO;

    for (HANDLER* handler = s_handlerlist.Head(); handler; handler = s_handlerlist.Next(handler)) {
        if (!handler->handler(errorcode, errorstr, localnameptr, linenumber, description ? description : "")) {
            handled = true;
            break;
        }
    }

    if (!handled) {
        result = ShowAlert(appname, outstr, buttons, error);
    }

    pthread_mutex_unlock(&s_critsect);

    if (canBreak && result == breakresult) {
        __builtin_debugtrap();
        s_displaying = 0;
        return 1;
    }

    if (canKeepRunning && result == keeprunning) {
        s_displaying = 0;
        return 1;
    }

    _exit(static_cast<int32_t>(exitcode));
}

void STORMAPI SErrReportNamedResourceLeak(const char* handlename, const char* resourcename) {
    static int32_t checked;
    static uint32_t debugmode;

    if (!checked) {
        checked = 1;
        SRegLoadValue("Internal", "Debug Memory", 0, &debugmode);
    }

    if (!debugmode) {
        return;
    }

    char errormessage[256];

    if (resourcename && *resourcename) {
        SStrPrintf(errormessage, sizeof(errormessage), "%s (%s)", handlename, resourcename);
    } else {
        SStrPrintf(errormessage, sizeof(errormessage), "%s", handlename);
    }

    if (g_opt.serrleaksilentwarning) {
        char szMessage[200];
        SStrPrintf(szMessage, sizeof(szMessage), "Storm Error : handle never released -- %s\n", errormessage);
        MacDebugOutput::Write(szMessage);
    } else {
        SErrDisplayError(STORM_ERROR_RESOURCES_NOT_RELEASED, errormessage, SLOG_HANDLE, nullptr, 1, 1, 0x11111111);
    }
}

// Fatal signals stand in for Win32 unhandled exceptions. The handler runs on its own stack so a stack
// overflow can still be reported, logs and shows the error like ExceptionFilterWin32, and then lets
// the default action run so macOS writes its crash report too.

static const int32_t kFatalSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS };

static void FatalSignalHandler(int32_t signal, siginfo_t* info, void* contextParam) {
    ucontext_t* ucontext = static_cast<ucontext_t*>(contextParam);

    static EXCEPTION_RECORD record;
    static EXCEPTION_POINTERS pointers;
    record.m_signal = signal;
    record.m_code = info ? info->si_code : 0;
    record.m_faultAddress = info ? info->si_addr : nullptr;
    record.m_instructionAddress = ucontext ? reinterpret_cast<void*>(__darwin_arm_thread_state64_get_pc(ucontext->uc_mcontext->__ss)) : nullptr;
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = ucontext ? ucontext->uc_mcontext : nullptr;

    pthread_mutex_lock(&s_exceptioncritsect);

    if (!s_exceptionPointers) {
        s_exceptionPointers = &pointers;
    }

    pthread_mutex_unlock(&s_exceptioncritsect);

    char buffer[120];
    char message[256];
    message[0] = 0;

    SStrPrintf(buffer, sizeof(buffer), "%s (signal %d, code %d) at %p", SignalName(signal), signal, record.m_code, record.m_instructionAddress);

    if (signal == SIGSEGV || signal == SIGBUS) {
        // ESR bit 6 (WnR) tells a faulting write from a read for data aborts.
        bool written = pointers.ContextRecord && (pointers.ContextRecord->__es.__esr & 0x40);
        char format[256];
        SStrCopy(format, GetErrorString(SERR_STR_ACCESS_VIOLATION), sizeof(format));
        SStrPrintf(message, sizeof(message), format, record.m_instructionAddress, record.m_faultAddress, GetErrorString(written ? SERR_STR_WRITTEN : SERR_STR_READ));
    }

    // SErrDisplayError terminates for a non-recoverable error; restore the default action in case a
    // handler takes over and returns.
    SErrDisplayError(STORM_ERROR_FATAL, buffer, SLOG_EXCEPTION, message, 0, 1, 0x11111111);

    ::signal(signal, SIG_DFL);
}

void SErrDisplayAppFatalCustomV(uint32_t errorcode, const char* format, va_list args) {
    const char* filename = nullptr;
    int32_t linenumber = 0;

    pthread_mutex_lock(&s_critsect);

    if (s_appFatInfo.threadId == SGetCurrentThreadId()) {
        filename = s_appFatInfo.filename;
        linenumber = s_appFatInfo.linenumber;
        s_appFatInfo.filename = nullptr;
        s_appFatInfo.linenumber = 0;
        s_appFatInfo.threadId = 0;
    }

    pthread_mutex_unlock(&s_critsect);

    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    buffer[sizeof(buffer) - 1] = 0;
    SErrDisplayError(errorcode, filename, linenumber, buffer, 0, 1, 0x11111111);
    StormExitProcess(1);
}

void SErrDisplayAppFatalCustom(uint32_t errorcode, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(errorcode, format, args);
}

void SErrDisplayAppFatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(STORM_ERROR_FATAL_CONDITION, format, args);
}

int32_t SErrDisplayErrorFmt(uint32_t errorcode, const char* filename, int32_t linenumber, int32_t recoverable, uint32_t exitcode, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;

    return SErrDisplayError(errorcode, filename, linenumber, buffer, recoverable, exitcode, 0x11111111);
}

void STORMAPI SErrCatchUnhandledExceptions() {
    static uint8_t alternateStack[256 * 1024];

    stack_t stack = {};
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof(alternateStack);
    sigaltstack(&stack, nullptr);

    struct sigaction action = {};
    action.sa_sigaction = FatalSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);

    for (int32_t signal : kFatalSignals) {
        sigaction(signal, &action, nullptr);
    }
}

void SErrDisplayAppFatalMessage(const char* message, const char* filename, int32_t linenumber) {
    SErrPrepareAppFatal(filename, linenumber);
    SErrDisplayAppFatal("%s", message);
}
