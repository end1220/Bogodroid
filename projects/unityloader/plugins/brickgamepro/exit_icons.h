#pragma once

#include "plugin_il2cpp.h"

namespace brickgamepro {

// Unity thread only. Adds a centered, noninteractive decoration in ExitMenu.
void bind_exit_icon(bogodroid::Il2cppRuntime& runtime, const BogoPluginApi* api);
void add_exit_icon(void* exit_rect);

} // namespace brickgamepro
