#ifndef __LOGGING_H__
#define __LOGGING_H__

#include <cstdio>
#include <cstdlib>

// BogoDroid logging — single source of truth.
//
//   fatal_error          always fires (pre-termination).
//   SIGSEGV handler      libc backtrace, always fires (debug_utils.cpp).
//
// All other logs are gated under a 2-level hierarchy. The master switch
// must be on for any sub-switch to do anything; CMake enforces this.
//
//   -DBD_ENABLE_LOG=ON                 → BD_LOG, jnivm LOG (event level)
//      -DBD_ENABLE_TRACE=ON            →   + BD_DEBUG, warning, BOOT_LOG
//      -DBD_ENABLE_VERBOSE=ON          →   + verbose
//      -DIL2CPP_TRACE=ON               →   + il2cpp internal trace

#define fatal_error(msg, ...) \
    do { \
        fprintf(stderr, "%s:%d: " msg, __FILE__, __LINE__, ##__VA_ARGS__); \
        fflush(stderr); \
        abort(); \
    } while(0)

#ifdef BD_ENABLE_LOG
    #define BD_LOG(cat, fmt, ...) \
        do { \
            fprintf(stderr, "[BD-" cat "] " fmt "\n", ##__VA_ARGS__); \
            fflush(stderr); \
        } while (0)
#else
    #define BD_LOG(cat, fmt, ...) ((void)0)
#endif

#ifdef BD_ENABLE_TRACE
    #define warning(msg, ...)   do { fprintf(stderr, msg, ##__VA_ARGS__); } while(0)
    #define WARN_STUB           fprintf(stderr, "Warning, stubbed function \"%s\".\n", __FUNCTION__);
    #define BOOT_LOG(...)       printf(__VA_ARGS__)
    #define BD_DEBUG(cat, fmt, ...) \
        fprintf(stderr, "[BD-" cat "] " fmt "\n", ##__VA_ARGS__)
#else
    #define warning(msg, ...)       ((void)0)
    #define WARN_STUB
    #define BOOT_LOG(...)           ((void)0)
    #define BD_DEBUG(cat, fmt, ...) ((void)0)
#endif

#ifdef BD_ENABLE_VERBOSE
    #define verbose(tag, format, ...) printf("[" tag "] " format "\n", ##__VA_ARGS__)
#else
    #define verbose(tag, format, ...) ((void)0)
#endif

#endif
