#pragma once
#include "plugin_il2cpp.h"
namespace brickgamepro { namespace casing_art {
// Unity thread only. Noninteractive overlay sized in Canvas units, not casing units.
void bind(bogodroid::Il2cppRuntime& runtime, const BogoPluginApi* api);
void update(void* canvas, void* center_casing, float width, float height);
} }
