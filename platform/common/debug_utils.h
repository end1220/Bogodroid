#pragma once

#include "anative_activity.h"
void print_native_callbacks(ANativeActivity nActivity);
void print_backtrace_on_segfault();
void exit_on_signals();

// Prints backtrace() frames plus the hand-rolled frame-pointer chain and stack
// scan as "module+offset". Defined in thunks/libc/misc.cpp, which shares the
// unwinder with Bionic's abort-message hook. Safe to call from a crash handler.
extern "C" void bd_dump_crash_backtrace(const char* tag);

// Same address -> "module+offset" resolution for a single raw address, used by
// the SIGSEGV handler to name the pc/lr/fp it reads out of the signal context.
extern "C" void bd_describe_crash_address(const char* tag, const char* label,
                                          unsigned long long addr);

#include "process_memory.h"
