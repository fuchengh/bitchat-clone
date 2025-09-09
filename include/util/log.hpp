#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>

namespace bitchat
{

enum class Level
{
    Debug   = 0,
    Info    = 1,
    Warning = 2,
    Error   = 3
};

inline Level &global_level()
{
    static Level lv = Level::Debug;  // TODO: change to info later
    return lv;
}

inline void set_log_level(Level lv)
{
    global_level() = lv;
}

// Allow callers to pass string literals or other non-owning strings.
inline void set_log_level_by_name(std::string_view name)
{
    if (name == "debug")
        set_log_level(Level::Debug);
    else if (name == "info")
        set_log_level(Level::Info);
    else if (name == "warn" || name == "warning")
        set_log_level(Level::Warning);
    else if (name == "error")
        set_log_level(Level::Error);
}

inline const char *level_name(Level lv)
{
    switch (lv)
    {
        case Level::Debug:
            return "[DEBUG]";
        case Level::Info:
            return "[INFO]";
        case Level::Warning:
            return "[WARN]";
        case Level::Error:
            return "[ERROR]";
    }
    return "?";
}

inline void timestamp(char *buf, size_t n)
{
    using namespace std::chrono;
    const auto  now = system_clock::now();
    const auto  ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t tt  = system_clock::to_time_t(now);
    std::tm     tm{};
    localtime_r(&tt, &tm);
    std::snprintf(buf, n, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  (int)ms.count());
}

inline void logf(Level lv, const char *func, const char *fmt, ...)
{
    if ((int)lv < (int)global_level())
        return;

    char ts[16];
    timestamp(ts, sizeof(ts));

    std::fprintf(stderr, "%s| %-7s| %s: ", ts, level_name(lv), func ? func : "?");

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);

    size_t m = std::strlen(fmt);
    if (m == 0 || fmt[m - 1] != '\n')
        std::fputc('\n', stderr);
}

#define LOG_DEBUG(...) ::bitchat::logf(::bitchat::Level::Debug, __func__, __VA_ARGS__)
#define LOG_INFO(...) ::bitchat::logf(::bitchat::Level::Info, __func__, __VA_ARGS__)
#define LOG_WARN(...) ::bitchat::logf(::bitchat::Level::Warning, __func__, __VA_ARGS__)
#define LOG_ERROR(...) ::bitchat::logf(::bitchat::Level::Error, __func__, __VA_ARGS__)

}  // namespace bitchat
