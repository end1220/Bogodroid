#ifndef __LOGGING_H__
#define __LOGGING_H__

#include <cstdio>

// BogoDroid logging — single source of truth.
//   fatal_error  always fires (pre-termination).
//   BD_LOG       [BD-cat] line; release-visible unless BOGO_QUIET.
//   BD_DEBUG     [BD-cat] line; NDEBUG-gated.
//   warning / WARN_STUB / BOOT_LOG   NDEBUG-gated legacy paths.
//   verbose      VERBOSE_LOG-gated legacy.

#define fatal_error(msg, ...) \
    do { fprintf(stderr, "%s:%d: " msg, __FILE__, __LINE__, ##__VA_ARGS__); } while(0)

#ifndef NDEBUG
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

#ifdef BOGO_QUIET
    #define BD_LOG(cat, fmt, ...) ((void)0)
#else
    #define BD_LOG(cat, fmt, ...) \
        fprintf(stderr, "[BD-" cat "] " fmt "\n", ##__VA_ARGS__)
#endif

#ifdef VERBOSE_LOG
    #define verbose(tag, format, ...) printf("[" tag "] " format "\n", ##__VA_ARGS__)
#else
    #define verbose(tag, format, ...) ((void)0)
#endif

#endif
