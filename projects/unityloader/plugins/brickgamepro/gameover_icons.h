#pragma once

#include "plugin_il2cpp.h"

namespace brickgamepro {

// Unity thread only. The original actions stay inactive; one sibling RawImage
// has no Button, text, click listener or raycast target.
void bind_gameover_icons(bogodroid::Il2cppRuntime& runtime, const BogoPluginApi* api);
void add_gameover_icon(void* original_rect, void* finish_rect, const char* action);

} // namespace brickgamepro
