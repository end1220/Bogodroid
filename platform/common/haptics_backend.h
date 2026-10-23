#pragma once

#include <cstdint>

// Android/JNI callers only publish requests. Device discovery and SDL output
// run on the input thread; no per-request threads or Android object lifetimes.
namespace bd_haptics {
bool has_vibrator();
void vibrate(int64_t milliseconds);
void cancel();

// InputBackend lifecycle, with the same SDL joystick subsystem lifetime.
void refresh_devices();
void update();
void set_suspended(bool suspended);
void shutdown();

// Fast Unity exit runs on the render thread. Ask the input thread to stop and
// wait at most 250 ms for acknowledgement before the caller uses _exit().
void prepare_exit();
}
