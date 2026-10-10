#include "plugin_api.h"

#include <SDL2/SDL.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace brickgamepro {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static bool g_enabled = true;
static bool g_touch_enabled = true;
static bool g_touch_hold_refresh = true;
static constexpr float kDesignWidth = 640.0f;
static constexpr float kDesignHeight = 480.0f;
static constexpr float kCanvasScale = 2.2f;
static constexpr int kCanvasOffsetY = -60;
static constexpr float kCanvasPositionY = -1190.0f;
static constexpr float kGridExtraScale = 1.0f;
static constexpr float kWorldCameraOffsetX = 0.0f;
static constexpr float kWorldCameraOffsetY = 10.60f;
static constexpr bool kUseCanvasLayout = true;
static constexpr bool kUseWorldObjectTransform = false;
static constexpr bool kUseWorldCameraTransform = true;
static constexpr Uint32 kMinTouchHoldMs = 35;
static constexpr uint64_t kMinTouchHoldFrames = 1;
static constexpr uint64_t kMinDirectHoldFrames = 2;
static constexpr Uint32 kDownPulseRepeatMs = 95;
static constexpr const char* kCanvasObject = "Canvas";
static constexpr const char* kPackageName = "com.perseusgames.brickgamepro";
static float g_design_width = kDesignWidth;
static float g_design_height = kDesignHeight;
static bool g_skin_switch_enabled = true;
static int g_skin_switch_button = SDL_CONTROLLER_BUTTON_GUIDE;
static bool g_probe_auto_start = false;
static int g_probe_auto_start_stage = 0;
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

