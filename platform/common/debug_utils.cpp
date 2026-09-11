#include "debug_utils.h"

#include "gles2.h"
#include "glad.h"
#include "glad_egl.h"
#include "android.h" // for SharedPreferences::flush_all
#include "logging.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <ctime>
#include <execinfo.h>
#include <unistd.h>

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
    BD_LOG("MEM", "pid=%d rss=%.1fMB hwm=%.1fMB vsz=%.1fMB sys_avail=%.1fMB/%ldMB%s%s",
           (int)getpid(),
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

void segfault_handler(int signal) {
    void *array[50];
    size_t size = backtrace(array, 50);
    BD_LOG("SEGV", "signal %d", signal);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
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
    signal(SIGSEGV, segfault_handler);
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