#include "debug_utils.h"

#include "gles2.h"
#include "glad.h"
#include "glad_egl.h"
#include "android.h" // for SharedPreferences::flush_all
#include "logging.h"

#include <csignal>
#include <execinfo.h>
#include <unistd.h>

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