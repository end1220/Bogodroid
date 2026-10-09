#include "plugin_api.h"

#include <SDL2/SDL.h>
#include <array>
#include <cctype>
#include <cmath>
#include <string>

namespace brickgamepro {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static bool g_enabled = false;
static bool g_touch_enabled = false;
static float g_design_width = 640.0f;
static float g_design_height = 480.0f;

struct TouchPoint {
    int button = SDL_CONTROLLER_BUTTON_INVALID;
    int pointer_id = -1;
    float x = 0.0f;
    float y = 0.0f;
    bool active = false;
};

static std::array<TouchPoint, 9> g_points = {{
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,  222.0f, 375.0f, false},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 300.0f, 375.0f, false},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_CONTROLLER_BUTTON_DPAD_UP,    260.0f, 335.0f, false},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_CONTROLLER_BUTTON_DPAD_DOWN,  260.0f, 414.0f, false},
    {SDL_CONTROLLER_BUTTON_A,          SDL_CONTROLLER_BUTTON_A,          393.0f, 377.0f, false},
    {SDL_CONTROLLER_BUTTON_B,          SDL_CONTROLLER_BUTTON_B,          334.0f, 307.0f, false},
    {SDL_CONTROLLER_BUTTON_X,          SDL_CONTROLLER_BUTTON_X,          367.0f, 307.0f, false},
    {SDL_CONTROLLER_BUTTON_Y,          SDL_CONTROLLER_BUTTON_Y,          400.0f, 307.0f, false},
    {SDL_CONTROLLER_BUTTON_START,      SDL_CONTROLLER_BUTTON_START,      433.0f, 307.0f, false},
}};

static std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static int parse_button(std::string s)
{
    s = lower(std::move(s));
    if (s == "left") s = "dpleft";
    else if (s == "right") s = "dpright";
    else if (s == "up") s = "dpup";
    else if (s == "down") s = "dpdown";
    else if (s == "select") s = "back";
    return SDL_GameControllerGetButtonFromString(s.c_str());
}

static void read_point(const char* name)
{
    const int button = parse_button(name);
    if (button == SDL_CONTROLLER_BUTTON_INVALID)
        return;
    TouchPoint* point = nullptr;
    for (auto& candidate : g_points) {
        if (candidate.button == button) {
            point = &candidate;
            break;
        }
    }
    if (!point)
        return;

    const std::string prefix = std::string("game_patches.brickgamepro.touch.") + name;
    const double x = g_api->config_get_f64(
        (prefix + ".x").c_str(), static_cast<double>(point->x));
    const double y = g_api->config_get_f64(
        (prefix + ".y").c_str(), static_cast<double>(point->y));
    if (std::isfinite(x) && std::isfinite(y)) {
        point->x = static_cast<float>(x);
        point->y = static_cast<float>(y);
    }
}

static void on_button(int button, int down, void*)
{
    if (!g_enabled || !g_touch_enabled || !g_api || !g_api->inject_touch)
        return;
    for (auto& point : g_points) {
        if (point.button != button)
            continue;
        const bool is_down = down != 0;
        if (point.active == is_down)
            return;
        if (g_api->inject_touch(point.pointer_id, point.x, point.y,
                                g_design_width, g_design_height,
                                is_down ? 1 : 0, "brickgamepro")) {
            point.active = is_down;
        }
        return;
    }
}

static int init(const BogoPluginApi* api)
{
    g_api_storage = *api;
    g_api = &g_api_storage;
    g_enabled = g_api->config_get_bool("game_patches.brickgamepro.enabled", 1) != 0;
    if (!g_enabled) {
        g_api->log("BRICKGAME", "plugin disabled");
        return BOGO_PLUGIN_OK;
    }

    const double design_width =
        g_api->config_get_f64("game_patches.brickgamepro.touch.design_width", 640.0);
    const double design_height =
        g_api->config_get_f64("game_patches.brickgamepro.touch.design_height", 480.0);
    if (std::isfinite(design_width) && design_width > 0.0)
        g_design_width = static_cast<float>(design_width);
    if (std::isfinite(design_height) && design_height > 0.0)
        g_design_height = static_cast<float>(design_height);

    for (const char* name : {"dpleft", "dpright", "dpup", "dpdown",
                             "a", "b", "x", "y", "start"}) {
        read_point(name);
    }

    g_touch_enabled =
        g_api->config_get_bool("game_patches.brickgamepro.touch.enabled", 1) != 0;
    if (g_touch_enabled && g_api->register_input_observer) {
        BogoInputObserver observer = {};
        observer.button = &on_button;
        observer.userdata = nullptr;
        g_api->register_input_observer(&observer);
        for (const auto& point : g_points) {
            const char* name = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(point.button));
            g_api->log("BRICKGAME", "%s -> touch %.1f,%.1f",
                       name ? name : "?", point.x, point.y);
        }
    }

    if (g_api->set_present_viewport) {
        const int viewport_enabled =
            g_api->config_get_bool("game_patches.brickgamepro.viewport.enabled", 1);
        const double render_scale =
            g_api->config_get_f64("game_patches.brickgamepro.viewport.renderScale", 2.0);
        const char* anchor = g_api->config_get_string(
            "game_patches.brickgamepro.viewport.anchor", "top");
        const int64_t offset_y =
            g_api->config_get_i64("game_patches.brickgamepro.viewport.offsetY", -48);
        g_api->set_present_viewport(viewport_enabled, render_scale, anchor,
                                    static_cast<int>(offset_y));
    }

    g_api->log("BRICKGAME", "plugin armed touch=%d design=%.0fx%.0f",
               g_touch_enabled ? 1 : 0,
               static_cast<double>(g_design_width),
               static_cast<double>(g_design_height));
    return BOGO_PLUGIN_OK;
}

} // namespace brickgamepro

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version < 8 ||
        api->struct_size < sizeof(BogoPluginApi) || !api->log ||
        !api->config_get_bool || !api->config_get_f64)
        return -1;
    return brickgamepro::init(api);
}
