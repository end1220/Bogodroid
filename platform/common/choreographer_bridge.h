#pragma once

// Frame pacing: the real EGL swap path (thunks/egl_sdl) has to drive the
// Java-side Choreographer so that Unity's frame callbacks keep firing.
//
// It cannot include javastubs/android.h to reach the class directly:
// egl_sdl.cpp includes glad_egl.h -> EGL/eglplatform.h -> X11/Xlib.h, and Xlib
// defines `None` as 0L, which collides with jnivm::FunctionType::None (that is
// also why egl_sdl.cpp is excluded from the target's precompiled header). Hand-
// writing a stand-in `class Choreographer` there instead is worse: two
// definitions of one class is an ODR violation that only shows up as
// -Wlto-type-mismatch, and it breaks outright if the real class gains a base.
//
// So the Java class stays on its side of the line and this C bridge crosses it.
// Implemented in javastubs/android_view.cpp next to the real Choreographer.
#ifdef __cplusplus
extern "C" {
#endif
void bd_choreographer_signal_vsync(void);
#ifdef __cplusplus
}
#endif
