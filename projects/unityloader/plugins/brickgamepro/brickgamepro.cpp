#include "plugin_api.h"

#include <SDL2/SDL.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace brickgamepro {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static bool g_enabled = true;
static bool g_touch_enabled = true;
static bool g_touch_hold_refresh = true;
static constexpr float kDesignWidth = 640.0f;
static constexpr float kDesignHeight = 480.0f;
static constexpr float kCanvasScale = 2.0f;
static constexpr int kCanvasOffsetY = -48;
static constexpr bool kUseCanvasLayout = false;
static constexpr const char* kCanvasObject = "Canvas";
static constexpr const char* kPackageName = "com.perseusgames.brickgamepro";
static float g_design_width = kDesignWidth;
static float g_design_height = kDesignHeight;
static bool g_skin_switch_enabled = true;
static int g_skin_switch_button = SDL_CONTROLLER_BUTTON_GUIDE;
static std::atomic<int> g_pending_skin{-1};
static std::atomic<int> g_next_skin{1};
static std::string g_skin_object = kCanvasObject;

struct Vector2 {
    float x;
    float y;
};

struct Vector3 {
    float x;
    float y;
    float z;
};

typedef void* (*p_domain_get)();
typedef void** (*p_domain_get_assemblies)(void* domain, size_t* size);
typedef void* (*p_assembly_get_image)(void* assembly);
typedef const char* (*p_image_get_name)(void* image);
typedef void* (*p_class_from_name)(void* image, const char* namespaze,
                                   const char* name);
typedef void* (*p_get_method)(void* klass, const char* name, int argc);
typedef void* (*p_class_get_methods)(void* klass, void** iter);
typedef const char* (*p_method_get_name)(void* method);
typedef int (*p_method_get_param_count)(void* method);
typedef void* (*p_string_new)(const char* value);

typedef uintptr_t (*orig8_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void* (*get_transform_t)(void* component, void* mi);
typedef void (*set_v2_t)(void* rt, Vector2 value, void* mi);
typedef void (*set_v3_t)(void* transform, Vector3 value, void* mi);
typedef void (*set_float_t)(void* object, float value, void* mi);
typedef void (*canvas_scaler_handle_t)(void* scaler, void* mi);
typedef int (*screen_get_t)(void* mi);
typedef void (*force_update_canvases_t)(void* mi);

static p_domain_get f_domain_get = nullptr;
static p_domain_get_assemblies f_domain_assemblies = nullptr;
static p_assembly_get_image f_assembly_image = nullptr;
static p_image_get_name f_image_name = nullptr;
static p_class_from_name f_class_from_name = nullptr;
static p_get_method f_get_method = nullptr;
static p_class_get_methods f_class_get_methods = nullptr;
static p_method_get_name f_method_get_name = nullptr;
static p_method_get_param_count f_method_get_param_count = nullptr;
static p_string_new f_string_new = nullptr;

static get_transform_t m_component_get_transform = nullptr;
static void* mi_component_get_transform = nullptr;
static set_v3_t m_tf_set_local_scale = nullptr;
static void* mi_tf_set_local_scale = nullptr;
static set_float_t m_canvas_set_scale_factor = nullptr;
static void* mi_canvas_set_scale_factor = nullptr;
static set_v2_t m_rt_set_anchor_min = nullptr;
static void* mi_rt_set_anchor_min = nullptr;
static set_v2_t m_rt_set_anchor_max = nullptr;
static void* mi_rt_set_anchor_max = nullptr;
static set_v2_t m_rt_set_pivot = nullptr;
static void* mi_rt_set_pivot = nullptr;
static set_v2_t m_rt_set_size_delta = nullptr;
static void* mi_rt_set_size_delta = nullptr;
static set_v2_t m_rt_set_anchored_position = nullptr;
static void* mi_rt_set_anchored_position = nullptr;
static screen_get_t m_screen_width = nullptr;
static void* mi_screen_width = nullptr;
static screen_get_t m_screen_height = nullptr;
static void* mi_screen_height = nullptr;
static force_update_canvases_t m_canvas_force_update = nullptr;
static void* mi_canvas_force_update = nullptr;
static bool g_canvas_layout_ready = false;
static void* g_canvas_rt = nullptr;
static int g_last_screen_width = 0;
static int g_last_screen_height = 0;
static bool g_canvas_layout_logged = false;
static uintptr_t g_gui_start_orig = 0;
static uintptr_t g_canvas_scaler_orig = 0;

struct TouchPoint {
    int button = SDL_CONTROLLER_BUTTON_INVALID;
    int pointer_id = -1;
    float x = 0.0f;
    float y = 0.0f;
    bool active = false;
};

static std::array<TouchPoint, 7> g_points = {{
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,  222.0f, 375.0f, false},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 300.0f, 375.0f, false},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_CONTROLLER_BUTTON_DPAD_UP,    260.0f, 335.0f, false},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_CONTROLLER_BUTTON_DPAD_DOWN,  260.0f, 414.0f, false},
    {SDL_CONTROLLER_BUTTON_A,          SDL_CONTROLLER_BUTTON_A,          393.0f, 377.0f, false},
    {SDL_CONTROLLER_BUTTON_B,          SDL_CONTROLLER_BUTTON_B,          334.0f, 307.0f, false},
    {SDL_CONTROLLER_BUTTON_X,          SDL_CONTROLLER_BUTTON_X,          367.0f, 307.0f, false},
}};

