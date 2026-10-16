#pragma once

// Plugin specifies a BMP or video relative to the game cwd in the early phase. The
// host copies the path before the deferred plugin DSO is unloaded.
int bd_startup_set_image(const char* path);
int bd_startup_set_animation(const char* path, int frame_count, int frame_ms);
int bd_startup_set_video(const char* path, float height_fraction);
void bd_startup_scene_ready();
void bd_startup_show();
// Called on the current render context before a guest swap.
bool bd_startup_draw();
// SDL-style result; -2 asks the EGL bridge to use its regular SDL binding.
int bd_startup_make_current(void* context);
// True while the independent animation owns the window, including handoff.
bool bd_startup_background_present();