struct Color {
    float r;
    float g;
    float b;
    float a;
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
typedef void* (*p_class_get_type)(void* klass);
typedef void* (*p_type_get_object)(void* type);
typedef void* (*p_class_get_fields)(void* klass, void** iter);
typedef const char* (*p_field_get_name)(void* field);
typedef void* (*p_field_get_type)(void* field);
typedef size_t (*p_field_get_offset)(void* field);
typedef char* (*p_type_get_name)(void* type);

typedef uintptr_t (*orig8_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*void_method_t)(void* self, void* mi);
typedef void* (*get_transform_t)(void* component, void* mi);
typedef void* (*transform_get_child_t)(void* transform, int index, void* mi);
typedef int (*transform_get_child_count_t)(void* transform, void* mi);
typedef void* (*transform_get_parent_t)(void* transform, void* mi);
typedef void* (*component_get_game_object_t)(void* component, void* mi);
typedef void* (*gameobject_get_transform_t)(void* game_object, void* mi);
typedef void* (*object_get_name_t)(void* object, void* mi);
typedef void* (*gameobject_get_component_t)(void* game_object, void* type,
                                            void* mi);
typedef void (*gameobject_set_active_t)(void* game_object, bool active,
                                        void* mi);
typedef void (*set_v3_t)(void* transform, Vector3 value, void* mi);
typedef Vector3 (*get_v3_t)(void* transform, void* mi);
typedef void (*set_v2_t)(void* rt, Vector2 value, void* mi);
typedef Vector2 (*get_v2_t)(void* rt, void* mi);
typedef void* (*camera_get_main_t)(void* mi);
typedef float (*get_float_t)(void* object, void* mi);
typedef void (*set_float_t)(void* object, float value, void* mi);
typedef void (*canvas_renderer_set_color_t)(void* renderer, Color value,
                                            void* mi);
typedef int (*screen_get_t)(void* mi);
typedef void* (*find_objects_of_type_t)(void* type, void* mi);

static p_domain_get f_domain_get = nullptr;
static p_domain_get_assemblies f_domain_assemblies = nullptr;
static p_assembly_get_image f_assembly_image = nullptr;
static p_image_get_name f_image_name = nullptr;
static p_class_from_name f_class_from_name = nullptr;
static p_get_method f_get_method = nullptr;
static p_class_get_methods f_class_get_methods = nullptr;
static p_method_get_name f_method_get_name = nullptr;
static p_method_get_param_count f_method_get_param_count = nullptr;
static p_class_get_type f_class_get_type = nullptr;
static p_type_get_object f_type_get_object = nullptr;
static p_class_get_fields f_class_get_fields = nullptr;
static p_field_get_name f_field_get_name = nullptr;
static p_field_get_type f_field_get_type = nullptr;
static p_field_get_offset f_field_get_offset = nullptr;
static p_type_get_name f_type_get_name = nullptr;

static get_transform_t m_component_get_transform = nullptr;
static void* mi_component_get_transform = nullptr;
static transform_get_child_t m_tf_get_child = nullptr;
static void* mi_tf_get_child = nullptr;
static transform_get_child_count_t m_tf_get_child_count = nullptr;
static void* mi_tf_get_child_count = nullptr;
static transform_get_parent_t m_tf_get_parent = nullptr;
static void* mi_tf_get_parent = nullptr;
static component_get_game_object_t m_component_get_game_object = nullptr;
static void* mi_component_get_game_object = nullptr;
static gameobject_get_transform_t m_gameobject_get_transform = nullptr;
static void* mi_gameobject_get_transform = nullptr;
static object_get_name_t m_object_get_name = nullptr;
static void* mi_object_get_name = nullptr;
static gameobject_get_component_t m_gameobject_get_component = nullptr;
static void* mi_gameobject_get_component = nullptr;
static gameobject_set_active_t m_gameobject_set_active = nullptr;
static void* mi_gameobject_set_active = nullptr;
static set_v3_t m_tf_set_local_scale = nullptr;
static void* mi_tf_set_local_scale = nullptr;
static get_v3_t m_tf_get_local_scale = nullptr;
static void* mi_tf_get_local_scale = nullptr;
static get_v3_t m_tf_get_local_position = nullptr;
static void* mi_tf_get_local_position = nullptr;
static set_v3_t m_tf_set_local_position = nullptr;
static void* mi_tf_set_local_position = nullptr;
static set_v2_t m_rt_set_anchored_position = nullptr;
static void* mi_rt_set_anchored_position = nullptr;
static get_v2_t m_rt_get_anchored_position = nullptr;
static void* mi_rt_get_anchored_position = nullptr;
static canvas_renderer_set_color_t m_canvas_renderer_set_color = nullptr;
static void* mi_canvas_renderer_set_color = nullptr;
static void* g_canvas_renderer_type = nullptr;
static screen_get_t m_screen_width = nullptr;
static void* mi_screen_width = nullptr;
static screen_get_t m_screen_height = nullptr;
static void* mi_screen_height = nullptr;
static find_objects_of_type_t m_object_find_objects_of_type = nullptr;
static void* mi_object_find_objects_of_type = nullptr;
static camera_get_main_t m_camera_get_main = nullptr;
static void* mi_camera_get_main = nullptr;
static get_float_t m_camera_get_orthographic_size = nullptr;
static void* mi_camera_get_orthographic_size = nullptr;
static set_float_t m_camera_set_orthographic_size = nullptr;
static void* mi_camera_set_orthographic_size = nullptr;
static void* g_transform_type = nullptr;
static void* g_tetris_manager_type = nullptr;
static void* g_transform_scan_type = nullptr;
static uintptr_t g_game_manager_instance = 0;
static int g_game_manager_current_obj_offset = 0;
static bool g_canvas_layout_ready = false;
static void* g_canvas_rt = nullptr;
static void* g_app_content_rt = nullptr;
static void* g_pad_rt = nullptr;
static void* g_grid_rt = nullptr;
static void* g_feedback_menu_go = nullptr;
static void* g_leave_feedback_go = nullptr;
static int g_last_screen_width = 0;
static int g_last_screen_height = 0;
static bool g_canvas_layout_logged = false;
static uintptr_t g_gui_start_orig = 0;
static uintptr_t g_gui_update_resolution_orig = 0;
static uintptr_t g_gui_manager_instance = 0;
static uintptr_t m_gui_press_start_pause_game = 0;
static void* mi_gui_press_start_pause_game = nullptr;
static uintptr_t g_gui_show_feedback_orig = 0;
static uintptr_t g_gui_leave_feedback_orig = 0;
static uintptr_t g_tetris_update_grid_orig = 0;
static uintptr_t g_tetris_get_transform_at_grid_position_orig = 0;
static uintptr_t g_tetris_five_update_grid_orig = 0;
static uintptr_t g_tetris_five_get_transform_at_grid_position_orig = 0;
static uintptr_t g_tetris_mino_start_orig = 0;
static uintptr_t g_tetris_mino_update_orig = 0;
static uintptr_t g_tetris_five_mino_start_orig = 0;
static uintptr_t g_tetris_five_mino_update_orig = 0;
static uintptr_t g_game_manager_start_orig = 0;
static uintptr_t g_game_manager_update_orig = 0;
static uintptr_t g_object_manager_start_orig = 0;
static uintptr_t g_object_manager_update_orig = 0;
static uintptr_t g_tank_manager_start_orig = 0;
static uintptr_t g_tank_manager_update_orig = 0;
static uintptr_t g_tank_update_grid_orig = 0;
static uintptr_t g_tank_get_transform_at_grid_position_orig = 0;
static uintptr_t g_tank_player_start_orig = 0;
static uintptr_t g_tank_player_update_orig = 0;
static uintptr_t g_tank_enemy_start_orig = 0;
static uintptr_t m_game_manager_start_enter = 0;
static void* mi_game_manager_start_enter = nullptr;
static uintptr_t m_game_manager_end_enter = 0;
static void* mi_game_manager_end_enter = nullptr;
static uintptr_t m_game_manager_start_move_right = 0;
static void* mi_game_manager_start_move_right = nullptr;
static uintptr_t m_game_manager_end_move_right = 0;
static void* mi_game_manager_end_move_right = nullptr;
static uintptr_t m_game_manager_start_move_left = 0;
static void* mi_game_manager_start_move_left = nullptr;
static uintptr_t m_game_manager_end_move_left = 0;
static void* mi_game_manager_end_move_left = nullptr;
static uintptr_t m_game_manager_start_move_up = 0;
static void* mi_game_manager_start_move_up = nullptr;
static uintptr_t m_game_manager_end_move_up = 0;
static void* mi_game_manager_end_move_up = nullptr;
static uintptr_t m_game_manager_start_move_down = 0;
static void* mi_game_manager_start_move_down = nullptr;
static uintptr_t m_game_manager_end_move_down = 0;
static void* mi_game_manager_end_move_down = nullptr;
static uintptr_t m_game_manager_play_tank = 0;
static void* mi_game_manager_play_tank = nullptr;
static bool g_canvas_targets_logged = false;
static Vector3 g_app_content_base_scale = {1.0f, 1.0f, 1.0f};
static Vector3 g_pad_base_scale = {1.0f, 1.0f, 1.0f};
static Vector3 g_grid_base_scale = {1.0f, 1.0f, 1.0f};
static Vector2 g_grid_base_position = {0.0f, 0.0f};
static bool g_app_content_base_scale_ready = false;
static bool g_pad_base_scale_ready = false;
static bool g_grid_base_ready = false;
static bool g_grid_logged = false;
static int g_hierarchy_logged = 0;
static int g_runtime_probe_logged = 0;
static bool g_transform_scan_logged = false;
static int g_transform_scan_requests = 0;
static bool g_tetris_classes_logged = false;
static int g_tetris_update_log_count = 0;
static int g_grid_transform_log_count = 0;
static int g_mino_log_count = 0;
static int g_component_log_count = 0;
static int g_current_game_log_count = 0;
static std::set<void*> g_runtime_seen_nodes;
static std::set<uintptr_t> g_seen_mino_components;

struct TransformBase {
    Vector3 position;
    Vector3 scale;
    bool ready = false;
};

static std::map<void*, TransformBase> g_world_transform_bases;

struct Il2CppArray {
    uintptr_t klass;
    uintptr_t monitor;
    uintptr_t bounds;
    uintptr_t max_length;
    void* vector[0];
};

struct ControlRect {
    const char* name = nullptr;
    void* rt = nullptr;
    void* renderer = nullptr;
    Vector2 base_position = {0.0f, 0.0f};
    Vector3 base_scale = {1.0f, 1.0f, 1.0f};
    bool ready = false;
};

static std::array<ControlRect, 19> g_control_rects = {{
    {"LeftBtn"},
    {"RightBtn"},
    {"UpBtn"},
    {"DownBtn"},
    {"EnterBtn"},
    {"StartPauseBtn"},
    {"AudioBtn"},
    {"SettingBtn"},
    {"ExitBtn"},
    {"UpText"},
    {"DownText"},
    {"LeftText"},
    {"RightText"},
    {"NavigationImage"},
    {"EnterText"},
    {"StartPauseText"},
    {"AudioText"},
    {"SettingText"},
    {"ExitText"},
}};
static bool g_control_rects_logged = false;

struct ManagedString {
    uintptr_t klass;
    uintptr_t monitor;
    int32_t length;
    uint16_t chars[1];
};

static std::string managed_string_utf8(void* value)
{
    if (!value)
        return {};
    const auto* string = reinterpret_cast<const ManagedString*>(value);
    if (string->length < 0 || string->length > 256)
        return {};
    std::string result;
    result.reserve(static_cast<size_t>(string->length));
    for (int32_t i = 0; i < string->length; ++i) {
        const uint16_t ch = string->chars[i];
        result.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return result;
}

static std::string object_name(void* object)
{
    if (!object || !m_object_get_name)
        return {};
    return managed_string_utf8(m_object_get_name(object, mi_object_get_name));
}

static std::string transform_name(void* transform)
{
    if (!transform || !m_component_get_game_object)
        return {};
    void* go = m_component_get_game_object(
        transform, mi_component_get_game_object);
    return object_name(go);
}

static bool plausible_object_ptr(uintptr_t p)
{
    return p > 0x10000 && p < 0x8000000000ULL;
}

static void log_transform_state(const char* tag, void* transform)
{
    if (!g_api || !transform)
        return;
    const std::string name = transform_name(transform);
    std::string parent_name;
    if (m_tf_get_parent) {
        void* parent = m_tf_get_parent(transform, mi_tf_get_parent);
        parent_name = transform_name(parent);
    }
    Vector2 anchored = {};
    Vector3 local_position = {};
    Vector3 scale = {};
    int child_count = -1;
    if (m_rt_get_anchored_position)
        anchored = m_rt_get_anchored_position(
            transform, mi_rt_get_anchored_position);
    if (m_tf_get_local_position)
        local_position = m_tf_get_local_position(
            transform, mi_tf_get_local_position);
    if (m_tf_get_local_scale)
        scale = m_tf_get_local_scale(transform, mi_tf_get_local_scale);
    if (m_tf_get_child_count)
        child_count = m_tf_get_child_count(transform, mi_tf_get_child_count);
    g_api->log("BRICKGAME",
               "%s name=%s parent=%s tf=%p child=%d "
               "anchored=(%.1f,%.1f) local=(%.1f,%.1f,%.1f) "
               "scale=(%.3f,%.3f,%.3f)",
               tag,
               name.empty() ? "?" : name.c_str(),
               parent_name.empty() ? "?" : parent_name.c_str(),
               transform, child_count,
               static_cast<double>(anchored.x),
               static_cast<double>(anchored.y),
               static_cast<double>(local_position.x),
               static_cast<double>(local_position.y),
               static_cast<double>(local_position.z),
               static_cast<double>(scale.x),
               static_cast<double>(scale.y),
               static_cast<double>(scale.z));
}

static void apply_world_grid_transform(void* transform, const char* reason)
{
    if (!kUseWorldObjectTransform)
        return;
    if (!transform || !m_tf_get_local_position || !m_tf_set_local_position ||
        !m_tf_get_local_scale || !m_tf_set_local_scale)
        return;
    TransformBase& base = g_world_transform_bases[transform];
    if (!base.ready) {
        base.position = m_tf_get_local_position(transform,
                                                mi_tf_get_local_position);
        base.scale = m_tf_get_local_scale(transform, mi_tf_get_local_scale);
        base.ready = true;
        if (g_api) {
            g_api->log("BRICKGAME",
                       "world base %s tf=%p local=(%.1f,%.1f,%.1f) "
                       "scale=(%.3f,%.3f,%.3f)",
                       reason ? reason : "?", transform,
                       static_cast<double>(base.position.x),
                       static_cast<double>(base.position.y),
                       static_cast<double>(base.position.z),
                       static_cast<double>(base.scale.x),
                       static_cast<double>(base.scale.y),
                       static_cast<double>(base.scale.z));
        }
    }

    const Vector3 position = base.position;
    const Vector3 scale = {
        base.scale.x * kCanvasScale,
        base.scale.y * kCanvasScale,
        base.scale.z,
    };
    m_tf_set_local_position(transform, position, mi_tf_set_local_position);
    m_tf_set_local_scale(transform, scale, mi_tf_set_local_scale);
}

static void apply_world_camera_transform(const char* reason)
{
    if (!kUseWorldCameraTransform || !m_camera_get_main ||
        !m_camera_get_orthographic_size || !m_camera_set_orthographic_size ||
        !m_component_get_transform || !m_tf_get_local_position ||
        !m_tf_set_local_position)
        return;
    void* camera = m_camera_get_main(mi_camera_get_main);
    if (!camera)
        return;
    void* transform = m_component_get_transform(camera,
                                                mi_component_get_transform);
    if (!transform)
        return;
    static bool base_ready = false;
    static Vector3 base_position = {};
    static float base_size = 0.0f;
    if (!base_ready) {
        base_position = m_tf_get_local_position(transform,
                                                mi_tf_get_local_position);
        base_size = m_camera_get_orthographic_size(
            camera, mi_camera_get_orthographic_size);
        base_ready = true;
        if (g_api) {
            g_api->log("BRICKGAME",
                       "camera base reason=%s pos=(%.2f,%.2f,%.2f) size=%.3f",
                       reason ? reason : "?",
                       static_cast<double>(base_position.x),
                       static_cast<double>(base_position.y),
                       static_cast<double>(base_position.z),
                       static_cast<double>(base_size));
        }
    }
    const Vector3 position = {
        base_position.x + kWorldCameraOffsetX,
        base_position.y + kWorldCameraOffsetY,
        base_position.z,
    };
    m_tf_set_local_position(transform, position, mi_tf_set_local_position);
    m_camera_set_orthographic_size(camera, base_size / kCanvasScale,
                                   mi_camera_set_orthographic_size);
}

static ControlRect* find_control_rect(const std::string& name)
{
    for (auto& rect : g_control_rects) {
        if (rect.name && name == rect.name)
            return &rect;
    }
    return nullptr;
}

struct TouchPoint {
    int button = SDL_CONTROLLER_BUTTON_INVALID;
    int pointer_id = -1;
    float x = 0.0f;
    float y = 0.0f;
    bool active = false;
    bool physical_down = false;
    bool pending_up = false;
    Uint32 down_ticks = 0;
    uint64_t down_frame = 0;
};

struct DirectButtonState {
    int button = SDL_CONTROLLER_BUTTON_INVALID;
    bool active = false;
    bool physical_down = false;
    bool pending_up = false;
    Uint32 down_ticks = 0;
    Uint32 last_pulse_ticks = 0;
    uint64_t down_frame = 0;
};

static std::array<TouchPoint, 8> g_points = {{
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,  222.0f, 375.0f},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 300.0f, 375.0f},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_CONTROLLER_BUTTON_DPAD_UP,    260.0f, 335.0f},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_CONTROLLER_BUTTON_DPAD_DOWN,  260.0f, 414.0f},
    {SDL_CONTROLLER_BUTTON_A,          SDL_CONTROLLER_BUTTON_A,          393.0f, 377.0f},
    {SDL_CONTROLLER_BUTTON_Y,          SDL_CONTROLLER_BUTTON_Y,          393.0f, 377.0f},
    {SDL_CONTROLLER_BUTTON_START,      SDL_CONTROLLER_BUTTON_START,      334.0f, 307.0f},
    {SDL_CONTROLLER_BUTTON_X,          SDL_CONTROLLER_BUTTON_X,          367.0f, 307.0f},
}};
static std::array<DirectButtonState, 6> g_direct_buttons = {{
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {SDL_CONTROLLER_BUTTON_DPAD_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {SDL_CONTROLLER_BUTTON_A},
    {SDL_CONTROLLER_BUTTON_Y},
}};
static uint64_t g_frame_counter = 0;

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

static void* resolve_type_object(void* image, const char* namespaze,
                                 const char* klass)
{
    if (!f_class_from_name || !f_class_get_type || !f_type_get_object)
        return nullptr;
    void* k = f_class_from_name(image, namespaze, klass);
    if (!k)
        return nullptr;
    void* type = f_class_get_type(k);
    return type ? f_type_get_object(type) : nullptr;
}

static bool interesting_runtime_name(const std::string& name);

static void enumerate_class_details(void* image, const char* klass_name)
{
    if (!image || !f_class_from_name || !g_api)
        return;
    void* klass = f_class_from_name(image, "", klass_name);
    if (!klass)
        return;
    g_api->log("BRICKGAME", "class %s found", klass_name);
    if (f_class_get_fields && f_field_get_name) {
        void* iter = nullptr;
        int logged = 0;
        while (void* field = f_class_get_fields(klass, &iter)) {
            const char* field_name = f_field_get_name(field);
            const char* type_name = "?";
            if (f_field_get_type && f_type_get_name) {
                void* type = f_field_get_type(field);
                char* resolved = type ? f_type_get_name(type) : nullptr;
                if (resolved)
                    type_name = resolved;
            }
            const size_t offset = f_field_get_offset
                ? f_field_get_offset(field)
                : 0;
            g_api->log("BRICKGAME", "  field %s.%s type=%s offset=0x%zx",
                       klass_name, field_name ? field_name : "?",
                       type_name, offset);
            if (++logged >= 80)
                break;
        }
    }
    if (f_class_get_methods && f_method_get_name && f_method_get_param_count) {
        void* iter = nullptr;
        int logged = 0;
        while (void* method = f_class_get_methods(klass, &iter)) {
            const char* method_name = f_method_get_name(method);
            if (!method_name)
                continue;
            g_api->log("BRICKGAME", "  method %s.%s(%d)", klass_name,
                       method_name, f_method_get_param_count(method));
            if (++logged >= 120)
                break;
        }
    }
}

static int field_offset_for(void* image, const char* klass_name,
                            const char* field_name)
{
    if (!image || !klass_name || !field_name || !f_class_from_name ||
        !f_class_get_fields || !f_field_get_name || !f_field_get_offset)
        return 0;
    void* klass = f_class_from_name(image, "", klass_name);
    if (!klass)
        return 0;
    void* iter = nullptr;
    while (void* field = f_class_get_fields(klass, &iter)) {
        const char* name = f_field_get_name(field);
        if (name && std::strcmp(name, field_name) == 0)
            return static_cast<int>(f_field_get_offset(field));
    }
    return 0;
}

static void scan_transforms_once(const char* reason)
{
    if (g_transform_scan_requests >= 3 || !m_object_find_objects_of_type ||
        !g_transform_scan_type || !g_api)
        return;
    ++g_transform_scan_requests;
    void* array_object = m_object_find_objects_of_type(
        g_transform_scan_type, mi_object_find_objects_of_type);
    auto* array = reinterpret_cast<Il2CppArray*>(array_object);
    if (!array || array->max_length == 0 || array->max_length > 10000) {
        g_api->log("BRICKGAME",
                   "FindObjectsOfType(Transform) unusable reason=%s array=%p",
                   reason ? reason : "?", array_object);
        return;
    }
    g_transform_scan_logged = true;
    g_api->log("BRICKGAME",
               "FindObjectsOfType(Transform) reason=%s count=%llu",
               reason ? reason : "?",
               static_cast<unsigned long long>(array->max_length));
    int logged = 0;
    for (uintptr_t i = 0; i < array->max_length && logged < 320; ++i) {
        void* transform = array->vector[i];
        if (!transform)
            continue;
        const std::string name = transform_name(transform);
        const bool interesting = interesting_runtime_name(name) ||
            name == "Canvas" || name == "AppContent" || name == "Pad" ||
            name == "Grid" || name == "BlockImage" || name == "Main Camera" ||
            logged < 96;
        if (!interesting)
            continue;
        log_transform_state("transform scan", transform);
        ++logged;
    }
}

static void bind_control_rect(void* rt, const std::string& name)
{
    ControlRect* rect = find_control_rect(name);
    if (!rect || rect->rt == rt)
        return;
    rect->rt = rt;
    rect->ready = false;
    rect->renderer = nullptr;
    if (m_component_get_game_object && m_gameobject_get_component &&
        g_canvas_renderer_type) {
        void* go = m_component_get_game_object(
            rt, mi_component_get_game_object);
        if (go) {
            rect->renderer = m_gameobject_get_component(
                go, g_canvas_renderer_type, mi_gameobject_get_component);
        }
    }
    g_api->log("BRICKGAME", "control rect %s rt=%p renderer=%p",
               rect->name, rect->rt, rect->renderer);
}

static void bind_feedback_node(void* transform, void* game_object,
                               const std::string& name)
{
    if (!game_object)
        return;
    if (name == "FeedBackMenu") {
        g_feedback_menu_go = game_object;
        if (g_api)
            g_api->log("BRICKGAME", "feedback menu go=%p rt=%p",
                       game_object, transform);
    } else if (name == "LeaveFeedBackBtn") {
        g_leave_feedback_go = game_object;
        if (g_api)
            g_api->log("BRICKGAME", "leave feedback go=%p rt=%p",
                       game_object, transform);
    }
}

static void suppress_feedback_ui()
{
    if (!m_gameobject_set_active)
        return;
    if (g_feedback_menu_go)
        m_gameobject_set_active(g_feedback_menu_go, false,
                                mi_gameobject_set_active);
    if (g_leave_feedback_go)
        m_gameobject_set_active(g_leave_feedback_go, false,
                                mi_gameobject_set_active);
}

static void discover_control_rects(void* transform, int depth)
{
    if (!transform || depth > 6 || !m_tf_get_child || !m_tf_get_child_count ||
        !m_component_get_game_object || !m_object_get_name)
        return;
    const int child_count = m_tf_get_child_count(
        transform, mi_tf_get_child_count);
    for (int i = 0; i < child_count && i < 64; ++i) {
        void* child = m_tf_get_child(transform, i, mi_tf_get_child);
        void* child_go = child
            ? m_component_get_game_object(child, mi_component_get_game_object)
            : nullptr;
        void* child_name = child_go
            ? m_object_get_name(child_go, mi_object_get_name)
            : nullptr;
        const std::string name = managed_string_utf8(child_name);
        bind_control_rect(child, name);
        bind_feedback_node(child, child_go, name);
        if (!name.empty() && g_hierarchy_logged < 96) {
            if (depth <= 2 || name == "Grid" || name.find("Grid") != std::string::npos) {
                g_api->log("BRICKGAME", "Canvas node depth=%d name=%s rt=%p",
                           depth + 1, name.c_str(), child);
                ++g_hierarchy_logged;
            }
        }
        if (name == "Grid" && !g_grid_rt) {
            g_grid_rt = child;
            g_grid_base_ready = false;
            g_api->log("BRICKGAME", "Grid RectTransform found rt=%p", g_grid_rt);
        }
        discover_control_rects(child, depth + 1);
    }
}

static bool interesting_runtime_name(const std::string& name)
{
    if (name.empty())
        return false;
    const char* needles[] = {
        "Mino", "mino", "Tetris", "Block", "block", "Brick", "brick",
        "Grid", "grid", "Clone", "clone", "Prefab", "prefab"
    };
    for (const char* needle : needles) {
        if (name.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

static void probe_runtime_nodes(void* transform, int depth,
                                const std::string& path)
{
    if (!transform || depth > 8 || !m_tf_get_child || !m_tf_get_child_count ||
        !m_component_get_game_object || !m_object_get_name ||
        g_runtime_probe_logged >= 256)
        return;

    const int child_count = m_tf_get_child_count(
        transform, mi_tf_get_child_count);
    for (int i = 0; i < child_count && i < 128; ++i) {
        void* child = m_tf_get_child(transform, i, mi_tf_get_child);
        if (!child)
            continue;
        void* child_go = m_component_get_game_object(
            child, mi_component_get_game_object);
        void* child_name = child_go
            ? m_object_get_name(child_go, mi_object_get_name)
            : nullptr;
        const std::string name = managed_string_utf8(child_name);
        std::string child_path = path;
        child_path += "/";
        child_path += name.empty() ? "?" : name;
        if (g_runtime_seen_nodes.insert(child).second) {
            const bool log_node = depth <= 2 || interesting_runtime_name(name);
            if (log_node && g_runtime_probe_logged < 256) {
                Vector2 position = {};
                Vector3 scale = {};
                if (m_rt_get_anchored_position)
                    position = m_rt_get_anchored_position(
                        child, mi_rt_get_anchored_position);
                if (m_tf_get_local_scale)
                    scale = m_tf_get_local_scale(child, mi_tf_get_local_scale);
                g_api->log("BRICKGAME",
                           "runtime node frame=%llu depth=%d path=%s rt=%p "
                           "anchored=(%.1f,%.1f) scale=(%.3f,%.3f,%.3f)",
                           static_cast<unsigned long long>(g_frame_counter),
                           depth + 1, child_path.c_str(), child,
                           static_cast<double>(position.x),
                           static_cast<double>(position.y),
                           static_cast<double>(scale.x),
                           static_cast<double>(scale.y),
                           static_cast<double>(scale.z));
                ++g_runtime_probe_logged;
            }
        }
        probe_runtime_nodes(child, depth + 1, child_path);
    }
}

static void apply_grid_rect()
{
    if (!g_grid_rt || !m_rt_get_anchored_position ||
        !m_rt_set_anchored_position || !m_tf_get_local_scale ||
        !m_tf_set_local_scale)
        return;

    if (!g_grid_base_ready) {
        g_grid_base_position = m_rt_get_anchored_position(
            g_grid_rt, mi_rt_get_anchored_position);
        g_grid_base_scale = m_tf_get_local_scale(
            g_grid_rt, mi_tf_get_local_scale);
        g_grid_base_ready = true;
        g_api->log("BRICKGAME",
                   "Grid base anchored=(%.1f,%.1f) scale=(%.3f,%.3f,%.3f)",
                   static_cast<double>(g_grid_base_position.x),
                   static_cast<double>(g_grid_base_position.y),
                   static_cast<double>(g_grid_base_scale.x),
                   static_cast<double>(g_grid_base_scale.y),
                   static_cast<double>(g_grid_base_scale.z));
    }

    Vector3 grid_scale = g_grid_base_scale;
    if (kGridExtraScale != 1.0f) {
        grid_scale.x *= kGridExtraScale;
        grid_scale.y *= kGridExtraScale;
        grid_scale.z *= kGridExtraScale;
        m_tf_set_local_scale(g_grid_rt, grid_scale, mi_tf_set_local_scale);
        m_rt_set_anchored_position(g_grid_rt, g_grid_base_position,
                                   mi_rt_set_anchored_position);
    }

    if (!g_grid_logged) {
        const Vector2 actual_position = m_rt_get_anchored_position(
            g_grid_rt, mi_rt_get_anchored_position);
        const Vector3 actual_scale = m_tf_get_local_scale(
            g_grid_rt, mi_tf_get_local_scale);
        g_grid_logged = true;
        g_api->log("BRICKGAME",
                   "Grid adjusted extraScale=%.1f anchored=(%.1f,%.1f) "
                   "scale=(%.3f,%.3f,%.3f)",
                   static_cast<double>(kGridExtraScale),
                   static_cast<double>(actual_position.x),
                   static_cast<double>(actual_position.y),
                   static_cast<double>(actual_scale.x),
                   static_cast<double>(actual_scale.y),
                   static_cast<double>(actual_scale.z));
    }
}

static void apply_control_rects()
{
    if (!g_app_content_rt || !m_rt_get_anchored_position ||
        !m_rt_set_anchored_position || !m_tf_get_local_scale ||
        !m_tf_set_local_scale)
        return;

    const Vector2 canvas_position = {0.0f, kCanvasPositionY};
    int applied = 0;
    for (auto& rect : g_control_rects) {
        if (!rect.rt)
            continue;
        if (!rect.ready) {
            rect.base_position = m_rt_get_anchored_position(
                rect.rt, mi_rt_get_anchored_position);
            rect.base_scale = m_tf_get_local_scale(
                rect.rt, mi_tf_get_local_scale);
            rect.ready = true;
        }
        const Vector2 position = {
            (rect.base_position.x - canvas_position.x) / kCanvasScale,
            (rect.base_position.y - canvas_position.y) / kCanvasScale,
        };
        const Vector3 scale = {
            rect.base_scale.x / kCanvasScale,
            rect.base_scale.y / kCanvasScale,
            rect.base_scale.z / kCanvasScale,
        };
        m_rt_set_anchored_position(rect.rt, position,
                                   mi_rt_set_anchored_position);
        m_tf_set_local_scale(rect.rt, scale, mi_tf_set_local_scale);
        if (rect.renderer && m_canvas_renderer_set_color) {
            const Color invisible = {1.0f, 1.0f, 1.0f, 0.0f};
            m_canvas_renderer_set_color(rect.renderer, invisible,
                                        mi_canvas_renderer_set_color);
        }
        ++applied;
    }
    if (!g_control_rects_logged && applied > 0) {
        g_control_rects_logged = true;
        g_api->log("BRICKGAME", "control graphics handled=%d alpha=%d",
                   applied, m_canvas_renderer_set_color ? 1 : 0);
    }
}

static void refresh_hidden_ui()
{
    apply_control_rects();
    suppress_feedback_ui();
}

static void apply_canvas_layout()
{
    if (!g_canvas_layout_ready)
        return;
    const int screen_width = m_screen_width(mi_screen_width);
    const int screen_height = m_screen_height(mi_screen_height);
    if (screen_width <= 0 || screen_height <= 0)
        return;

    if (!g_canvas_rt && !g_app_content_rt && !g_pad_rt)
        return;

    const Vector3 scale = {kCanvasScale, kCanvasScale, 1.0f};
    const Vector2 position = {
        0.0f,
        kCanvasPositionY,
    };

    // The asset contains three Canvas objects. Start with the root Canvas
    // calibration transform: its serialized local scale is ~0.0043, so the
    // target must be a relative scale rather than an absolute (2,2,1).
    const auto apply_target = [&](void* target, const char* name) {
        if (!target)
            return;
        Vector3 target_scale = scale;
        Vector3* base_scale = nullptr;
        bool* base_scale_ready = nullptr;
        if (target == g_app_content_rt) {
            base_scale = &g_app_content_base_scale;
            base_scale_ready = &g_app_content_base_scale_ready;
        } else if (target == g_pad_rt) {
            base_scale = &g_pad_base_scale;
            base_scale_ready = &g_pad_base_scale_ready;
        }
        if (base_scale && base_scale_ready && m_tf_get_local_scale) {
            if (!*base_scale_ready) {
                *base_scale = m_tf_get_local_scale(
                    target, mi_tf_get_local_scale);
                *base_scale_ready = true;
            }
            target_scale = *base_scale;
            target_scale.x *= kCanvasScale;
            target_scale.y *= kCanvasScale;
            target_scale.z *= kCanvasScale;
        }
        m_tf_set_local_scale(target, target_scale, mi_tf_set_local_scale);
        m_rt_set_anchored_position(target, position,
                                   mi_rt_set_anchored_position);
        if (!g_canvas_targets_logged) {
            Vector3 actual_scale = {};
            Vector2 actual_position = {};
            if (m_tf_get_local_scale)
                actual_scale = m_tf_get_local_scale(
                    target, mi_tf_get_local_scale);
            if (m_rt_get_anchored_position)
                actual_position = m_rt_get_anchored_position(
                    target, mi_rt_get_anchored_position);
            g_api->log("BRICKGAME",
                       "Canvas target %s=%p scale=(%.3f,%.3f,%.3f) "
                       "anchored=(%.1f,%.1f)",
                       name, target, static_cast<double>(actual_scale.x),
                       static_cast<double>(actual_scale.y),
                       static_cast<double>(actual_scale.z),
                       static_cast<double>(actual_position.x),
                       static_cast<double>(actual_position.y));
        }
    };
    apply_target(g_app_content_rt, "AppContent");
    apply_target(g_pad_rt, "Pad");
    apply_grid_rect();
    apply_control_rects();
    suppress_feedback_ui();
    g_canvas_targets_logged = true;

    const bool first_or_resized =
        !g_canvas_layout_logged || screen_width != g_last_screen_width ||
        screen_height != g_last_screen_height;
    if (first_or_resized) {
        g_canvas_layout_logged = true;
        g_last_screen_width = screen_width;
        g_last_screen_height = screen_height;
        g_api->log("BRICKGAME",
                   "Canvas branches active screen=%dx%d pos=(%.0f,%.0f) "
                   "scale=%.1f root=%p app=%p pad=%p",
                   screen_width, screen_height,
                   static_cast<double>(position.x),
                   static_cast<double>(position.y),
                   static_cast<double>(kCanvasScale),
                   g_canvas_rt, g_app_content_rt, g_pad_rt);
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
    g_gui_manager_instance = x0;
    if (x0 && m_component_get_transform) {
        void* rt = m_component_get_transform(reinterpret_cast<void*>(x0),
                                             mi_component_get_transform);
        if (rt) {
            g_canvas_rt = rt;
            if (m_tf_get_child && m_tf_get_child_count &&
                m_component_get_game_object && m_object_get_name) {
                const int child_count = m_tf_get_child_count(
                    rt, mi_tf_get_child_count);
                for (int i = 0; i < child_count && i < 16; ++i) {
                    void* child = m_tf_get_child(rt, i, mi_tf_get_child);
                    void* child_go = child
                        ? m_component_get_game_object(
                              child, mi_component_get_game_object)
                        : nullptr;
                    void* child_name = child_go
                        ? m_object_get_name(child_go, mi_object_get_name)
                        : nullptr;
                    const std::string name = managed_string_utf8(child_name);
                    g_api->log("BRICKGAME", "Canvas child[%d] %s=%p",
                               i, name.empty() ? "?" : name.c_str(), child);
                    if (name == "AppContent")
                        g_app_content_rt = child;
                    else if (name == "Pad")
                        g_pad_rt = child;
                }
                g_api->log("BRICKGAME",
                           "GUIManager Canvas root=%p AppContent=%p Pad=%p",
                           g_canvas_rt, g_app_content_rt, g_pad_rt);
                discover_control_rects(g_app_content_rt, 0);
                suppress_feedback_ui();
            }
            apply_canvas_layout();
        }
    }
    return ret;
}

static uintptr_t gui_update_resolution_hook(uintptr_t x0, uintptr_t x1,
                                            uintptr_t x2, uintptr_t x3,
                                            uintptr_t x4, uintptr_t x5,
                                            uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_gui_update_resolution_orig) {
        ret = reinterpret_cast<orig8_t>(g_gui_update_resolution_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    apply_canvas_layout();
    return ret;
}

static uintptr_t tetris_update_grid_hook(uintptr_t x0, uintptr_t x1,
                                         uintptr_t x2, uintptr_t x3,
                                         uintptr_t x4, uintptr_t x5,
                                         uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_update_grid_orig) {
        ret = reinterpret_cast<orig8_t>(g_tetris_update_grid_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (g_tetris_update_log_count < 24) {
        g_api->log("BRICKGAME", "TetrisManager.UpdateGrid self=%p frame=%llu",
                   reinterpret_cast<void*>(x0),
                   static_cast<unsigned long long>(g_frame_counter));
        ++g_tetris_update_log_count;
    }
    return ret;
}

static uintptr_t tetris_five_update_grid_hook(uintptr_t x0, uintptr_t x1,
                                              uintptr_t x2, uintptr_t x3,
                                              uintptr_t x4, uintptr_t x5,
                                              uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_five_update_grid_orig) {
        ret = reinterpret_cast<orig8_t>(g_tetris_five_update_grid_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (g_tetris_update_log_count < 48) {
        g_api->log("BRICKGAME",
                   "TetrisFiveManager.UpdateGrid self=%p arg=0x%llx frame=%llu",
                   reinterpret_cast<void*>(x0),
                   static_cast<unsigned long long>(x1),
                   static_cast<unsigned long long>(g_frame_counter));
        ++g_tetris_update_log_count;
    }
    return ret;
}

static uintptr_t tetris_get_transform_at_grid_position_hook(
    uintptr_t x0, uintptr_t x1, uintptr_t x2, uintptr_t x3,
    uintptr_t x4, uintptr_t x5, uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_get_transform_at_grid_position_orig) {
        ret = reinterpret_cast<orig8_t>(
            g_tetris_get_transform_at_grid_position_orig)(
                x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (g_grid_transform_log_count < 48 && plausible_object_ptr(ret)) {
        g_api->log("BRICKGAME",
                   "GetTransformAtGridPosition self=%p arg1=0x%llx arg2=0x%llx "
                   "ret=%p frame=%llu",
                   reinterpret_cast<void*>(x0),
                   static_cast<unsigned long long>(x1),
                   static_cast<unsigned long long>(x2),
                   reinterpret_cast<void*>(ret),
                   static_cast<unsigned long long>(g_frame_counter));
        log_transform_state("grid position transform", reinterpret_cast<void*>(ret));
        ++g_grid_transform_log_count;
    }
    return ret;
}

static uintptr_t tetris_five_get_transform_at_grid_position_hook(
    uintptr_t x0, uintptr_t x1, uintptr_t x2, uintptr_t x3,
    uintptr_t x4, uintptr_t x5, uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_five_get_transform_at_grid_position_orig) {
        ret = reinterpret_cast<orig8_t>(
            g_tetris_five_get_transform_at_grid_position_orig)(
                x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (g_grid_transform_log_count < 96 && plausible_object_ptr(ret)) {
        g_api->log("BRICKGAME",
                   "Five.GetTransformAtGridPosition self=%p arg1=0x%llx "
                   "ret=%p frame=%llu",
                   reinterpret_cast<void*>(x0),
                   static_cast<unsigned long long>(x1),
                   reinterpret_cast<void*>(ret),
                   static_cast<unsigned long long>(g_frame_counter));
        log_transform_state("five grid position transform",
                            reinterpret_cast<void*>(ret));
        ++g_grid_transform_log_count;
    }
    return ret;
}

static uintptr_t tank_update_grid_hook(uintptr_t x0, uintptr_t x1,
                                       uintptr_t x2, uintptr_t x3,
                                       uintptr_t x4, uintptr_t x5,
                                       uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tank_update_grid_orig) {
        ret = reinterpret_cast<orig8_t>(g_tank_update_grid_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (plausible_object_ptr(x1)) {
        apply_world_grid_transform(reinterpret_cast<void*>(x1),
                                   "TankManager.UpdateGrid.arg");
        if (g_grid_transform_log_count < 140) {
            log_transform_state("TankManager.UpdateGrid.arg",
                                reinterpret_cast<void*>(x1));
            ++g_grid_transform_log_count;
        }
    }
    return ret;
}

static uintptr_t tank_get_transform_at_grid_position_hook(
    uintptr_t x0, uintptr_t x1, uintptr_t x2, uintptr_t x3,
    uintptr_t x4, uintptr_t x5, uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tank_get_transform_at_grid_position_orig) {
        ret = reinterpret_cast<orig8_t>(
            g_tank_get_transform_at_grid_position_orig)(
                x0, x1, x2, x3, x4, x5, x6, x7);
    }
    if (plausible_object_ptr(ret)) {
        apply_world_grid_transform(reinterpret_cast<void*>(ret),
                                   "TankManager.GetTransformAtGridPosition");
        if (g_grid_transform_log_count < 180) {
            log_transform_state("TankManager.GetTransformAtGridPosition",
                                reinterpret_cast<void*>(ret));
            ++g_grid_transform_log_count;
        }
    }
    return ret;
}

static void log_mino_component(const char* tag, uintptr_t self)
{
    if (!self || g_mino_log_count >= 96)
        return;
    const bool first = g_seen_mino_components.insert(self).second;
    if (!first && g_mino_log_count >= 32)
        return;
    void* transform = m_component_get_transform
        ? m_component_get_transform(reinterpret_cast<void*>(self),
                                    mi_component_get_transform)
        : nullptr;
    g_api->log("BRICKGAME", "%s self=%p frame=%llu first=%d",
               tag, reinterpret_cast<void*>(self),
               static_cast<unsigned long long>(g_frame_counter),
               first ? 1 : 0);
    if (transform)
        apply_world_grid_transform(transform, tag);
    if (transform)
        log_transform_state(tag, transform);
    ++g_mino_log_count;
}

static void log_component_probe(const char* tag, uintptr_t self)
{
    if (!self || g_component_log_count >= 160)
        return;
    void* transform = m_component_get_transform
        ? m_component_get_transform(reinterpret_cast<void*>(self),
                                    mi_component_get_transform)
        : nullptr;
    g_api->log("BRICKGAME", "%s self=%p frame=%llu",
               tag, reinterpret_cast<void*>(self),
               static_cast<unsigned long long>(g_frame_counter));
    if (transform && tag && std::strstr(tag, "Tank"))
        apply_world_grid_transform(transform, tag);
    if (transform)
        log_transform_state(tag, transform);
    ++g_component_log_count;
}

static void log_current_game_object(const char* reason)
{
    if (!g_game_manager_instance || !g_game_manager_current_obj_offset ||
        !m_gameobject_get_transform || g_current_game_log_count >= 64)
        return;
    uintptr_t obj = *reinterpret_cast<uintptr_t*>(
        g_game_manager_instance + g_game_manager_current_obj_offset);
    if (!plausible_object_ptr(obj))
        return;
    void* transform = m_gameobject_get_transform(
        reinterpret_cast<void*>(obj), mi_gameobject_get_transform);
    std::string name = object_name(reinterpret_cast<void*>(obj));
    g_api->log("BRICKGAME",
               "currentGameManagerObj reason=%s obj=%p name=%s frame=%llu",
               reason ? reason : "?", reinterpret_cast<void*>(obj),
               name.empty() ? "?" : name.c_str(),
               static_cast<unsigned long long>(g_frame_counter));
    if (transform)
        log_transform_state("current game root", transform);
    ++g_current_game_log_count;
}

#define BRICKGAME_COMPONENT_HOOK(NAME, ORIG, TAG)                         \
static uintptr_t NAME(uintptr_t x0, uintptr_t x1, uintptr_t x2,           \
                      uintptr_t x3, uintptr_t x4, uintptr_t x5,           \
                      uintptr_t x6, uintptr_t x7)                         \
{                                                                         \
    uintptr_t ret = 0;                                                     \
    if (ORIG) {                                                            \
        ret = reinterpret_cast<orig8_t>(ORIG)(                             \
            x0, x1, x2, x3, x4, x5, x6, x7);                               \
    }                                                                     \
    log_component_probe(TAG, x0);                                          \
    return ret;                                                           \
}

static uintptr_t game_manager_start_hook(uintptr_t x0, uintptr_t x1,
                                         uintptr_t x2, uintptr_t x3,
                                         uintptr_t x4, uintptr_t x5,
                                         uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_game_manager_start_orig) {
        ret = reinterpret_cast<orig8_t>(g_game_manager_start_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    g_game_manager_instance = x0;
    log_component_probe("GameManager.Start", x0);
    log_current_game_object("game-manager-start");
    return ret;
}

BRICKGAME_COMPONENT_HOOK(game_manager_update_hook, g_game_manager_update_orig,
                         "GameManager.Update")
BRICKGAME_COMPONENT_HOOK(object_manager_start_hook, g_object_manager_start_orig,
                         "ObjectManager.Start")
BRICKGAME_COMPONENT_HOOK(object_manager_update_hook, g_object_manager_update_orig,
                         "ObjectManager.Update")
BRICKGAME_COMPONENT_HOOK(tank_manager_start_hook, g_tank_manager_start_orig,
                         "TankManager.Start")
BRICKGAME_COMPONENT_HOOK(tank_manager_update_hook, g_tank_manager_update_orig,
                         "TankManager.Update")
BRICKGAME_COMPONENT_HOOK(tank_player_start_hook, g_tank_player_start_orig,
                         "TankPlayer.Start")
BRICKGAME_COMPONENT_HOOK(tank_player_update_hook, g_tank_player_update_orig,
                         "TankPlayer.Update")
BRICKGAME_COMPONENT_HOOK(tank_enemy_start_hook, g_tank_enemy_start_orig,
                         "TankEnemy.Start")

static uintptr_t tetris_mino_start_hook(uintptr_t x0, uintptr_t x1,
                                        uintptr_t x2, uintptr_t x3,
                                        uintptr_t x4, uintptr_t x5,
                                        uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_mino_start_orig) {
        ret = reinterpret_cast<orig8_t>(g_tetris_mino_start_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    log_mino_component("TetrisMino.Start", x0);
    return ret;
}

static uintptr_t tetris_mino_update_hook(uintptr_t x0, uintptr_t x1,
                                         uintptr_t x2, uintptr_t x3,
                                         uintptr_t x4, uintptr_t x5,
                                         uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_mino_update_orig) {
        ret = reinterpret_cast<orig8_t>(g_tetris_mino_update_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    log_mino_component("TetrisMino.Update", x0);
    return ret;
}

static uintptr_t tetris_five_mino_start_hook(uintptr_t x0, uintptr_t x1,
                                             uintptr_t x2, uintptr_t x3,
                                             uintptr_t x4, uintptr_t x5,
                                             uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_five_mino_start_orig) {
        ret = reinterpret_cast<orig8_t>(g_tetris_five_mino_start_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    log_mino_component("TetrisFiveMino.Start", x0);
    return ret;
}

static uintptr_t tetris_five_mino_update_hook(uintptr_t x0, uintptr_t x1,
                                              uintptr_t x2, uintptr_t x3,
                                              uintptr_t x4, uintptr_t x5,
                                              uintptr_t x6, uintptr_t x7)
{
    uintptr_t ret = 0;
    if (g_tetris_five_mino_update_orig) {
        ret = reinterpret_cast<orig8_t>(g_tetris_five_mino_update_orig)(
            x0, x1, x2, x3, x4, x5, x6, x7);
    }
    log_mino_component("TetrisFiveMino.Update", x0);
    return ret;
}

static bool resolve_game_manager_method(void* image, const char* name,
                                        uintptr_t* fn_out, void** mi_out)
{
    void* fn = nullptr;
    if (!resolve_call(image, "", "GameManager", name, 0, &fn, mi_out))
        return false;
    *fn_out = reinterpret_cast<uintptr_t>(fn);
    g_api->log("BRICKGAME", "resolved GameManager.%s=%p", name, fn);
    return true;
}

static bool resolve_gui_method(void* image, const char* name,
                               uintptr_t* fn_out, void** mi_out)
{
    void* fn = nullptr;
    if (!resolve_call(image, "", "GUIManager", name, 0, &fn, mi_out))
        return false;
    *fn_out = reinterpret_cast<uintptr_t>(fn);
    g_api->log("BRICKGAME", "resolved GUIManager.%s=%p", name, fn);
    return true;
}

static uintptr_t gui_show_feedback_hook(uintptr_t, uintptr_t, uintptr_t,
                                        uintptr_t, uintptr_t, uintptr_t,
                                        uintptr_t, uintptr_t)
{
    suppress_feedback_ui();
    if (g_api)
        g_api->log("BRICKGAME", "blocked GUIManager.ShowFeedBackMenu");
    return 0;
}

static uintptr_t gui_leave_feedback_hook(uintptr_t, uintptr_t, uintptr_t,
                                         uintptr_t, uintptr_t, uintptr_t,
                                         uintptr_t, uintptr_t)
{
    suppress_feedback_ui();
    if (g_api)
        g_api->log("BRICKGAME", "blocked GUIManager.LeaveFeedBack");
    return 0;
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
    f_class_get_type = reinterpret_cast<p_class_get_type>(
        g_api->so_symbol(il2cpp, "il2cpp_class_get_type"));
    f_type_get_object = reinterpret_cast<p_type_get_object>(
        g_api->so_symbol(il2cpp, "il2cpp_type_get_object"));
    f_class_get_fields = reinterpret_cast<p_class_get_fields>(
        g_api->so_symbol(il2cpp, "il2cpp_class_get_fields"));
    f_field_get_name = reinterpret_cast<p_field_get_name>(
        g_api->so_symbol(il2cpp, "il2cpp_field_get_name"));
    f_field_get_type = reinterpret_cast<p_field_get_type>(
        g_api->so_symbol(il2cpp, "il2cpp_field_get_type"));
    f_field_get_offset = reinterpret_cast<p_field_get_offset>(
        g_api->so_symbol(il2cpp, "il2cpp_field_get_offset"));
    f_type_get_name = reinterpret_cast<p_type_get_name>(
        g_api->so_symbol(il2cpp, "il2cpp_type_get_name"));
    if (!f_domain_get || !f_domain_assemblies || !f_assembly_image ||
        !f_image_name || !f_class_from_name || !f_get_method) {
        g_api->log("BRICKGAME", "missing il2cpp exports; Canvas layout disabled");
        return;
    }

    void* core = find_image("UnityEngine.CoreModule");
    if (!core)
        core = find_image("UnityEngine");
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
    ok &= resolve_call(core, "UnityEngine", "Transform", "GetChild", 1,
                       reinterpret_cast<void**>(&m_tf_get_child),
                       &mi_tf_get_child);
    ok &= resolve_call(core, "UnityEngine", "Transform", "get_childCount", 0,
                       reinterpret_cast<void**>(&m_tf_get_child_count),
                       &mi_tf_get_child_count);
    ok &= resolve_call(core, "UnityEngine", "Transform", "get_parent", 0,
                       reinterpret_cast<void**>(&m_tf_get_parent),
                       &mi_tf_get_parent);
    ok &= resolve_call(core, "UnityEngine", "Component",
                       "get_gameObject", 0,
                       reinterpret_cast<void**>(
                           &m_component_get_game_object),
                       &mi_component_get_game_object);
    ok &= resolve_call(core, "UnityEngine", "GameObject", "get_transform", 0,
                       reinterpret_cast<void**>(&m_gameobject_get_transform),
                       &mi_gameobject_get_transform);
    ok &= resolve_call(core, "UnityEngine", "GameObject", "SetActive", 1,
                       reinterpret_cast<void**>(&m_gameobject_set_active),
                       &mi_gameobject_set_active);
    ok &= resolve_call(core, "UnityEngine", "Object", "get_name", 0,
                       reinterpret_cast<void**>(&m_object_get_name),
                       &mi_object_get_name);
    ok &= resolve_call(core, "UnityEngine", "Transform", "set_localScale",
                       1, reinterpret_cast<void**>(&m_tf_set_local_scale),
                       &mi_tf_set_local_scale);
    ok &= resolve_call(core, "UnityEngine", "Transform", "get_localScale",
                       0, reinterpret_cast<void**>(&m_tf_get_local_scale),
                       &mi_tf_get_local_scale);
    ok &= resolve_call(core, "UnityEngine", "Transform", "get_localPosition",
                       0, reinterpret_cast<void**>(&m_tf_get_local_position),
                       &mi_tf_get_local_position);
    ok &= resolve_call(core, "UnityEngine", "Transform", "set_localPosition",
                       1, reinterpret_cast<void**>(&m_tf_set_local_position),
                       &mi_tf_set_local_position);
    ok &= resolve_call(core, "UnityEngine", "RectTransform",
                       "get_anchoredPosition", 0,
                       reinterpret_cast<void**>(&m_rt_get_anchored_position),
                       &mi_rt_get_anchored_position);
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
    ok &= resolve_call(core, "UnityEngine", "Camera", "get_main", 0,
                       reinterpret_cast<void**>(&m_camera_get_main),
                       &mi_camera_get_main);
    ok &= resolve_call(core, "UnityEngine", "Camera", "get_orthographicSize",
                       0, reinterpret_cast<void**>(
                           &m_camera_get_orthographic_size),
                       &mi_camera_get_orthographic_size);
    ok &= resolve_call(core, "UnityEngine", "Camera", "set_orthographicSize",
                       1, reinterpret_cast<void**>(
                           &m_camera_set_orthographic_size),
                       &mi_camera_set_orthographic_size);
    if (!resolve_call(core, "UnityEngine", "Resources",
                      "FindObjectsOfTypeAll", 1,
                      reinterpret_cast<void**>(&m_object_find_objects_of_type),
                      &mi_object_find_objects_of_type)) {
        resolve_call(core, "UnityEngine", "Object", "FindObjectsOfType", 1,
                     reinterpret_cast<void**>(&m_object_find_objects_of_type),
                     &mi_object_find_objects_of_type);
    }

    g_transform_type = resolve_type_object(core, "UnityEngine", "Transform");
    g_transform_scan_type = g_transform_type;
    g_tetris_manager_type = resolve_type_object(asm_image, "", "TetrisManager");
    g_game_manager_current_obj_offset = field_offset_for(
        asm_image, "GameManager", "currentGameManagerObj");
    g_api->log("BRICKGAME", "GameManager.currentGameManagerObj offset=0x%x",
               g_game_manager_current_obj_offset);
    resolve_game_manager_method(asm_image, "StartMoveRight",
                                &m_game_manager_start_move_right,
                                &mi_game_manager_start_move_right);
    resolve_game_manager_method(asm_image, "EndMoveRight",
                                &m_game_manager_end_move_right,
                                &mi_game_manager_end_move_right);
    resolve_game_manager_method(asm_image, "StartMoveLeft",
                                &m_game_manager_start_move_left,
                                &mi_game_manager_start_move_left);
    resolve_game_manager_method(asm_image, "EndMoveLeft",
                                &m_game_manager_end_move_left,
                                &mi_game_manager_end_move_left);
    resolve_game_manager_method(asm_image, "StartMoveUp",
                                &m_game_manager_start_move_up,
                                &mi_game_manager_start_move_up);
    resolve_game_manager_method(asm_image, "EndMoveUp",
                                &m_game_manager_end_move_up,
                                &mi_game_manager_end_move_up);
    resolve_game_manager_method(asm_image, "StartMoveDown",
                                &m_game_manager_start_move_down,
                                &mi_game_manager_start_move_down);
    resolve_game_manager_method(asm_image, "EndMoveDown",
                                &m_game_manager_end_move_down,
                                &mi_game_manager_end_move_down);
    resolve_game_manager_method(asm_image, "StartEnter",
                                &m_game_manager_start_enter,
                                &mi_game_manager_start_enter);
    resolve_game_manager_method(asm_image, "EndEnter",
                                &m_game_manager_end_enter,
                                &mi_game_manager_end_enter);
    resolve_gui_method(asm_image, "PressStartPauseGame",
                       &m_gui_press_start_pause_game,
                       &mi_gui_press_start_pause_game);
    if (g_probe_auto_start && !g_tetris_classes_logged) {
        g_tetris_classes_logged = true;
        enumerate_class_details(asm_image, "TetrisManager");
        enumerate_class_details(asm_image, "TetrisMino");
        enumerate_class_details(asm_image, "TetrisFiveManager");
        enumerate_class_details(asm_image, "TetrisFiveMino");
        enumerate_class_details(asm_image, "GameManager");
        enumerate_class_details(asm_image, "ObjectManager");
        enumerate_class_details(asm_image, "TankManager");
        enumerate_class_details(asm_image, "TankPlayerController");
        enumerate_class_details(asm_image, "TankEnemyController");
    }

    bool alpha_ok = false;
    if (f_class_get_type && f_type_get_object) {
        void* canvas_renderer_image = find_class_image(
            "UnityEngine", "CanvasRenderer");
        if (!canvas_renderer_image)
            canvas_renderer_image = core;
        g_canvas_renderer_type = resolve_type_object(
            canvas_renderer_image, "UnityEngine", "CanvasRenderer");
        alpha_ok = g_canvas_renderer_type &&
            resolve_call(core, "UnityEngine", "GameObject", "GetComponent",
                         1, reinterpret_cast<void**>(
                                &m_gameobject_get_component),
                         &mi_gameobject_get_component) &&
            resolve_call(canvas_renderer_image, "UnityEngine",
                         "CanvasRenderer", "SetColor", 1,
                         reinterpret_cast<void**>(
                             &m_canvas_renderer_set_color),
                         &mi_canvas_renderer_set_color);
    }
    if (!alpha_ok)
        g_api->log("BRICKGAME", "control alpha hiding unavailable");

    bool hooked = false;
    bool resolution_hooked = false;
    if (ok) {
        hooked = hook_method(asm_image, "", "GUIManager", "Start", 0,
                             reinterpret_cast<uintptr_t>(&gui_start_hook),
                             &g_gui_start_orig);
        if (!hooked) {
            hooked = hook_method(asm_image, "", "GUIManager", "Awake", 0,
                                 reinterpret_cast<uintptr_t>(&gui_start_hook),
                                 &g_gui_start_orig);
        }
        hook_method(asm_image, "", "GameManager", "Start", 0,
                    reinterpret_cast<uintptr_t>(&game_manager_start_hook),
                    &g_game_manager_start_orig);
        resolution_hooked = hook_method(
            asm_image, "", "GUIManager", "UpdateResolution", 0,
            reinterpret_cast<uintptr_t>(&gui_update_resolution_hook),
            &g_gui_update_resolution_orig);
        hook_method(asm_image, "", "GUIManager", "ShowFeedBackMenu", 0,
                    reinterpret_cast<uintptr_t>(&gui_show_feedback_hook),
                    &g_gui_show_feedback_orig);
        hook_method(asm_image, "", "GUIManager", "LeaveFeedBack", 0,
                    reinterpret_cast<uintptr_t>(&gui_leave_feedback_hook),
                    &g_gui_leave_feedback_orig);
        if (g_probe_auto_start) {
            void* play_tank_fn = nullptr;
            if (resolve_call(asm_image, "", "GameManager", "PlayTank", 0,
                             &play_tank_fn, &mi_game_manager_play_tank)) {
                m_game_manager_play_tank =
                    reinterpret_cast<uintptr_t>(play_tank_fn);
                g_api->log("BRICKGAME", "resolved GameManager.PlayTank=%p",
                           play_tank_fn);
            }
            hook_method(asm_image, "", "TankPlayerController", "Start", 0,
                        reinterpret_cast<uintptr_t>(&tank_player_start_hook),
                        &g_tank_player_start_orig);
            hook_method(asm_image, "", "TankEnemyController", "Start", 0,
                        reinterpret_cast<uintptr_t>(&tank_enemy_start_hook),
                        &g_tank_enemy_start_orig);
        }
    }
    g_canvas_layout_ready = ok && hooked;
    g_api->log("BRICKGAME",
               "Canvas RectTransform layout %s gui=%d resolution=%d alpha=%d",
               g_canvas_layout_ready ? "armed" : "disabled",
               hooked ? 1 : 0, resolution_hooked ? 1 : 0,
               alpha_ok ? 1 : 0);
}

static bool invoke_game_manager(uintptr_t fn, void* mi, const char* name)
{
    if (!g_game_manager_instance || !fn || !mi)
        return false;
    reinterpret_cast<void_method_t>(fn)(
        reinterpret_cast<void*>(g_game_manager_instance), mi);
    if (g_api) {
        g_api->log("BRICKGAME", "direct GameManager.%s frame=%llu",
                   name ? name : "?",
                   static_cast<unsigned long long>(g_frame_counter));
    }
    return true;
}

static bool invoke_gui_manager(uintptr_t fn, void* mi, const char* name)
{
    if (!g_gui_manager_instance || !fn || !mi)
        return false;
    reinterpret_cast<void_method_t>(fn)(
        reinterpret_cast<void*>(g_gui_manager_instance), mi);
    if (g_api) {
        g_api->log("BRICKGAME", "direct GUIManager.%s frame=%llu",
                   name ? name : "?",
                   static_cast<unsigned long long>(g_frame_counter));
    }
    return true;
}

static DirectButtonState* find_direct_button_state(int button)
{
    for (auto& state : g_direct_buttons) {
        if (state.button == button)
            return &state;
    }
    return nullptr;
}

static bool direct_button_method(int button, bool down, uintptr_t* fn,
                                 void** mi, const char** name)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        *fn = down ? m_game_manager_start_move_left
                   : m_game_manager_end_move_left;
        *mi = down ? mi_game_manager_start_move_left
                   : mi_game_manager_end_move_left;
        *name = down ? "StartMoveLeft" : "EndMoveLeft";
        return true;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        *fn = down ? m_game_manager_start_move_right
                   : m_game_manager_end_move_right;
        *mi = down ? mi_game_manager_start_move_right
                   : mi_game_manager_end_move_right;
        *name = down ? "StartMoveRight" : "EndMoveRight";
        return true;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        *fn = down ? m_game_manager_start_move_up
                   : m_game_manager_end_move_up;
        *mi = down ? mi_game_manager_start_move_up
                   : mi_game_manager_end_move_up;
        *name = down ? "StartMoveUp" : "EndMoveUp";
        return true;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        *fn = down ? m_game_manager_start_move_down
                   : m_game_manager_end_move_down;
        *mi = down ? mi_game_manager_start_move_down
                   : mi_game_manager_end_move_down;
        *name = down ? "StartMoveDown" : "EndMoveDown";
        return true;
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_Y:
        *fn = down ? m_game_manager_start_enter : m_game_manager_end_enter;
        *mi = down ? mi_game_manager_start_enter : mi_game_manager_end_enter;
        *name = down ? "StartEnter" : "EndEnter";
        return true;
    default:
        return false;
    }
}

static bool release_direct_button(DirectButtonState& state)
{
    uintptr_t fn = 0;
    void* mi = nullptr;
    const char* name = nullptr;
    if (!direct_button_method(state.button, false, &fn, &mi, &name))
        return false;
    if (!invoke_game_manager(fn, mi, name))
        return false;
    state.active = false;
    state.pending_up = false;
    return true;
}

static bool start_down_pulse(DirectButtonState& state)
{
    if (state.active)
        return true;
    uintptr_t fn = 0;
    void* mi = nullptr;
    const char* name = nullptr;
    if (!direct_button_method(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true,
                              &fn, &mi, &name))
        return false;
    if (!invoke_game_manager(fn, mi, name))
        return false;
    const Uint32 now = SDL_GetTicks();
    state.active = true;
    state.pending_up = true;
    state.down_ticks = now;
    state.last_pulse_ticks = now;
    state.down_frame = g_frame_counter;
    return true;
}

static bool handle_direct_button(int button, bool down)
{
    (void)button;
    (void)down;
    // Direct managed input is disabled for now: real-device testing showed it
    // broke menu/gameplay routing for D-pad/A and made B exit the process.
    return false;

    if (button == SDL_CONTROLLER_BUTTON_B) {
        if (!down)
            return false;
        return invoke_gui_manager(m_gui_press_start_pause_game,
                                  mi_gui_press_start_pause_game,
                                  "PressStartPauseGame");
    }

    DirectButtonState* state = find_direct_button_state(button);
    if (!state)
        return false;
    if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        state->physical_down = down;
        if (down)
            return start_down_pulse(*state);
        if (state->active)
            state->pending_up = true;
        return true;
    }
    uintptr_t fn = 0;
    void* mi = nullptr;
    const char* name = nullptr;
    if (!direct_button_method(button, down, &fn, &mi, &name))
        return false;
    if (down) {
        state->physical_down = true;
        state->pending_up = false;
        if (state->active)
            return true;
        if (!invoke_game_manager(fn, mi, name))
            return false;
        state->active = true;
        state->down_ticks = SDL_GetTicks();
        state->down_frame = g_frame_counter;
        return true;
    }
    state->physical_down = false;
    if (!state->active)
        return true;
    const Uint32 elapsed_ms = SDL_GetTicks() - state->down_ticks;
    const uint64_t elapsed_frames = g_frame_counter - state->down_frame;
    if (elapsed_ms < kMinTouchHoldMs ||
        elapsed_frames < kMinDirectHoldFrames) {
        state->pending_up = true;
        if (g_api) {
            const char* button_name = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(button));
            g_api->log("BRICKGAME",
                       "%s direct up deferred hold=%ums frames=%llu",
                       button_name ? button_name : "?",
                       static_cast<unsigned>(elapsed_ms),
                       static_cast<unsigned long long>(elapsed_frames));
        }
        return true;
    }
    return release_direct_button(*state);
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
    if (handle_direct_button(button, down != 0))
        return;
    if (!g_touch_enabled || !g_api || !g_api->inject_touch)
        return;
    for (auto& point : g_points) {
        if (point.button != button)
            continue;
        const bool is_down = down != 0;
        const char* button_name = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(button));
        if (is_down) {
            point.physical_down = true;
            if (point.pending_up) {
                point.pending_up = false;
                g_api->log("BRICKGAME", "%s down cancels deferred up",
                           button_name ? button_name : "?");
            }
            if (point.active)
                return;
        } else {
            point.physical_down = false;
            if (!point.active)
                return;
            point.pending_up = true;
            g_api->log("BRICKGAME",
                       "%s up deferred touch=(%.1f,%.1f)",
                       button_name ? button_name : "?",
                       static_cast<double>(point.x),
                       static_cast<double>(point.y));
            return;
        }
        if (g_api->inject_touch(point.pointer_id, point.x, point.y,
                                g_design_width, g_design_height,
                                1, "brickgamepro")) {
            refresh_hidden_ui();
            point.active = true;
            point.pending_up = false;
            point.down_ticks = SDL_GetTicks();
            point.down_frame = g_frame_counter;
            g_api->log("BRICKGAME",
                       "%s %s touch=(%.1f,%.1f)",
                       button_name ? button_name : "?",
                       "down",
                       static_cast<double>(point.x),
                       static_cast<double>(point.y));
        }
        return;
    }
}

static void on_frame(void*)
{
    ++g_frame_counter;
    if (g_probe_auto_start && g_api && g_api->inject_touch &&
        g_probe_auto_start_stage < 2) {
        if (g_frame_counter == 90 && g_game_manager_instance &&
            m_game_manager_play_tank && mi_game_manager_play_tank) {
            reinterpret_cast<void_method_t>(m_game_manager_play_tank)(
                reinterpret_cast<void*>(g_game_manager_instance),
                mi_game_manager_play_tank);
            g_probe_auto_start_stage = 2;
            g_api->log("BRICKGAME", "probe auto-start PlayTank()");
        } else if (g_frame_counter == 140 && g_probe_auto_start_stage == 0) {
            g_api->inject_touch(31, 334.0f, 307.0f,
                                g_design_width, g_design_height,
                                1, "brickgamepro-probe-start");
            g_probe_auto_start_stage = 1;
            g_api->log("BRICKGAME", "probe fallback touch down");
        } else if (g_frame_counter == 146 && g_probe_auto_start_stage == 1) {
            g_api->inject_touch(31, 334.0f, 307.0f,
                                g_design_width, g_design_height,
                                0, "brickgamepro-probe-start");
            g_probe_auto_start_stage = 2;
            g_api->log("BRICKGAME", "probe fallback touch up");
        }
    }
    if (g_probe_auto_start && g_frame_counter == 8)
        scan_transforms_once("early");
    if (g_probe_auto_start && g_frame_counter == 170)
        scan_transforms_once("after-start");
    if (g_probe_auto_start &&
        (g_frame_counter == 110 || g_frame_counter == 140 ||
         g_frame_counter == 170 || g_frame_counter == 220 ||
         g_frame_counter == 300)) {
        log_current_game_object("frame-probe");
    }
    apply_world_camera_transform("frame");
    if (g_canvas_rt && g_frame_counter <= 600 &&
        (g_frame_counter <= 10 || (g_frame_counter % 30) == 0)) {
        probe_runtime_nodes(g_canvas_rt, 0, "Canvas");
        probe_runtime_nodes(g_app_content_rt, 0, "AppContent");
        probe_runtime_nodes(g_pad_rt, 0, "Pad");
    }
    const int skin = g_pending_skin.exchange(-1);
    if (skin >= 0)
        invoke_skin(skin);
    apply_canvas_layout();
    for (auto& state : g_direct_buttons) {
        if (state.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            if (state.active && state.pending_up) {
                const Uint32 elapsed_ms = SDL_GetTicks() - state.down_ticks;
                const uint64_t elapsed_frames =
                    g_frame_counter - state.down_frame;
                if (elapsed_ms >= kMinTouchHoldMs &&
                    elapsed_frames >= kMinDirectHoldFrames) {
                    release_direct_button(state);
                }
            }
            const Uint32 since_pulse = SDL_GetTicks() - state.last_pulse_ticks;
            if (state.physical_down && !state.active &&
                since_pulse >= kDownPulseRepeatMs) {
                start_down_pulse(state);
            }
            continue;
        }
        if (!state.active || !state.pending_up)
            continue;
        const Uint32 elapsed_ms = SDL_GetTicks() - state.down_ticks;
        const uint64_t elapsed_frames = g_frame_counter - state.down_frame;
        if (elapsed_ms < kMinTouchHoldMs ||
            elapsed_frames < kMinDirectHoldFrames) {
            continue;
        }
        release_direct_button(state);
    }
    if (!g_enabled || !g_touch_enabled || !g_touch_hold_refresh ||
        !g_api || !g_api->inject_touch)
        return;
    for (auto& point : g_points) {
        if (!point.active)
            continue;
        g_api->inject_touch(point.pointer_id, point.x, point.y,
                            g_design_width, g_design_height,
                            1, "brickgamepro");
        refresh_hidden_ui();
        if (!point.pending_up)
            continue;
        const Uint32 elapsed_ms = SDL_GetTicks() - point.down_ticks;
        const uint64_t elapsed_frames = g_frame_counter - point.down_frame;
        if (elapsed_ms < kMinTouchHoldMs ||
            elapsed_frames < kMinTouchHoldFrames) {
            continue;
        }
        if (g_api->inject_touch(point.pointer_id, point.x, point.y,
                                g_design_width, g_design_height,
                                0, "brickgamepro")) {
            refresh_hidden_ui();
            const char* button_name = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(point.button));
            point.active = false;
            point.pending_up = false;
            g_api->log("BRICKGAME",
                       "%s up touch=(%.1f,%.1f) hold=%ums frames=%llu",
                       button_name ? button_name : "?",
                       static_cast<double>(point.x),
                       static_cast<double>(point.y),
                       static_cast<unsigned>(elapsed_ms),
                       static_cast<unsigned long long>(elapsed_frames));
        }
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

    g_api->log("BRICKGAME", "Canvas layout enabled; GPU viewport disabled");
    if (g_api->getenv) {
        const char* probe_start = g_api->getenv("BRICKGAME_PROBE_START");
        g_probe_auto_start = probe_start && *probe_start &&
            std::strcmp(probe_start, "0") != 0;
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

    if (g_api->register_il2cpp_post_init) {
        if (!g_api->register_il2cpp_post_init(&post_il2cpp_init, nullptr))
            g_api->log("BRICKGAME", "failed to register Canvas layout callback");
    } else {
        g_api->log("BRICKGAME", "Canvas layout unavailable: no post-init API");
    }

    g_api->log("BRICKGAME",
               "plugin armed touch=%d refresh=%d minHold=%ums/%lluf "
               "directInput=0 "
               "design=%.0fx%.0f canvasScale=%.1f canvasY=%.0f",
               g_touch_enabled ? 1 : 0,
               g_touch_hold_refresh ? 1 : 0,
               static_cast<unsigned>(kMinTouchHoldMs),
               static_cast<unsigned long long>(kMinTouchHoldFrames),
               static_cast<double>(g_design_width),
               static_cast<double>(g_design_height),
               static_cast<double>(kCanvasScale),
               static_cast<double>(kCanvasPositionY));
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
