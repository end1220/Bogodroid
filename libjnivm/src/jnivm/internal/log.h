#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Routine JNI traffic includes several calls per rendered frame. Keep it out
// of normal handheld logs while retaining diagnostics that identify missing
// compatibility stubs. Set BD_JNI_TRACE=1 before launching to restore the
// complete libjnivm trace for focused debugging.
inline bool jnivm_log_enabled(const char* tag, const char* format)
{
    static const bool trace = [] {
        const char* value = std::getenv("BD_JNI_TRACE");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (trace)
        return true;
    if (tag && std::strncmp(tag, "BD-", 3) == 0)
        return true;
    if (!format)
        return false;
    static const char* diagnostics[] = {
        "STUB-MISS", "Exception", "Fatal", "failed", "Failed",
        " is null", " unsupported", " unknown", "Unknown",
        "Not Implemented", "Unimplemented"
    };
    for (const char* marker : diagnostics) {
        if (std::strstr(format, marker))
            return true;
    }
    return false;
}

#ifdef BD_ENABLE_LOG
#  if defined(HAVE_LOGGER)
#    include <log.h>
#    define LOG(...) Log::debug(__VA_ARGS__)
#  else
// stderr (unbuffered in main.cpp) so the tail survives SIGKILL.
#    define LOG(tag, format, ...) \
        do { \
            if (jnivm_log_enabled(tag, format)) \
                fprintf(stderr, "[" tag "]: " format "\n", ##__VA_ARGS__); \
        } while (0)
#  endif
#else
#  define LOG(tag, format, ...) ((void)0)
#endif

// Prints each unique (kind, class, name, sig) tuple via LOG once.
namespace jnivm {
    void log_stub_miss_once(const char* kind, const char* cls, const char* meth, const char* sig);
}
