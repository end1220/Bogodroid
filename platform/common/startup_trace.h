#pragma once
#include "logging.h"
#include <chrono>

// One monotonic origin shared by loader and render threads. LOG=OFF removes
// measurement work. Durations may nest; do not sum nested scopes.
namespace bd_startup {
#ifdef BD_ENABLE_LOG
inline std::chrono::steady_clock::time_point origin;
inline void init() { origin = std::chrono::steady_clock::now(); }
inline double ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - origin).count();
}
inline void mark(const char* label) { BD_LOG("STARTUP", "+%.3fms %s", ms(), label); }
class Scope {
    const char* label_;
    double start_;
public:
    explicit Scope(const char* label) : label_(label), start_(ms()) {
        BD_LOG("STARTUP", "+%.3fms begin %s", start_, label_);
    }
    ~Scope() {
        const double end = ms();
        BD_LOG("STARTUP", "+%.3fms end %s duration=%.3fms", end, label_, end - start_);
    }
};
#else
inline void init() {}
inline void mark(const char*) {}
class Scope { public: explicit Scope(const char*) {} };
#endif
}
