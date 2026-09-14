#pragma once

// Resolve the [device] display geometry for the Android / EGL / NDK shims so
// one TOML can serve several handhelds.
//
// Priority (highest wins), see device_display.cpp:
//   1. explicit TOML displayWidth / displayHeight / displayRefreshRate > 0;
//      a TOML value is final and is never overwritten;
//   2. SDL_GetCurrentDisplayMode(0) once SDL video is up (mirrors
//      for-fun-h700/GLEScene PlatformContext);
//   3. /dev/fb0, only as a pre-SDL fallback for JNI stubs that query the
//      display before EGL initialises;
//   4. 640x480@60.
//
// The getters probe on demand, so before SDL video exists they may report the
// fb0/default value. Call bd_device_display_probe() right after
// SDL_Init(SDL_INIT_VIDEO) to pick up the SDL source.

int bd_device_display_width();
int bd_device_display_height();
float bd_device_display_refresh_rate();

void bd_device_display_probe();
