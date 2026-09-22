#include "debug_utils.h"

#include "gles2.h"
#include "glad.h"
#include "glad_egl.h"
#include "android.h" // for SharedPreferences::flush_all
#include "logging.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <execinfo.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "toml++/toml.hpp"

extern toml::table config;

static long bd_read_status_kb(const char* key)
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    const size_t key_len = strlen(key);
    long value = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            if (sscanf(line + key_len + 1, "%ld", &value) == 1)
                break;
        }
    }
    fclose(f);
    return value;
}

static long bd_read_meminfo_kb(const char* key)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[256];
    const size_t key_len = strlen(key);
    long value = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            if (sscanf(line + key_len + 1, "%ld", &value) == 1)
                break;
        }
    }
    fclose(f);
    return value;
}

void bd_log_process_memory(const char* why)
{
    const long rss = bd_read_status_kb("VmRSS");
    const long hwm = bd_read_status_kb("VmHWM");
    const long size = bd_read_status_kb("VmSize");
    const long avail = bd_read_meminfo_kb("MemAvailable");
    const long total = bd_read_meminfo_kb("MemTotal");
    BD_LOG("MEM", "pid=%d tid=%d rss=%.1fMB hwm=%.1fMB vsz=%.1fMB sys_avail=%.1fMB/%ldMB%s%s",
           (int)getpid(), (int)syscall(SYS_gettid),
           rss >= 0 ? rss / 1024.0 : -1.0,
           hwm >= 0 ? hwm / 1024.0 : -1.0,
           size >= 0 ? size / 1024.0 : -1.0,
           avail >= 0 ? avail / 1024.0 : -1.0,
           total >= 0 ? total / 1024 : -1L,
           why && *why ? " @ " : "",
           why && *why ? why : "");
}

int bd_log_process_memory_throttled(const char* why, int interval_ms)
{
    if (interval_ms <= 0)
        return 0;
    static struct timespec last = {0, 0};
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    const long long elapsed_ms =
        (long long)(now.tv_sec - last.tv_sec) * 1000LL +
        (now.tv_nsec - last.tv_nsec) / 1000000LL;
    if (last.tv_sec != 0 && elapsed_ms < interval_ms)
        return 0;
    last = now;
    bd_log_process_memory(why);
    return 1;
}

int bd_mem_log_interval_ms()
{
    // Resolved once, on the first swap (TOML is loaded and plugins had their
    // chance to setenv by then). Thread-safe magic static.
    static const int interval = [] {
        const char* env = getenv("BD_MEM_LOG_MS");
        if (env && *env)
            return atoi(env);
        // Omitted key or omitted [debug] table -> 2000 (toml++ value_or).
        return config["debug"]["mem_log_interval_ms"].value_or<int>(2000);
    }();
    return interval;
}

void print_native_callbacks(ANativeActivity nActivity)
{
#ifndef NDEBUG
    printf("onStart: %p\n", (void*)nActivity.callbacks->onStart);
    printf("onResume: %p\n", (void*)nActivity.callbacks->onResume);
    printf("onSaveInstanceState: %p\n", (void*)nActivity.callbacks->onSaveInstanceState);
    printf("onPause: %p\n", (void*)nActivity.callbacks->onPause);
    printf("onStop: %p\n", (void*)nActivity.callbacks->onStop);
    printf("onDestroy: %p\n", (void*)nActivity.callbacks->onDestroy);
    printf("onWindowFocusChanged: %p\n", (void*)nActivity.callbacks->onWindowFocusChanged);
    printf("onNativeWindowCreated: %p\n", (void*)nActivity.callbacks->onNativeWindowCreated);
    printf("onNativeWindowResized: %p\n", (void*)nActivity.callbacks->onNativeWindowResized);
    printf("onNativeWindowRedrawNeeded: %p\n", (void*)nActivity.callbacks->onNativeWindowRedrawNeeded);
    printf("onNativeWindowDestroyed: %p\n", (void*)nActivity.callbacks->onNativeWindowDestroyed);
    printf("onInputQueueCreated: %p\n", (void*)nActivity.callbacks->onInputQueueCreated);
    printf("onInputQueueDestroyed: %p\n", (void*)nActivity.callbacks->onInputQueueDestroyed);
    printf("onContentRectChanged: %p\n", (void*)nActivity.callbacks->onContentRectChanged);
    printf("onConfigurationChanged: %p\n", (void*)nActivity.callbacks->onConfigurationChanged);
    printf("onLowMemory: %p\n", (void*)nActivity.callbacks->onLowMemory);
#endif
}