struct SkinMethod {
    const char* name;
};

static std::array<SkinMethod, 11> g_skin_methods = {{
    {"ChangeToBlueSkin"},
    {"ChangeToBlackSkin"},
    {"ChangeToPinkSkin"},
    {"ChangeToGreenSkin"},
    {"ChangeToYellowSkin"},
    {"ChangeToRedSkin"},
    {"ChangeToPurpleSkin"},
    {"ChangeToOrangeSkin"},
    {"ChangeToBlue2Skin"},
    {"ChangeToGreen2Skin"},
    {"ChangeToSilverSkin"},
}};
static int g_skin_count = static_cast<int>(g_skin_methods.size());

using UnitySendMessageFn = void (*)(const char*, const char*, const char*);

static UnitySendMessageFn unity_send_message = nullptr;

static bool resolve_unity_send_message()
{
    if (unity_send_message)
        return true;
    if (!g_api || !g_api->find_module || !g_api->so_symbol)
        return false;
    BogoSoModule* unity = g_api->find_module("libunity.so");
    if (!unity)
        return false;
    unity_send_message = reinterpret_cast<UnitySendMessageFn>(
        g_api->so_symbol(unity, "UnitySendMessage"));
    if (!unity_send_message && g_api)
        g_api->log("BRICKGAME", "UnitySendMessage not found");
    return unity_send_message != nullptr;
}

static void invoke_skin(int index)
{
    if (!g_skin_switch_enabled || index < 0)
        return;
    if (g_skin_count <= 0)
        return;
    index %= g_skin_count;
    if (resolve_unity_send_message()) {
        unity_send_message(g_skin_object.c_str(), g_skin_methods[index].name, "");
        if (g_api) {
            g_api->log("BRICKGAME", "guide skin message %s.%s",
                       g_skin_object.c_str(), g_skin_methods[index].name);
        }
        return;
    }
    if (g_api) {
        g_api->log("BRICKGAME", "skin switch skipped: UnitySendMessage unavailable");
    }
}

static void* find_image(const char* want)
{
    if (!f_domain_get || !f_domain_assemblies || !f_assembly_image ||
        !f_image_name)
        return nullptr;
    void* domain = f_domain_get();
    size_t count = 0;
    void** assemblies = f_domain_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        void* image = f_assembly_image(assemblies[i]);
        const char* name = f_image_name(image);
        if (name && std::strstr(name, want))
            return image;
    }
    return nullptr;
}

static void* find_class_image(const char* namespaze, const char* klass)
{
    if (!f_domain_get || !f_domain_assemblies || !f_assembly_image ||
        !f_class_from_name)
        return nullptr;
    void* domain = f_domain_get();
    size_t count = 0;
    void** assemblies = f_domain_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        void* image = f_assembly_image(assemblies[i]);
        if (f_class_from_name(image, namespaze, klass))
            return image;
    }
    return nullptr;
}

