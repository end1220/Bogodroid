#pragma once

// Run present-thread callbacks registered by plugins (Plugin ABI v3+).
// Safe no-op when none registered. Called from eglSwapBuffers paths.
#ifdef __cplusplus
extern "C" {
#endif
void bd_plugin_run_present_callbacks(void);
#ifdef __cplusplus
}
#endif