// Crash diagnostics.
//
// Unity installs its own handler for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT from
// libunity's JNI_OnLoad -- after main() has already installed segfault_handler()
// -- and chains into the handler it replaced (libunity+0x319b2c re-enters the
// saved one), so this function is still reached. Two consequences:
//
//  * The ucontext Unity forwards is the only trustworthy record of the fault,
//    hence SA_SIGINFO and the register dump below. si_addr plus the saved pc
//    name the actual faulting instruction; the frame-pointer walk cannot,
//    because by now it runs on a stack Unity's handler has already rewritten
//    (it keeps the TLS base in x29 instead of a frame pointer).
//  * backtrace(3) therefore reports a bogus outer frame that lands inside
//    Unity's own crash handler. bd_dump_crash_backtrace() recovers the real
//    chain by scanning the stack for return addresses instead of frame links.
//
// Output goes through write(2)/vsnprintf rather than BD_LOG so a crash dump
// survives a build configured with BD_ENABLE_LOG=OFF (see AGENTS.md).
static void bd_crash_printf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        ssize_t written = write(STDERR_FILENO, buf, len);
        (void)written;
    }
}

void segfault_handler(int signal, siginfo_t* info, void* context) {
    bd_crash_printf("\n[BD-SEGV] signal %d si_code=%d si_addr=%p\n",
                    signal, info ? info->si_code : -1,
                    info ? info->si_addr : (void*)0);

#if defined(__aarch64__)
    if (context) {
        const mcontext_t& mc = ((ucontext_t*)context)->uc_mcontext;
        bd_crash_printf("  sp %016llx pstate %#llx\n",
                        (unsigned long long)mc.sp, (unsigned long long)mc.pstate);
        for (int i = 0; i + 2 < 30; i += 3) {
            bd_crash_printf("  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n",
                            i, (unsigned long long)mc.regs[i],
                            i + 1, (unsigned long long)mc.regs[i + 1],
                            i + 2, (unsigned long long)mc.regs[i + 2]);
        }
        bd_describe_crash_address("BD-SEGV", "pc", mc.pc);
        bd_describe_crash_address("BD-SEGV", "lr", mc.regs[30]);
        bd_describe_crash_address("BD-SEGV", "fp", mc.regs[29]);
    }
#endif

    bd_dump_crash_backtrace("BD-SEGV");
    _exit(1);
}

void exit_handler(int signal) {
    // Flush prefs first — small synchronous write, safe under memory pressure.
    jnivm::android::content::SharedPreferences::flush_all();
    // _exit instead of exit: skip C++ dtors / atexit / stdio flush. On 1GB
    // devices the cleanup cascade (Mali teardown + IL2CPP shutdown) OOMs.
    const char msg[] = "Caught signal, fast-exiting via _exit\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(0);
}

void print_backtrace_on_segfault()
{
    // SA_SIGINFO, not signal(2): the handler is chained onto from Unity's own
    // crash handler, which forwards the original siginfo and ucontext. Without
    // it the fault address and pc are unrecoverable once the stack is gone.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segfault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (int s : signals)
        sigaction(s, &sa, nullptr);
}

void exit_on_signals()
{
    struct sigaction sa;
    sa.sa_handler = exit_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}