static bool resolve_call(void* image, const char* namespaze, const char* klass,
                         const char* method, int argc, void** fn_out,
                         void** mi_out)
{
    void* k = f_class_from_name(image, namespaze, klass);
    if (!k) {
        g_api->log("BRICKGAME", "class %s.%s not found", namespaze, klass);
        return false;
    }
    void* m = f_get_method(k, method, argc);
    if (!m && f_class_get_methods && f_method_get_name &&
        f_method_get_param_count) {
        void* iter = nullptr;
        void* candidate = nullptr;
        while ((candidate = f_class_get_methods(k, &iter)) != nullptr) {
            const char* candidate_name = f_method_get_name(candidate);
            if (!candidate_name || std::strcmp(candidate_name, method) != 0)
                continue;
            if (f_method_get_param_count(candidate) != argc)
                continue;
            m = candidate;
            break;
        }
    }
    if (!m) {
        g_api->log("BRICKGAME", "method %s.%s(%d) not found", klass, method,
                   argc);
        if (f_class_get_methods && f_method_get_name &&
            f_method_get_param_count) {
            int logged = 0;
            void* iter = nullptr;
            void* candidate = nullptr;
            while ((candidate = f_class_get_methods(k, &iter)) != nullptr &&
                   logged < 24) {
                const char* candidate_name = f_method_get_name(candidate);
                if (candidate_name && std::strstr(candidate_name, method)) {
                    g_api->log("BRICKGAME", "  candidate %s.%s(%d)", klass,
                               candidate_name,
                               f_method_get_param_count(candidate));
                    ++logged;
                }
            }
        }
        return false;
    }
    *fn_out = reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(m));
    *mi_out = m;
    return *fn_out != nullptr;
}

static bool hook_method(void* image, const char* namespaze, const char* klass,
                        const char* method, int argc, uintptr_t hook,
                        uintptr_t* orig_out)
{
    void* fn = nullptr;
    void* mi = nullptr;
    if (!resolve_call(image, namespaze, klass, method, argc, &fn, &mi))
        return false;
    uintptr_t orig = 0;
    g_api->hook_address_detour(g_api->il2cpp, reinterpret_cast<uintptr_t>(fn),
                               hook, &orig);
    if (!orig) {
        g_api->log("BRICKGAME", "detour %s.%s FAILED", klass, method);
        return false;
    }
    *orig_out = orig;
    g_api->log("BRICKGAME", "hooked %s.%s(%d) @ %p", klass, method, argc,
               fn);
    return true;
}

static void apply_canvas_layout()
{
    if (!g_canvas_layout_ready)
        return;
    const int screen_width = m_screen_width(mi_screen_width);
    const int screen_height = m_screen_height(mi_screen_height);
    if (screen_width <= 0 || screen_height <= 0)
        return;

    if (!g_canvas_rt)
        return;

    const Vector2 center = {0.5f, 0.5f};
    const Vector3 scale = {kCanvasScale, kCanvasScale, 1.0f};
    const Vector2 size = {
        static_cast<float>(screen_width) * kCanvasScale,
        static_cast<float>(screen_height) * kCanvasScale,
    };
    const Vector2 position = {
        0.0f,
        -static_cast<float>(screen_height) * (kCanvasScale - 1.0f) * 0.5f -
            static_cast<float>(kCanvasOffsetY),
    };

    m_tf_set_local_scale(g_canvas_rt, scale, mi_tf_set_local_scale);
    m_rt_set_anchor_min(g_canvas_rt, center, mi_rt_set_anchor_min);
    m_rt_set_anchor_max(g_canvas_rt, center, mi_rt_set_anchor_max);
    m_rt_set_pivot(g_canvas_rt, center, mi_rt_set_pivot);
    m_rt_set_size_delta(g_canvas_rt, size, mi_rt_set_size_delta);
    m_rt_set_anchored_position(g_canvas_rt, position,
                               mi_rt_set_anchored_position);
    const bool first_or_resized =
        !g_canvas_layout_logged || screen_width != g_last_screen_width ||
        screen_height != g_last_screen_height;
    if (m_canvas_force_update && first_or_resized)
        m_canvas_force_update(mi_canvas_force_update);

    if (first_or_resized) {
        g_canvas_layout_logged = true;
        g_last_screen_width = screen_width;
        g_last_screen_height = screen_height;
        g_api->log("BRICKGAME",
                   "Canvas RectTransform active screen=%dx%d size=%.0fx%.0f "
                   "pos=(%.0f,%.0f) scale=%.1f offsetY=%d",
                   screen_width, screen_height, static_cast<double>(size.x),
                   static_cast<double>(size.y),
                   static_cast<double>(position.x),
                   static_cast<double>(position.y),
                   static_cast<double>(kCanvasScale), kCanvasOffsetY);
    }
}

