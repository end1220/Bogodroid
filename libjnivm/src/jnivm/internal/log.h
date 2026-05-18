#pragma once
#include <cstdio>
#ifdef BD_ENABLE_LOG
#  if defined(HAVE_LOGGER)
#    include <log.h>
#    define LOG(...) Log::debug(__VA_ARGS__)
#  else
// stderr (unbuffered in main.cpp) so the tail survives SIGKILL.
#    define LOG(tag, format, ...) fprintf(stderr, "[" tag "]: " format "\n" , ##__VA_ARGS__)
#  endif
#else
#  define LOG(tag, format, ...) ((void)0)
#endif

// Prints each unique (kind, class, name, sig) tuple via LOG once.
namespace jnivm {
    void log_stub_miss_once(const char* kind, const char* cls, const char* meth, const char* sig);
}