// Tiny logging helper. Header-only so every component (including the injected
// DLL) can use it without extra linkage. Writes to stderr and, optionally, to
// a file (used by the hook DLL where stderr is invisible).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace ghost {
namespace log {

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

inline std::string& file_path() {
    static std::string p;
    return p;
}

inline void set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex());
    file_path() = path;
}

inline void write(const char* level, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(mutex());

    char buffer[2048];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);

    std::fprintf(stderr, "[ghost][%s] %s\n", level, buffer);

    if (!file_path().empty()) {
        if (FILE* f = std::fopen(file_path().c_str(), "a")) {
            std::fprintf(f, "[%s] %s\n", level, buffer);
            std::fclose(f);
        }
    }
}

inline void info(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    write("info", fmt, args);
    va_end(args);
}

inline void warn(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    write("warn", fmt, args);
    va_end(args);
}

inline void error(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    write("error", fmt, args);
    va_end(args);
}

} // namespace log
} // namespace ghost