static uintptr_t gui_start_hook(uintptr_t x0, uintptr_t x1, uintptr_t x2,
                                uintptr_t x3, uintptr_t x4, uintptr_t x5,
                                uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_gui_start_orig) {
        ret = reinterpret_cast<orig8_t>(g_gui_start_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (x0 && m_component_get_transform) {
        void* rt = m_component_get_transform(reinterpret_cast<void*>(x0),
                                             mi_component_get_transform);
        if (rt) {
            g_canvas_rt = rt;
            apply_canvas_layout();
        }
    }
    return ret;
}

static void canvas_scaler_hook(void* self, void* mi)
{
    if (g_canvas_scaler_orig) {
        reinterpret_cast<canvas_scaler_handle_t>(g_canvas_scaler_orig)(
            self, mi);
    }
    if (self && m_canvas_set_scale_factor) {
        m_canvas_set_scale_factor(self, kCanvasScale,
                                  mi_canvas_set_scale_factor);
        apply_canvas_layout();
    }
}

static void post_il2cpp_init(void*)
{
    BogoSoModule* il2cpp = g_api ? g_api->il2cpp : nullptr;
    if (!il2cpp || !g_api->so_symbol)
        return;
    f_domain_get = reinterpret_cast<p_domain_get>(
        g_api->so_symbol(il2cpp, "il2cpp_domain_get"));
    f_domain_assemblies = reinterpret_cast<p_domain_get_assemblies>(
        g_api->so_symbol(il2cpp, "il2cpp_domain_get_assemblies"));
    f_assembly_image = reinterpret_cast<p_assembly_get_image>(
        g_api->so_symbol(il2cpp, "il2cpp_assembly_get_image"));
    f_image_name = reinterpret_cast<p_image_get_name>(
        g_api->so_symbol(il2cpp, "il2cpp_image_get_name"));
    f_class_from_name = reinterpret_cast<p_class_from_name>(
        g_api->so_symbol(il2cpp, "il2cpp_class_from_name"));
    f_get_method = reinterpret_cast<p_get_method>(
        g_api->so_symbol(il2cpp, "il2cpp_class_get_method_from_name"));
    f_class_get_methods = reinterpret_cast<p_class_get_methods>(
        g_api->so_symbol(il2cpp, "il2cpp_class_get_methods"));
    f_method_get_name = reinterpret_cast<p_method_get_name>(
        g_api->so_symbol(il2cpp, "il2cpp_method_get_name"));
    f_method_get_param_count = reinterpret_cast<p_method_get_param_count>(
        g_api->so_symbol(il2cpp, "il2cpp_method_get_param_count"));
    f_string_new = reinterpret_cast<p_string_new>(
        g_api->so_symbol(il2cpp, "il2cpp_string_new_wrapper"));
    if (!f_string_new) {
        f_string_new = reinterpret_cast<p_string_new>(
            g_api->so_symbol(il2cpp, "il2cpp_string_new"));
    }
    if (!f_domain_get || !f_domain_assemblies || !f_assembly_image ||
        !f_image_name || !f_class_from_name || !f_get_method ||
        !f_string_new) {
        g_api->log("BRICKGAME", "missing il2cpp exports; Canvas layout disabled");
        return;
    }

    void* core = find_image("UnityEngine.CoreModule");
    if (!core)
        core = find_image("UnityEngine");
    void* ui = find_image("UnityEngine.UIModule");
    if (!ui)
        ui = core;
    void* ui_managed = find_image("UnityEngine.UI");
    if (!ui_managed)
        ui_managed = ui;
    void* scaler_image = find_class_image("UnityEngine.UI", "CanvasScaler");
    const char* scaler_ns = "UnityEngine.UI";
    if (!scaler_image) {
        scaler_image = find_class_image("UnityEngine", "CanvasScaler");
        scaler_ns = "UnityEngine";
    }
    if (!scaler_image) {
        scaler_image = find_class_image("", "CanvasScaler");
        scaler_ns = "";
    }
    void* asm_image = find_image("Assembly-CSharp");
    if (!core) {
        g_api->log("BRICKGAME", "UnityEngine image not found; Canvas layout disabled");
        return;
    }
    if (!asm_image) {
        g_api->log("BRICKGAME", "Assembly-CSharp image not found; Canvas layout disabled");
        return;
    }

    bool ok = true;
    ok &= resolve_call(core, "UnityEngine", "Component", "get_transform", 0,
                       reinterpret_cast<void**>(&m_component_get_transform),
                       &mi_component_get_transform);
    ok &= resolve_call(core, "UnityEngine", "Transform", "set_localScale",
                       1, reinterpret_cast<void**>(&m_tf_set_local_scale),
                       &mi_tf_set_local_scale);
    ok &= resolve_call(core, "UnityEngine", "RectTransform", "set_anchorMin", 1,
                       reinterpret_cast<void**>(&m_rt_set_anchor_min),
                       &mi_rt_set_anchor_min);
    ok &= resolve_call(core, "UnityEngine", "RectTransform", "set_anchorMax", 1,
                       reinterpret_cast<void**>(&m_rt_set_anchor_max),
                       &mi_rt_set_anchor_max);
    ok &= resolve_call(core, "UnityEngine", "RectTransform", "set_pivot", 1,
                       reinterpret_cast<void**>(&m_rt_set_pivot),
                       &mi_rt_set_pivot);
    ok &= resolve_call(core, "UnityEngine", "RectTransform", "set_sizeDelta", 1,
                       reinterpret_cast<void**>(&m_rt_set_size_delta),
                       &mi_rt_set_size_delta);
    ok &= resolve_call(core, "UnityEngine", "RectTransform",
                       "set_anchoredPosition", 1,
                       reinterpret_cast<void**>(&m_rt_set_anchored_position),
                       &mi_rt_set_anchored_position);
    ok &= resolve_call(core, "UnityEngine", "Screen", "get_width", 0,
                       reinterpret_cast<void**>(&m_screen_width),
                       &mi_screen_width);
    ok &= resolve_call(core, "UnityEngine", "Screen", "get_height", 0,
                       reinterpret_cast<void**>(&m_screen_height),
                       &mi_screen_height);

    if (!resolve_call(ui, "UnityEngine", "Canvas", "ForceUpdateCanvases", 0,
                      reinterpret_cast<void**>(&m_canvas_force_update),
                      &mi_canvas_force_update)) {
        m_canvas_force_update = nullptr;
        mi_canvas_force_update = nullptr;
    }
    ok &= resolve_call(scaler_image ? scaler_image : ui_managed, scaler_ns,
                       "CanvasScaler",
                       "set_scaleFactor", 1,
                       reinterpret_cast<void**>(&m_canvas_set_scale_factor),
                       &mi_canvas_set_scale_factor);

    bool hooked = false;
    if (ok) {
        hooked = hook_method(asm_image, "", "GUIManager", "Start", 0,
                             reinterpret_cast<uintptr_t>(&gui_start_hook),
                             &g_gui_start_orig);
        if (!hooked) {
            hooked = hook_method(asm_image, "", "GUIManager", "Awake", 0,
                                 reinterpret_cast<uintptr_t>(&gui_start_hook),
                                 &g_gui_start_orig);
        }
    }
    bool scaler_hooked = false;
    if (ok) {
        scaler_hooked = hook_method(
            scaler_image ? scaler_image : ui_managed, scaler_ns,
            "CanvasScaler",
            "HandleScaleWithScreenSize",
            0, reinterpret_cast<uintptr_t>(&canvas_scaler_hook),
            &g_canvas_scaler_orig);
        if (!scaler_hooked) {
            scaler_hooked = hook_method(
                scaler_image ? scaler_image : ui_managed, scaler_ns,
                "CanvasScaler",
                "HandleScaleWithScreenSize", 0,
                reinterpret_cast<uintptr_t>(&canvas_scaler_hook),
                &g_canvas_scaler_orig);
        }
    }

    g_canvas_layout_ready = ok && hooked && scaler_hooked;
    g_api->log("BRICKGAME",
               "Canvas RectTransform layout %s gui=%d scaler=%d",
               g_canvas_layout_ready ? "armed" : "disabled",
               hooked ? 1 : 0, scaler_hooked ? 1 : 0);
}

static void on_button(int button, int down, void*)
{
    if (!g_enabled)
        return;
    if (g_skin_switch_enabled && button == g_skin_switch_button && down != 0) {
        int pending = g_pending_skin.load();
        if (pending < 0) {
            const int count = g_skin_count > 0 ? g_skin_count : 1;
            const int skin = g_next_skin.fetch_add(1) % count;
            g_pending_skin.store(skin);
        }
        return;
    }
    if (!g_touch_enabled || !g_api || !g_api->inject_touch)
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

static void on_frame(void*)
{
    const int skin = g_pending_skin.exchange(-1);
    if (skin >= 0)
        invoke_skin(skin);
    apply_canvas_layout();
    if (!g_enabled || !g_touch_enabled || !g_touch_hold_refresh ||
        !g_api || !g_api->inject_touch)
        return;
    for (auto& point : g_points) {
        if (!point.active)
            continue;
        g_api->inject_touch(point.pointer_id, point.x, point.y,
                            g_design_width, g_design_height,
                            1, "brickgamepro");
    }
}

static int init(const BogoPluginApi* api)
{
    g_api_storage = *api;
    g_api = &g_api_storage;
    if (g_api->config_get_string) {
        const char* package = g_api->config_get_string("package.packageName", "");
        if (package && *package && std::strcmp(package, kPackageName) != 0) {
            g_api->log("BRICKGAME", "skipped package=%s", package);
            return BOGO_PLUGIN_OK;
        }
    }

    if (g_api->set_present_viewport_filter) {
        g_api->set_present_viewport_filter(1, kCanvasScale, "top",
                                           kCanvasOffsetY, "nearest");
    } else if (g_api->set_present_viewport) {
        g_api->set_present_viewport(1, kCanvasScale, "top", kCanvasOffsetY);
    }

    if (g_api->register_input_observer) {
        BogoInputObserver observer = {};
        observer.button = &on_button;
        observer.frame_begin = &on_frame;
        observer.userdata = nullptr;
        g_api->register_input_observer(&observer);
        for (const auto& point : g_points) {
            const char* name = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(point.button));
            g_api->log("BRICKGAME", "%s -> touch %.1f,%.1f",
                       name ? name : "?", point.x, point.y);
        }
    }

    if (kUseCanvasLayout && g_api->register_il2cpp_post_init) {
        if (!g_api->register_il2cpp_post_init(&post_il2cpp_init, nullptr))
            g_api->log("BRICKGAME", "failed to register Canvas layout callback");
    } else if (!kUseCanvasLayout) {
        g_api->log("BRICKGAME", "Canvas layout disabled; using GPU present viewport");
    } else {
        g_api->log("BRICKGAME", "Canvas layout unavailable: no post-init API");
    }

    g_api->log("BRICKGAME",
               "plugin armed touch=%d refresh=%d design=%.0fx%.0f canvasScale=%.1f offsetY=%d",
               g_touch_enabled ? 1 : 0,
               g_touch_hold_refresh ? 1 : 0,
               static_cast<double>(g_design_width),
               static_cast<double>(g_design_height),
               static_cast<double>(kCanvasScale),
               kCanvasOffsetY);
    if (g_skin_switch_enabled) {
        const char* button = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(g_skin_switch_button));
        g_api->log("BRICKGAME", "skin switch button=%s start=%d count=%d",
                   button ? button : "?", g_next_skin.load(), g_skin_count);
    }
    return BOGO_PLUGIN_OK;
}

} // namespace brickgamepro

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version < 8 ||
        api->struct_size < sizeof(BogoPluginApi) || !api->log ||
        !api->so_symbol)
        return -1;
    if (!api->il2cpp)
        return BOGO_PLUGIN_DEFERRED;
    return brickgamepro::init(api);
}
