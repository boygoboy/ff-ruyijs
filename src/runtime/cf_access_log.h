// Diagnostic property-access logging for challenge research (RUYIJS_CF_ACCESS_LOG).
// Off by default; when the env var names a file, every logged host-object read is
// appended there so the challenge VM's observed surface can be enumerated and
// compared against a real browser.
#ifndef RUYIJS_CF_ACCESS_LOG_H
#define RUYIJS_CF_ACCESS_LOG_H
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ruyijs::cf_log {
inline FILE* handle() {
    static FILE* file = [] {
        const char* path = std::getenv("RUYIJS_CF_ACCESS_LOG");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}
inline void access(const char* object, const std::string& property) {
    if (FILE* file = handle()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(file, "%lld %s %s\n", static_cast<long long>(ms), object, property.c_str());
        std::fflush(file);
    }
}
inline void access(const char* object, unsigned long field) {
    access(object, std::to_string(field));
}
}
#endif
