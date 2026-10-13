#pragma once

// Plugin specifies a BMP relative to the game cwd in the early phase. The
// host copies the path before the deferred plugin DSO is unloaded.
int bd_startup_set_image(const char* path);
void bd_startup_scene_ready();
void bd_startup_show();
// Called on the current render context before a guest swap.
bool bd_startup_draw();
