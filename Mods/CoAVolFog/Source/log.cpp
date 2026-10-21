// log.h of the Windows CoAVolFog, writing Logs/CoAVolFog.log in the game directory

#include "log.h"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace
{

FILE* s_file = nullptr;
int s_level = 1;
std::mutex s_lock;

const char* LevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:
        return "error";
    case LogLevel::Info:
        return "info";
    default:
        return "debug";
    }
}

}

void LogOpen(const char* path)
{
    std::lock_guard<std::mutex> guard(s_lock);

    if (s_file)
        std::fclose(s_file);

    s_file = std::fopen(path, "w");
}

void LogSetLevel(int level)
{
    s_level = level;
}

bool LogEnabled(LogLevel level)
{
    return static_cast<int>(level) <= s_level;
}

void LogWrite(LogLevel level, const char* fmt, ...)
{
    if (!LogEnabled(level))
        return;

    std::lock_guard<std::mutex> guard(s_lock);

    if (!s_file)
        return;

    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    std::fprintf(s_file, "%s [%s] ", stamp, LevelName(level));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(s_file, fmt, args);
    va_end(args);

    std::fputc('\n', s_file);
    std::fflush(s_file);
}
