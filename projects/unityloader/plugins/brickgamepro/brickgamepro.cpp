#include "plugin_il2cpp.h"
#include "gameover_icons.h"
#include "exit_icons.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace brickgamepro {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static constexpr float kDesignWidth = 640.0f;
static constexpr float kDesignHeight = 480.0f;
static constexpr float kCanvasScale = 2.2f;
static constexpr float kCanvasPositionY = -1190.0f;
static constexpr float kWorldCameraOffsetX = 0.0f;
// One world unit is one cell. Lowering the camera moves dynamic blocks up.
static constexpr float kWorldCameraOffsetY = 10.55f;
static constexpr Uint32 kMinTouchHoldMs = 35;
static constexpr uint64_t kMinTouchHoldFrames = 1;
static constexpr const char* kCanvasObject = "Canvas";
static constexpr const char* kPackageName = "com.perseusgames.brickgamepro";
static constexpr const char* kStartupVideo = "res/drawable/splashvideo.mp4";
static bool g_startup_scene_reported = false;
static int g_startup_menu = -1;
static float g_design_width = kDesignWidth;
static float g_design_height = kDesignHeight;
static bool g_probe_auto_start = false;
static const char* g_probe_game = "Tank";
static bool g_down_fix_enabled = true;
static bool g_rotate_fix_enabled = true;
static bool g_down_trace = false;
static bool g_tank_speed_fix = true;
static bool g_font_trace = false;
static bool g_font_fix_enabled = true;
static bool g_ui_trace = false;
static std::atomic<bool> g_finish_visible{false};
static std::atomic<bool> g_pending_finish_close{false};
static bool g_finish_a_owned = false; // SDL edge ownership survives closing.
static std::atomic<bool> g_exit_visible{false};
static std::atomic<bool> g_pending_guide{false};
static std::atomic<bool> g_pending_start{false};
static std::atomic<int> g_pending_exit_action{0}; // 1: Yes, 2: No.
static bool g_exit_a_owned = false;
static bool g_exit_b_owned = false;
static constexpr float kDownInitialDelay = 0.100f;
static constexpr float kDownRepeatDelay = 0.030f;

// SDL publishes edges only. All managed reads/calls run in CheckUserInput.
static std::atomic<bool> g_down_gameplay_ready{false};
static std::atomic<bool> g_down_gameplay_running{false};
static std::atomic<Uint32> g_down_update_ticks{0};
// MoveDown can re-enter FinishGame on the game thread while holding this lock.
static std::recursive_mutex g_down_mutex;
static bool g_down_owned = false;
static bool g_down_held = false;
static unsigned g_down_taps = 0;
static bool g_down_repeat_armed = false;
static float g_down_next_repeat = 0;
static std::atomic<bool> g_rotate_gameplay_ready{false};
static bool g_rotate_a_owned = false;
static bool g_rotate_y_owned = false;
static unsigned g_rotate_taps = 0; // Protected by g_down_mutex, bound to this mino.

static int g_probe_auto_start_stage = 0;
static bool g_probe_logo_dismissed = false;
static int g_probe_menu_offset = 0;
static int g_probe_selection_offset = 0;

struct Vector2 {
    float x;
    float y;
};

struct Vector3 {
    float x;
    float y;
    float z;
};

struct Rect { float x, y, width, height; };

struct Color {
    float r;
    float g;
    float b;
    float a;
};

typedef uintptr_t (*orig8_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*void_method_t)(void* self, void* mi);
struct DownMinoBinding {
    const char* name;
    int status = 0, immediate = 0, hold_timer = 0, repeat_timer = 0;
    int single = 0;
    void_method_t move = nullptr, move_single = nullptr;
    void_method_t rotate = nullptr;
    void* rotate_mi = nullptr;
    void* move_mi = nullptr;
    void* single_mi = nullptr;
    uintptr_t check_orig = 0;
    uintptr_t start_orig = 0;
    uintptr_t active_self = 0; // newest spawned mino; retired bombs can Update
    bool ready = false;
};
static DownMinoBinding g_down_mino{"TetrisMino"};
static DownMinoBinding g_down_five{"TetrisFiveMino"};
static int g_game_status_offset = 0;
static float (*m_time)(void*, void*) = nullptr;
static void* mi_time = nullptr;
static float (*m_time_scale)(void*, void*) = nullptr;
static void* mi_time_scale = nullptr;
static bool (*m_behaviour_enabled)(void*, void*) = nullptr;
static void* mi_behaviour_enabled = nullptr;
static uintptr_t g_down_finish_orig = 0;
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

static bogodroid::Il2cppRuntime g_il2cpp;

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
static get_v3_t m_tf_get_position = nullptr;
static void* mi_tf_get_position = nullptr;
static set_v3_t m_tf_set_local_position = nullptr;
static void* mi_tf_set_local_position = nullptr;
static set_v2_t m_rt_set_anchored_position = nullptr;
static void* mi_rt_set_anchored_position = nullptr;
static get_v2_t m_rt_get_anchored_position = nullptr;
static void* mi_rt_get_anchored_position = nullptr;
static Rect (*m_rt_get_rect)(void*, void*) = nullptr;
static void* mi_rt_get_rect = nullptr;
static get_v2_t m_rt_get_size_delta = nullptr;
static void* mi_rt_get_size_delta = nullptr;
static set_v2_t m_rt_set_size_delta = nullptr;
static void* mi_rt_set_size_delta = nullptr;
static canvas_renderer_set_color_t m_canvas_renderer_set_color = nullptr;
static void* mi_canvas_renderer_set_color = nullptr;
static uintptr_t g_renderer_set_color_orig = 0;
static uintptr_t g_gameobject_set_active_orig = 0;
static std::set<void*> g_hidden_renderers;
static std::set<void*> g_hidden_finish_actions;
static bool (*m_gameobject_active)(void*, void*) = nullptr;
static void* mi_gameobject_active = nullptr;
static Color (*m_renderer_get_color)(void*, void*) = nullptr;
static void* mi_renderer_get_color = nullptr;
static void* g_canvas_renderer_type = nullptr;
static void* g_text_type = nullptr;
static void* g_canvas_type = nullptr;
static get_float_t m_text_ppu = nullptr;
static void* mi_text_ppu = nullptr;
static int (*m_text_size)(void*, void*) = nullptr;
static void* mi_text_size = nullptr;
static get_float_t m_canvas_scale = nullptr;
static void* mi_canvas_scale = nullptr;
static int (*m_canvas_mode)(void*, void*) = nullptr;
static void* mi_canvas_mode = nullptr;
static get_transform_t m_text_font = nullptr;
static void* mi_text_font = nullptr;
static bool (*m_font_dynamic)(void*, void*) = nullptr;
static void* mi_font_dynamic = nullptr;
static void_method_t m_graphic_dirty = nullptr;
static void* mi_graphic_dirty = nullptr;
static uintptr_t g_text_ppu_orig = 0;
// Bound during GUI Start on the Unity thread, before the first mesh rebuild.
static std::set<void*> g_scaled_texts;
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
static void* g_transform_scan_type = nullptr;
static uintptr_t g_game_manager_instance = 0;
static int g_game_manager_current_obj_offset = 0;
static bool g_canvas_layout_ready = false;
static void* g_canvas_rt = nullptr;
static void* g_app_content_rt = nullptr;
static void* g_pad_rt = nullptr;
static void* g_grid_rt = nullptr;
struct CasingSide {
    void* rt = nullptr;
    Vector2 position = {}, size = {};
    bool ready = false;
};
static CasingSide g_casing_left, g_casing_right;
static void* g_feedback_menu_go = nullptr;
// ShowFeedBackMenu can replace IntroductionMenu.Start at playCounts 5/10.
// Dismiss via the original No action on the next Unity Update; hiding the
// GameObject alone leaves menuStatus=None and makes the controls inert.
static bool g_pending_feedback_dismiss = false;
static void* g_leave_feedback_go = nullptr;
static void* g_finish_menu_go = nullptr;
static void* g_exit_menu_go = nullptr;
static uintptr_t m_gui_press_exit = 0;
static void* mi_gui_press_exit = nullptr;
static uintptr_t m_gui_exit_game = 0;
static void* mi_gui_exit_game = nullptr;
static int g_last_screen_width = 0;
static int g_last_screen_height = 0;
static bool g_canvas_layout_logged = false;
static uintptr_t g_gui_start_orig = 0;
static uintptr_t g_gui_update_resolution_orig = 0;
static uintptr_t g_gui_manager_instance = 0;
static uintptr_t g_gui_update_orig = 0;
static uintptr_t m_gui_back_select_game = 0;
static void* mi_gui_back_select_game = nullptr;
static int g_gui_menu_offset = 0;
static uintptr_t m_gui_press_start_pause_game = 0;
static void* mi_gui_press_start_pause_game = nullptr;
static uintptr_t g_gui_show_feedback_orig = 0;
static uintptr_t g_gui_leave_feedback_orig = 0;
static uintptr_t g_game_manager_start_orig = 0;
static uintptr_t g_tank_player_start_orig = 0;
static uintptr_t g_tank_enemy_start_orig = 0;
static uintptr_t m_probe_play_game = 0;
static void* mi_probe_play_game = nullptr;
static bool g_canvas_targets_logged = false;
static Vector3 g_app_content_base_scale = {1.0f, 1.0f, 1.0f};
static Vector3 g_pad_base_scale = {1.0f, 1.0f, 1.0f};
static bool g_app_content_base_scale_ready = false;
static bool g_pad_base_scale_ready = false;
static int g_hierarchy_logged = 0;
static int g_runtime_probe_logged = 0;
static int g_transform_scan_requests = 0;
static bool g_tetris_classes_logged = false;
static int g_component_log_count = 0;
static std::set<void*> g_exit_shifted_rects;
static int g_current_game_log_count = 0;
static std::set<void*> g_runtime_seen_nodes;

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

static void apply_world_camera_transform(const char* reason)
{
    if (!m_camera_get_main ||
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
    bool pending_up = false;
    Uint32 down_ticks = 0;
    uint64_t down_frame = 0;
};

static std::array<TouchPoint, 7> g_points = {{
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,  222.0f, 375.0f},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 300.0f, 375.0f},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_CONTROLLER_BUTTON_DPAD_UP,    260.0f, 335.0f},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_CONTROLLER_BUTTON_DPAD_DOWN,  260.0f, 414.0f},
    {SDL_CONTROLLER_BUTTON_A,          SDL_CONTROLLER_BUTTON_A,          393.0f, 377.0f},
    {SDL_CONTROLLER_BUTTON_Y,          SDL_CONTROLLER_BUTTON_Y,          393.0f, 377.0f},
    {SDL_CONTROLLER_BUTTON_X,          SDL_CONTROLLER_BUTTON_X,          367.0f, 307.0f},
}};
static uint64_t g_frame_counter = 0;

static bool interesting_runtime_name(const std::string& name);

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

static void renderer_set_color_hook(void* self, Color* color, void* mi)
{
    Color adjusted = *color;
    if (g_hidden_renderers.count(self)) {
        static unsigned traced = 0;
        if (g_ui_trace && adjusted.a != 0 && traced++ < 24)
            g_api->log("BRICKGAME", "UI blocked color renderer=%p alpha=%.3f",
                self, static_cast<double>(adjusted.a));
        adjusted.a = 0;
    }
    // Copy the ref argument: do not alter the tween's own target/current color.
    reinterpret_cast<void (*)(void*, Color*, void*)>(g_renderer_set_color_orig)(
        self, &adjusted, mi);
}

static void gameobject_set_active_hook(void* self, bool active, void* mi)
{
    if (g_hidden_finish_actions.count(self))
        active = false;
    reinterpret_cast<gameobject_set_active_t>(g_gameobject_set_active_orig)(
        self, active, mi);
}

static void bind_hidden_renderer(void* go)
{
    if (!go || !g_canvas_renderer_type || !m_gameobject_get_component)
        return;
    void* renderer = m_gameobject_get_component(go, g_canvas_renderer_type,
                                                mi_gameobject_get_component);
    if (renderer) {
        g_hidden_renderers.insert(renderer);
        if (m_canvas_renderer_set_color)
            m_canvas_renderer_set_color(renderer, {1, 1, 1, 0},
                                         mi_canvas_renderer_set_color);
    }
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

static float text_ppu_hook(void* self, void* mi)
{
    const float original = reinterpret_cast<get_float_t>(g_text_ppu_orig)(self, mi);
    // Text.GetGenerationSettings and OnPopulateMesh use this same accessor:
    // rasterize at the displayed size, then divide mesh vertices by that PPU.
    // Canvas.scaleFactor omits the extra 2.2x AppContent transform.
    return g_font_fix_enabled && g_scaled_texts.count(self)
        ? original * kCanvasScale : original;
}

static void discover_control_rects(void* transform, int depth,
                                   bool hidden_control_tree = false, bool exit_tree = false)
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
        if (transform == g_app_content_rt) {
            if (name == "Casing_Left") g_casing_left.rt = child;
            if (name == "Casing_Right") g_casing_right.rt = child;
        }
        bind_control_rect(child, name);
        bind_feedback_node(child, child_go, name);
        const bool hidden_control = hidden_control_tree || find_control_rect(name);
        const bool in_exit = exit_tree || name == "ExitMenu";
        if (hidden_control)
            bind_hidden_renderer(child_go);
        if (name == "FinishMenu")
            g_finish_menu_go = child_go;
        if (name == "ExitMenu")
            g_exit_menu_go = child_go;
        if (g_exit_menu_go && transform == m_gameobject_get_transform(
                g_exit_menu_go, mi_gameobject_get_transform)) {
            // Move all original children together, including FrameButton's
            // dashed Yes/No border. The panel background is on ExitMenu itself.
            // Keep the decoration fixed; apply once per node, even on a rescan.
            if (name != "ExitGameIcon" &&
                m_rt_get_anchored_position && m_rt_set_anchored_position &&
                g_exit_shifted_rects.insert(child).second) {
                Vector2 position = m_rt_get_anchored_position(
                    child, mi_rt_get_anchored_position);
                position.y -= 260.0f;
                m_rt_set_anchored_position(child, position,
                                            mi_rt_set_anchored_position);
            }
            add_exit_icon(transform);
        }
        if (g_finish_menu_go && transform == m_gameobject_get_transform(
                g_finish_menu_go, mi_gameobject_get_transform) &&
            (name == "ShareBtn" || name == "LeaderBoardBtn" ||
             name == "BackToMenu" || name == "RePlayBtn" ||
             name == "GiftBtn" || name == "LeaveFeedBackBtn")) {
            g_hidden_finish_actions.insert(child_go);
            m_gameobject_set_active(child_go, false, mi_gameobject_set_active);
            g_api->log("BRICKGAME", "Finish action hidden %s go=%p",
                       name.c_str(), child_go);
            add_gameover_icon(child, transform, name.c_str());
        }
        if (child_go && g_text_type && m_gameobject_get_component &&
            m_text_ppu && m_text_size) {
            void* text = m_gameobject_get_component(child_go, g_text_type,
                                                   mi_gameobject_get_component);
            if (text) {
                void* parent_go = m_component_get_game_object(transform,
                    mi_component_get_game_object);
                const std::string parent_name = parent_go
                    ? managed_string_utf8(m_object_get_name(parent_go, mi_object_get_name))
                    : std::string();
                void* font = m_text_font ? m_text_font(text, mi_text_font) : nullptr;
                // HUD and finish scores share names; bind by their actual
                // parent. Hidden controls compensate the AppContent scale.
                const bool scaled = (parent_name == "MainGUI" || parent_name == "FinishMenu" || in_exit) &&
                    font && m_font_dynamic && m_font_dynamic(font, mi_font_dynamic);
                if (scaled && g_text_ppu_orig) {
                    g_scaled_texts.insert(text);
                    if (m_graphic_dirty)
                        m_graphic_dirty(text, mi_graphic_dirty);
                }
                if (g_font_trace || scaled)
                    g_api->log("BRICKGAME", "Font node=%s parent=%s size=%d ppu=%.3f scaled=%d",
                        name.c_str(), parent_name.c_str(), m_text_size(text, mi_text_size),
                        static_cast<double>(m_text_ppu(text, mi_text_ppu)), scaled ? 1 : 0);
            }
        }
        if (!name.empty() && g_hierarchy_logged < 96) {
            if (depth <= 2 || name == "Grid" || name.find("Grid") != std::string::npos) {
                g_api->log("BRICKGAME", "Canvas node depth=%d name=%s rt=%p",
                           depth + 1, name.c_str(), child);
                ++g_hierarchy_logged;
            }
        }
        if (name == "Grid" && !g_grid_rt) {
            g_grid_rt = child;
            g_api->log("BRICKGAME", "Grid RectTransform found rt=%p", g_grid_rt);
        }
        discover_control_rects(child, depth + 1, hidden_control, in_exit);
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

static void fit_casing_sides()
{
    if (!g_canvas_rt || !g_app_content_rt || !m_rt_get_rect ||
        !m_rt_get_size_delta || !m_rt_set_size_delta)
        return;
    const Rect canvas = m_rt_get_rect(g_canvas_rt, mi_rt_get_rect);
    const Rect app = m_rt_get_rect(g_app_content_rt, mi_rt_get_rect);
    const Vector3 scale = m_tf_get_local_scale(g_app_content_rt, mi_tf_get_local_scale);
    if (canvas.width <= 0 || scale.x <= 0) return;
    const float half_width = canvas.width / (2.0f * scale.x);
    const float center_x = m_rt_get_anchored_position(
        g_app_content_rt, mi_rt_get_anchored_position).x / scale.x;
    // Expand the two decorative sprites outwards, retaining their inner edges.
    // One physical pixel of overscan avoids a subpixel seam at the screen edge.
    const float overscan = canvas.width / (m_screen_width(mi_screen_width) * scale.x);
    const auto fit = [&](CasingSide& side, bool left) {
        if (!side.rt) return;
        if (!side.ready) {
            side.position = m_rt_get_anchored_position(side.rt, mi_rt_get_anchored_position);
            side.size = m_rt_get_size_delta(side.rt, mi_rt_get_size_delta);
            side.ready = true;
        }
        const float extension = std::max(0.0f, left
            ? half_width + center_x + app.x + side.position.x + overscan
            : half_width - center_x - (app.x + app.width + side.position.x) + overscan);
        Vector2 position = side.position, size = side.size;
        position.x += left ? -extension : extension;
        size.x += extension;
        m_rt_set_anchored_position(side.rt, position, mi_rt_set_anchored_position);
        m_rt_set_size_delta(side.rt, size, mi_rt_set_size_delta);
    };
    fit(g_casing_left, true);
    fit(g_casing_right, false);
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

    // Scale the visible branches relative to their original transforms.
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
    fit_casing_sides();
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
    const Uint32 started = SDL_GetTicks();
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
            g_scaled_texts.clear();
            if (g_font_trace && m_gameobject_get_component && g_canvas_type &&
                m_canvas_mode && m_canvas_scale) {
                void* go = m_component_get_game_object(rt, mi_component_get_game_object);
                void* canvas = m_gameobject_get_component(go, g_canvas_type,
                                                          mi_gameobject_get_component);
                if (canvas) {
                    const int mode = m_canvas_mode(canvas, mi_canvas_mode);
                    g_api->log("BRICKGAME", "Font canvas mode=%d scale=%.3f",
                        mode, static_cast<double>(m_canvas_scale(canvas, mi_canvas_scale)));
                }
            }
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
    g_api->log("BRICKGAME-STARTUP", "GUIManager.Start completed ticks=%u duration=%ums",
               SDL_GetTicks(), SDL_GetTicks() - started);
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

static uintptr_t gui_update_hook(uintptr_t self, uintptr_t mi,
                                 uintptr_t x2, uintptr_t x3, uintptr_t x4,
                                 uintptr_t x5, uintptr_t x6, uintptr_t x7)
{
    const uintptr_t ret = reinterpret_cast<orig8_t>(g_gui_update_orig)(
        self, mi, x2, x3, x4, x5, x6, x7);
    if (g_pending_feedback_dismiss && m_gui_back_select_game) {
        g_pending_feedback_dismiss = false;
        const int before = g_gui_menu_offset
            ? *reinterpret_cast<int*>(self + g_gui_menu_offset) : -1;
        reinterpret_cast<void_method_t>(m_gui_back_select_game)(
            reinterpret_cast<void*>(self), mi_gui_back_select_game);
        suppress_feedback_ui();
        const int after = g_gui_menu_offset
            ? *reinterpret_cast<int*>(self + g_gui_menu_offset) : -1;
        g_api->log("BRICKGAME", "feedback skipped via original No action menu=%d->%d", before, after);
    }
    if (!g_startup_scene_reported && g_canvas_rt) {
        g_startup_scene_reported = true;
        g_api->log("BRICKGAME-STARTUP", "first GUI Update ticks=%u", SDL_GetTicks());
        if (g_api->startup_scene_ready) g_api->startup_scene_ready();
    }
    if (g_gui_menu_offset && g_startup_menu != 2) {
        const int menu = *reinterpret_cast<int*>(self + g_gui_menu_offset);
        if (menu != g_startup_menu) {
            g_startup_menu = menu;
            g_api->log("BRICKGAME-STARTUP", "menu=%d ticks=%u", menu, SDL_GetTicks());
        }
    }
    // SDL only queues actions; Unity objects are accessed on the game thread.
    if (g_pending_start.exchange(false) && m_gui_press_start_pause_game) {
        reinterpret_cast<void_method_t>(m_gui_press_start_pause_game)(
            reinterpret_cast<void*>(self), mi_gui_press_start_pause_game);
        g_api->log("BRICKGAME", "Start direct PressStartPauseGame");
    }
    if (g_pending_guide.exchange(false) && m_gui_press_exit) {
        reinterpret_cast<void_method_t>(m_gui_press_exit)(
            reinterpret_cast<void*>(self), mi_gui_press_exit);
        g_api->log("BRICKGAME", "Guide direct PressExitBtn");
    }
    bool exit_visible = g_exit_menu_go && m_gameobject_active &&
        m_gameobject_active(g_exit_menu_go, mi_gameobject_active);
    const int exit_action = g_pending_exit_action.exchange(0);
    if (exit_visible && exit_action) {
        // Invoke the original Yes/No UnityEvent on the Unity thread.
        const uintptr_t method = exit_action == 1
            ? m_gui_exit_game : m_gui_back_select_game;
        void* method_info = exit_action == 1
            ? mi_gui_exit_game : mi_gui_back_select_game;
        if (method) {
            g_api->log("BRICKGAME", "Exit %s requested",
                       exit_action == 1 ? "A confirm" : "B cancel");
            reinterpret_cast<void_method_t>(method)(
                reinterpret_cast<void*>(self), method_info);
            exit_visible = m_gameobject_active(g_exit_menu_go, mi_gameobject_active);
            if (exit_action == 2) {
                const int menu = g_gui_menu_offset
                    ? *reinterpret_cast<int*>(self + g_gui_menu_offset) : -1;
                g_api->log("BRICKGAME", "Exit B close visible=%d menu=%d",
                           exit_visible ? 1 : 0, menu);
            }
        }
    }
    const bool was_exit_visible = g_exit_visible.exchange(
        exit_visible && m_gui_exit_game && m_gui_back_select_game);
    if (exit_visible != was_exit_visible)
        g_api->log("BRICKGAME", "Exit visible=%d", exit_visible ? 1 : 0);
    bool visible = g_finish_menu_go && m_gameobject_active &&
        m_gameobject_active(g_finish_menu_go, mi_gameobject_active);
    if (g_pending_finish_close.exchange(false) && visible && m_gui_back_select_game) {
        // Same method as the original FinishMenu Menu button. It cleans up
        // currentMenu / blankBtn and restores SelectingGame, unlike SetActive.
        reinterpret_cast<void_method_t>(m_gui_back_select_game)(
            reinterpret_cast<void*>(self), mi_gui_back_select_game);
        visible = m_gameobject_active(g_finish_menu_go, mi_gameobject_active);
        const int menu = g_gui_menu_offset
            ? *reinterpret_cast<int*>(self + g_gui_menu_offset) : -1;
        g_api->log("BRICKGAME", "Finish A close visible=%d menu=%d",
                   visible ? 1 : 0, menu);
    }
    const bool was_visible = g_finish_visible.exchange(
        visible && m_gui_back_select_game != 0);
    if (visible != was_visible)
        g_api->log("BRICKGAME", "Finish visible=%d", visible ? 1 : 0);
    if (g_ui_trace && (g_frame_counter % 120) == 0 && m_renderer_get_color) {
        unsigned bad_alpha = 0, active_actions = 0;
        for (void* renderer : g_hidden_renderers)
            bad_alpha += m_renderer_get_color(renderer, mi_renderer_get_color).a != 0;
        for (void* go : g_hidden_finish_actions)
            active_actions += m_gameobject_active(go, mi_gameobject_active);
        g_api->log("BRICKGAME", "UI hidden renderers=%u badAlpha=%u actions=%u activeActions=%u",
            static_cast<unsigned>(g_hidden_renderers.size()), bad_alpha,
            static_cast<unsigned>(g_hidden_finish_actions.size()), active_actions);
    }
    return ret;
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

BRICKGAME_COMPONENT_HOOK(tank_player_start_hook, g_tank_player_start_orig,
                         "TankPlayer.Start")
BRICKGAME_COMPONENT_HOOK(tank_enemy_start_hook, g_tank_enemy_start_orig,
                         "TankEnemy.Start")

// The original press handler resets the interval, preventing compounding.
// Leave the release handler unchanged so normal speed is restored by the game.
static int g_brickbreaker_interval_offset = 0;
static uintptr_t g_brickbreaker_fast_orig = 0;
static uintptr_t brickbreaker_fast_hook(uintptr_t self, uintptr_t mi) {
    uintptr_t result = reinterpret_cast<orig8_t>(g_brickbreaker_fast_orig)(
        self, mi, 0, 0, 0, 0, 0, 0);
    if (self && g_brickbreaker_interval_offset) {
        float& interval = *reinterpret_cast<float*>(self + g_brickbreaker_interval_offset);
        const float before = interval;
        const char* setting = std::getenv("BRICKGAME_G_SPEED_FIX");
        if (!setting || std::strcmp(setting, "0") != 0)
            interval *= 2.0f;
        if (g_down_trace)
            g_api->log("BRICKGAME", "G fast interval=%.6f->%.6f", double(before), double(interval));
    }
    return result;
}

// Advance only the player's movement clock; firing and enemies keep their clocks.
static int g_tank_move_timer_offset = 0;
static float (*m_delta_time)(void*, void*) = nullptr;
static void* mi_delta_time = nullptr;
static uintptr_t g_tank_move_orig[4] = {};
static uintptr_t tank_move(int direction, uintptr_t self, uintptr_t mi) {
    if (g_tank_speed_fix && self && g_tank_move_timer_offset && m_delta_time) {
        float& timer = *reinterpret_cast<float*>(self + g_tank_move_timer_offset);
        timer += m_delta_time(nullptr, mi_delta_time);
    }
    const float before = self && g_tank_move_timer_offset
        ? *reinterpret_cast<float*>(self + g_tank_move_timer_offset) : 0;
    uintptr_t result = reinterpret_cast<orig8_t>(g_tank_move_orig[direction])(
        self, mi, 0, 0, 0, 0, 0, 0);
    if (g_down_trace && self && g_tank_move_timer_offset && m_time &&
        *reinterpret_cast<float*>(self + g_tank_move_timer_offset) < before)
        g_api->log("BRICKGAME", "TANK move direction=%d time=%.3f timer=%.3f",
            direction, double(m_time(nullptr, mi_time)), double(before));
    return result;
}
#define TANK_MOVE_HOOK(N) \
static uintptr_t tank_move_##N(uintptr_t self, uintptr_t mi) { return tank_move(N, self, mi); }
TANK_MOVE_HOOK(0)
TANK_MOVE_HOOK(1)
TANK_MOVE_HOOK(2)
TANK_MOVE_HOOK(3)
#undef TANK_MOVE_HOOK

static std::string mino_shape(void* object)
{
    if (!m_component_get_transform || !m_tf_get_child_count ||
        !m_tf_get_child || !m_tf_get_position)
        return "?";
    void* transform = m_component_get_transform(object, mi_component_get_transform);
    if (!transform)
        return "?";
    const Vector3 origin = m_tf_get_position(transform, mi_tf_get_position);
    std::string shape;
    const int count = std::min(16, m_tf_get_child_count(transform, mi_tf_get_child_count));
    for (int i = 0; i < count; ++i) {
        void* child = m_tf_get_child(transform, i, mi_tf_get_child);
        const Vector3 p = m_tf_get_position(child, mi_tf_get_position);
        shape += std::to_string(std::lround((p.x - origin.x) * 10)) + "," +
                 std::to_string(std::lround((p.y - origin.y) * 10)) + ";";
    }
    return shape;
}

static uintptr_t down_check(DownMinoBinding& binding, uintptr_t self,
                            uintptr_t mi)
{
    auto* object = reinterpret_cast<unsigned char*>(self);
    if (binding.active_self != self) {
        reinterpret_cast<void_method_t>(binding.check_orig)(object,
            reinterpret_cast<void*>(mi));
        return 0;
    }
    const int game = g_game_manager_instance && g_game_status_offset
        ? *reinterpret_cast<int*>(g_game_manager_instance + g_game_status_offset)
        : 0;
    const bool matching = &binding == &g_down_five
        ? game == 13 : (game == 1 || game == 10 || game == 11 ||
                       game == 12 || game == 14 || game == 19);
    const bool ready = matching && m_behaviour_enabled(object, mi_behaviour_enabled);
    const bool playing = ready && m_time_scale(nullptr, mi_time_scale) > 0;
    g_down_gameplay_ready.store(ready, std::memory_order_release);
    g_down_gameplay_running.store(playing, std::memory_order_release);
    g_rotate_gameplay_ready.store(ready && binding.rotate,
                                  std::memory_order_release);
    if (ready)
        g_down_update_ticks.store(SDL_GetTicks(), std::memory_order_release);

    // Hold the edge lock through one managed step: once UP returns, no queued
    // repeat can still start. A DOWN/UP before Update retains its first tap.
    if (playing) {
        const float now = m_time(nullptr, mi_time);
        std::lock_guard<std::recursive_mutex> lock(g_down_mutex);
        if (g_rotate_taps) {
            --g_rotate_taps;
            // Original UI Rotate overwrites the horizontal status with 4;
            // CheckUserInput then clears it to Idle. Call the original rotation
            // directly, leaving horizontal hold/timers untouched. Rotate owns
            // all collision checks, wall adjustment and rotation restrictions.
            const std::string before = g_down_trace ? mino_shape(object) : "";
            binding.rotate(object, binding.rotate_mi);
            if (g_down_trace) {
                const std::string after = mino_shape(object);
                g_api->log("BRICKGAME",
                    "ROTATE step %s self=%p time=%.3f status=%d shape=%s->%s queued=%u",
                    binding.name, object, static_cast<double>(now),
                    *reinterpret_cast<int*>(object + binding.status),
                    before.c_str(), after.c_str(),
                    g_rotate_taps);
            }
        }
        bool step = g_down_taps != 0;
        const bool tap = step;
        if (step) {
            --g_down_taps;
            g_down_repeat_armed = g_down_held;
            g_down_next_repeat = now + kDownInitialDelay;
        } else if (g_down_held && g_down_repeat_armed &&
                   now >= g_down_next_repeat) {
            step = true;
            // One step per update, no burst after a slow frame.
            g_down_next_repeat = now + kDownRepeatDelay;
        }
        if (step) {
            auto& status = *reinterpret_cast<int*>(object + binding.status);
            const int saved_status = status;
            void* transform = g_down_trace && m_component_get_transform &&
                m_tf_get_local_position ? m_component_get_transform(
                object, mi_component_get_transform) : nullptr;
            const Vector3 before = transform ? m_tf_get_local_position(
                transform, mi_tf_get_local_position) : Vector3{};
            status = 3;
            object[binding.immediate] = 1;
            *reinterpret_cast<float*>(object + binding.hold_timer) = 0;
            *reinterpret_cast<float*>(object + binding.repeat_timer) = 0;
            if (binding.single && object[binding.single])
                binding.move_single(object, binding.single_mi);
            else
                binding.move(object, binding.move_mi);
            status = saved_status;
            // Manual MoveDown updated fall itself. Keep the next natural fall
            // eligible for an immediate step rather than its shared hold timer.
            object[binding.immediate] = 1;
            *reinterpret_cast<float*>(object + binding.hold_timer) = 0;
            *reinterpret_cast<float*>(object + binding.repeat_timer) = 0;
            if (g_down_trace) {
                const Vector3 after = transform ? m_tf_get_local_position(
                    transform, mi_tf_get_local_position) : Vector3{};
                g_api->log("BRICKGAME",
                    "DOWN step %s self=%p time=%.3f kind=%s held=%d y=%.1f->%.1f status=%d",
                    binding.name, object, static_cast<double>(now),
                    tap ? "tap" : "repeat", g_down_held ? 1 : 0,
                    static_cast<double>(before.y), static_cast<double>(after.y),
                    saved_status);
            }
            // A landing can disable this mino and start spawn/clear coroutines.
            if (!m_behaviour_enabled(object, mi_behaviour_enabled)) {
                return 0;
            }
        }
    } else {
        std::lock_guard<std::recursive_mutex> lock(g_down_mutex);
        g_down_taps = 0;
        g_down_repeat_armed = false;
        g_rotate_taps = 0;
    }
    void* transform = g_down_trace && m_component_get_transform &&
        m_tf_get_local_position ? m_component_get_transform(
            object, mi_component_get_transform) : nullptr;
    const Vector3 before = transform ? m_tf_get_local_position(
        transform, mi_tf_get_local_position) : Vector3{};
    const int status = *reinterpret_cast<int*>(object + binding.status);
    reinterpret_cast<void_method_t>(binding.check_orig)(object,
        reinterpret_cast<void*>(mi));
    if (transform) {
        const Vector3 after = m_tf_get_local_position(transform, mi_tf_get_local_position);
        if (before.x != after.x || before.y != after.y || status == 4)
            g_api->log("BRICKGAME", "DOWN original %s time=%.3f playing=%d status=%d xy=%.1f,%.1f->%.1f,%.1f",
                binding.name, static_cast<double>(m_time(nullptr, mi_time)),
                playing ? 1 : 0, status, static_cast<double>(before.x),
                static_cast<double>(before.y), static_cast<double>(after.x),
                static_cast<double>(after.y));
    }
    return 0;
}

static uintptr_t down_start(DownMinoBinding& binding, uintptr_t self, uintptr_t mi)
{
    reinterpret_cast<void_method_t>(binding.start_orig)(
        reinterpret_cast<void*>(self), reinterpret_cast<void*>(mi));
    binding.active_self = self;
    // The original Start resets the new mino's input status. A held physical
    // Down belongs to the piece just landed, never to its successor.
    std::lock_guard<std::recursive_mutex> lock(g_down_mutex);
    g_down_repeat_armed = false;
    g_down_taps = 0;
    g_rotate_taps = 0;
    if (g_down_trace)
        g_api->log("BRICKGAME", "DOWN spawn %s self=%p repeatReset=1 held=%d",
                   binding.name, reinterpret_cast<void*>(self), g_down_held ? 1 : 0);
    return 0;
}

static uintptr_t down_mino_start_hook(uintptr_t self, uintptr_t mi,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return down_start(g_down_mino, self, mi);
}

static uintptr_t down_five_start_hook(uintptr_t self, uintptr_t mi,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return down_start(g_down_five, self, mi);
}

static uintptr_t down_finish_hook(uintptr_t self, uintptr_t mi,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    if (g_down_trace)
        g_api->log("BRICKGAME", "DOWN finish");
    g_down_gameplay_ready.store(false, std::memory_order_release);
    g_down_gameplay_running.store(false, std::memory_order_release);
    g_rotate_gameplay_ready.store(false, std::memory_order_release);
    {
        std::lock_guard<std::recursive_mutex> lock(g_down_mutex);
        g_down_taps = 0;
        g_down_repeat_armed = false;
        g_rotate_taps = 0;
        // Keep ownership until UP so an edge is never sent to both routes.
    }
    g_down_mino.active_self = 0;
    g_down_five.active_self = 0;
    reinterpret_cast<void_method_t>(g_down_finish_orig)(
        reinterpret_cast<void*>(self), reinterpret_cast<void*>(mi));
    return 0;
}

static uintptr_t down_mino_check_hook(uintptr_t self, uintptr_t mi,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return down_check(g_down_mino, self, mi);
}

static uintptr_t down_five_check_hook(uintptr_t self, uintptr_t mi,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return down_check(g_down_five, self, mi);
}

static bool handle_tetris_down(bool down)
{
    std::lock_guard<std::recursive_mutex> lock(g_down_mutex);
    if (down && g_down_owned)
        return true;
    if (!down && g_down_owned) {
        g_down_owned = false;
        g_down_held = false;
        g_down_repeat_armed = false;
        if (g_down_trace)
            g_api->log("BRICKGAME", "DOWN edge up ticks=%u queued=%u", SDL_GetTicks(), g_down_taps);
        return true;
    }
    if (!down || !g_down_fix_enabled ||
        !g_down_gameplay_ready.load(std::memory_order_acquire) ||
        SDL_GetTicks() - g_down_update_ticks.load(std::memory_order_acquire) > 250)
        return false;
    if (!g_down_owned) {
        g_down_owned = true;
        g_down_held = true;
        // Consume paused Down too, preventing a held touch from leaking into
        // resumed gameplay. A press made while paused does not queue a step.
        if (g_down_gameplay_running.load(std::memory_order_acquire))
            ++g_down_taps;
        if (g_down_trace)
            g_api->log("BRICKGAME", "DOWN edge down ticks=%u queued=%u", SDL_GetTicks(), g_down_taps);
    }
    return true;
}

static bool handle_tetris_rotate(int button, bool down)
{
    if (button != SDL_CONTROLLER_BUTTON_A && button != SDL_CONTROLLER_BUTTON_Y)
        return false;
    std::lock_guard<std::recursive_mutex> lock(g_down_mutex);
    bool& owned = button == SDL_CONTROLLER_BUTTON_A
        ? g_rotate_a_owned : g_rotate_y_owned;
    if (owned) {
        if (!down)
            owned = false;
        return true;
    }
    if (!down || !g_rotate_fix_enabled ||
        !g_rotate_gameplay_ready.load(std::memory_order_acquire) ||
        SDL_GetTicks() - g_down_update_ticks.load(std::memory_order_acquire) > 250)
        return false;
    owned = true;
    // Preserve short taps, never repeat on hold, and swallow paused presses.
    // Start/FinishGame clear the queue so it cannot rotate a successor piece.
    if (g_down_gameplay_running.load(std::memory_order_acquire))
        ++g_rotate_taps;
    if (g_down_trace)
        g_api->log("BRICKGAME", "ROTATE edge %s ticks=%u queued=%u",
            button == SDL_CONTROLLER_BUTTON_A ? "a" : "y",
            SDL_GetTicks(), g_rotate_taps);
    return true;
}

static void install_down_binding(void* image, DownMinoBinding& binding,
                                 uintptr_t hook)
{
    binding.status = g_il2cpp.field_offset_for(image, binding.name, "tetrisMinoStatus");
    binding.immediate = g_il2cpp.field_offset_for(image, binding.name, "moveImmediateVertical");
    binding.hold_timer = g_il2cpp.field_offset_for(image, binding.name, "verticalButtonDownTimer");
    binding.repeat_timer = g_il2cpp.field_offset_for(image, binding.name, "verticalTimer");
    const bool five = &binding == &g_down_five;
    binding.single = five ? 0 : g_il2cpp.field_offset_for(image, binding.name, "isSingle");
    // Fail closed if this APK's managed layout differs from the decompiled one.
    bool valid = binding.status == 0x18 &&
        binding.immediate == (five ? 0x31 : 0x39) &&
        binding.hold_timer == (five ? 0x38 : 0x40) &&
        binding.repeat_timer == (five ? 0x40 : 0x48) &&
        (five || binding.single == 0x23);
    valid &= g_il2cpp.resolve_call(image, "", binding.name, "MoveDown", 0,
        reinterpret_cast<void**>(&binding.move), &binding.move_mi);
    if (!five)
        valid &= g_il2cpp.resolve_call(image, "", binding.name, "MoveDownSingleBrick", 0,
            reinterpret_cast<void**>(&binding.move_single), &binding.single_mi);
    if (valid)
        valid = g_il2cpp.hook_method(image, "", binding.name, "Start", 0,
            reinterpret_cast<uintptr_t>(five ? &down_five_start_hook
                                             : &down_mino_start_hook),
            &binding.start_orig);
    if (valid)
        binding.ready = g_il2cpp.hook_method(image, "", binding.name, "CheckUserInput", 0,
                                    hook, &binding.check_orig);
    if (binding.ready && g_rotate_fix_enabled)
        g_il2cpp.resolve_call(image, "", binding.name, "Rotate", 0,
            reinterpret_cast<void**>(&binding.rotate), &binding.rotate_mi);
    g_api->log("BRICKGAME", "Down fix %s %s enabled=%d initial=%.0fms repeat=%.0fms",
               binding.name, binding.ready ? "armed" : "unavailable",
               g_down_fix_enabled ? 1 : 0,
               static_cast<double>(kDownInitialDelay * 1000),
               static_cast<double>(kDownRepeatDelay * 1000));
}

static bool resolve_gui_method(void* image, const char* name,
                               uintptr_t* fn_out, void** mi_out)
{
    void* fn = nullptr;
    if (!g_il2cpp.resolve_call(image, "", "GUIManager", name, 0, &fn, mi_out))
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
    g_pending_feedback_dismiss = true;
    if (g_api)
        g_api->log("BRICKGAME", "blocked GUIManager.ShowFeedBackMenu; queued No action");
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
    if (!g_il2cpp.bind(g_api, "BRICKGAME")) {
        g_api->log("BRICKGAME", "missing il2cpp exports; Canvas layout disabled");
        return;
    }

    void* core = g_il2cpp.find_image("UnityEngine.CoreModule");
    if (!core)
        core = g_il2cpp.find_image("UnityEngine");
    void* asm_image = g_il2cpp.find_image("Assembly-CSharp");
    if (!core) {
        g_api->log("BRICKGAME", "UnityEngine image not found; Canvas layout disabled");
        return;
    }
    if (!asm_image) {
        g_api->log("BRICKGAME", "Assembly-CSharp image not found; Canvas layout disabled");
        return;
    }

    bind_gameover_icons(g_il2cpp, g_api);
    bind_exit_icon(g_il2cpp, g_api);
    bool ok = true;
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Component", "get_transform", 0,
                       reinterpret_cast<void**>(&m_component_get_transform),
                       &mi_component_get_transform);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "GetChild", 1,
                       reinterpret_cast<void**>(&m_tf_get_child),
                       &mi_tf_get_child);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "get_childCount", 0,
                       reinterpret_cast<void**>(&m_tf_get_child_count),
                       &mi_tf_get_child_count);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "get_parent", 0,
                       reinterpret_cast<void**>(&m_tf_get_parent),
                       &mi_tf_get_parent);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Component",
                       "get_gameObject", 0,
                       reinterpret_cast<void**>(
                           &m_component_get_game_object),
                       &mi_component_get_game_object);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "GameObject", "get_transform", 0,
                       reinterpret_cast<void**>(&m_gameobject_get_transform),
                       &mi_gameobject_get_transform);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "GameObject", "SetActive", 1,
                       reinterpret_cast<void**>(&m_gameobject_set_active),
                       &mi_gameobject_set_active);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "GameObject", "get_activeInHierarchy", 0,
                       reinterpret_cast<void**>(&m_gameobject_active),
                       &mi_gameobject_active);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Object", "get_name", 0,
                       reinterpret_cast<void**>(&m_object_get_name),
                       &mi_object_get_name);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "set_localScale",
                       1, reinterpret_cast<void**>(&m_tf_set_local_scale),
                       &mi_tf_set_local_scale);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "get_localScale",
                       0, reinterpret_cast<void**>(&m_tf_get_local_scale),
                       &mi_tf_get_local_scale);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "get_localPosition",
                       0, reinterpret_cast<void**>(&m_tf_get_local_position),
                       &mi_tf_get_local_position);
    if (g_down_trace)
        g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "get_position", 0,
            reinterpret_cast<void**>(&m_tf_get_position), &mi_tf_get_position);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Transform", "set_localPosition",
                       1, reinterpret_cast<void**>(&m_tf_set_local_position),
                       &mi_tf_set_local_position);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "RectTransform",
                       "get_anchoredPosition", 0,
                       reinterpret_cast<void**>(&m_rt_get_anchored_position),
                       &mi_rt_get_anchored_position);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "RectTransform",
                       "set_anchoredPosition", 1,
                       reinterpret_cast<void**>(&m_rt_set_anchored_position),
                       &mi_rt_set_anchored_position);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Screen", "get_width", 0,
                       reinterpret_cast<void**>(&m_screen_width),
                       &mi_screen_width);
    g_il2cpp.resolve_call(core, "UnityEngine", "RectTransform", "get_rect", 0,
        reinterpret_cast<void**>(&m_rt_get_rect), &mi_rt_get_rect);
    g_il2cpp.resolve_call(core, "UnityEngine", "RectTransform", "get_sizeDelta", 0,
        reinterpret_cast<void**>(&m_rt_get_size_delta), &mi_rt_get_size_delta);
    g_il2cpp.resolve_call(core, "UnityEngine", "RectTransform", "set_sizeDelta", 1,
        reinterpret_cast<void**>(&m_rt_set_size_delta), &mi_rt_set_size_delta);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Screen", "get_height", 0,
                       reinterpret_cast<void**>(&m_screen_height),
                       &mi_screen_height);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Camera", "get_main", 0,
                       reinterpret_cast<void**>(&m_camera_get_main),
                       &mi_camera_get_main);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Camera", "get_orthographicSize",
                       0, reinterpret_cast<void**>(
                           &m_camera_get_orthographic_size),
                       &mi_camera_get_orthographic_size);
    ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Camera", "set_orthographicSize",
                       1, reinterpret_cast<void**>(
                           &m_camera_set_orthographic_size),
                       &mi_camera_set_orthographic_size);
    if (!g_il2cpp.resolve_call(core, "UnityEngine", "Resources",
                      "FindObjectsOfTypeAll", 1,
                      reinterpret_cast<void**>(&m_object_find_objects_of_type),
                      &mi_object_find_objects_of_type)) {
        g_il2cpp.resolve_call(core, "UnityEngine", "Object", "FindObjectsOfType", 1,
                     reinterpret_cast<void**>(&m_object_find_objects_of_type),
                     &mi_object_find_objects_of_type);
    }

    g_transform_scan_type = g_il2cpp.resolve_type_object(core, "UnityEngine", "Transform");
    g_game_manager_current_obj_offset = g_il2cpp.field_offset_for(
        asm_image, "GameManager", "currentGameManagerObj");
    g_game_status_offset = g_il2cpp.field_offset_for(asm_image, "GameManager", "playGameStatus");
    if ((g_down_fix_enabled || g_rotate_fix_enabled || g_down_trace) &&
        g_game_status_offset == 0x90) {
        bool down_ok = g_il2cpp.resolve_call(core, "UnityEngine", "Time", "get_time", 0,
            reinterpret_cast<void**>(&m_time), &mi_time);
        down_ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Time", "get_timeScale", 0,
            reinterpret_cast<void**>(&m_time_scale), &mi_time_scale);
        down_ok &= g_il2cpp.resolve_call(core, "UnityEngine", "Behaviour", "get_enabled", 0,
            reinterpret_cast<void**>(&m_behaviour_enabled), &mi_behaviour_enabled);
        if (down_ok) {
            down_ok = g_il2cpp.hook_method(asm_image, "", "GameManager", "FinishGame", 0,
                reinterpret_cast<uintptr_t>(&down_finish_hook), &g_down_finish_orig);
        }
        if (down_ok) {
            install_down_binding(asm_image, g_down_mino,
                reinterpret_cast<uintptr_t>(&down_mino_check_hook));
            install_down_binding(asm_image, g_down_five,
                reinterpret_cast<uintptr_t>(&down_five_check_hook));
        }
    }
    g_api->log("BRICKGAME", "GameManager.currentGameManagerObj offset=0x%x",
               g_game_manager_current_obj_offset);
    resolve_gui_method(asm_image, "PressStartPauseGame",
                       &m_gui_press_start_pause_game,
                       &mi_gui_press_start_pause_game);
    resolve_gui_method(asm_image, "BackSelectGameMenu",
                       &m_gui_back_select_game, &mi_gui_back_select_game);
    resolve_gui_method(asm_image, "PressExitBtn", &m_gui_press_exit, &mi_gui_press_exit);
    resolve_gui_method(asm_image, "ExitGame", &m_gui_exit_game, &mi_gui_exit_game);
    g_gui_menu_offset = g_il2cpp.field_offset_for(asm_image, "GUIManager", "menuStatus");
    if (g_probe_auto_start && !g_tetris_classes_logged) {
        g_tetris_classes_logged = true;
        g_il2cpp.log_class(asm_image, "", "TetrisManager");
        g_il2cpp.log_class(asm_image, "", "TetrisMino");
        g_il2cpp.log_class(asm_image, "", "TetrisFiveManager");
        g_il2cpp.log_class(asm_image, "", "TetrisFiveMino");
        g_il2cpp.log_class(asm_image, "", "GameManager");
        g_il2cpp.log_class(asm_image, "", "ObjectManager");
        g_il2cpp.log_class(asm_image, "", "TankManager");
        g_il2cpp.log_class(asm_image, "", "TankPlayerController");
        g_il2cpp.log_class(asm_image, "", "BrickBreakerManager");
        g_il2cpp.log_class(asm_image, "", "SelectGameStatus");
        g_il2cpp.log_class(asm_image, "", "TankEnemyController");
    }

    bool alpha_ok = false;
    void* canvas_renderer_image = nullptr;
    if (g_il2cpp.f_class_get_type && g_il2cpp.f_type_get_object) {
        canvas_renderer_image = g_il2cpp.find_class_image(
            "UnityEngine", "CanvasRenderer");
        if (!canvas_renderer_image)
            canvas_renderer_image = core;
        g_canvas_renderer_type = g_il2cpp.resolve_type_object(
            canvas_renderer_image, "UnityEngine", "CanvasRenderer");
        alpha_ok = g_canvas_renderer_type &&
            g_il2cpp.resolve_call(core, "UnityEngine", "GameObject", "GetComponent",
                         1, reinterpret_cast<void**>(
                                &m_gameobject_get_component),
                         &mi_gameobject_get_component) &&
            g_il2cpp.resolve_call(canvas_renderer_image, "UnityEngine",
                         "CanvasRenderer", "SetColor", 1,
                         reinterpret_cast<void**>(
                             &m_canvas_renderer_set_color),
                         &mi_canvas_renderer_set_color);
    }
    if (!alpha_ok)
        g_api->log("BRICKGAME", "control alpha hiding unavailable");
    else {
        g_il2cpp.hook_method(canvas_renderer_image, "UnityEngine", "CanvasRenderer",
                    "SetColor_Injected", 1,
                    reinterpret_cast<uintptr_t>(&renderer_set_color_hook),
                    &g_renderer_set_color_orig);
        if (g_ui_trace)
            g_il2cpp.resolve_call(canvas_renderer_image, "UnityEngine", "CanvasRenderer",
                         "GetColor", 0, reinterpret_cast<void**>(&m_renderer_get_color),
                         &mi_renderer_get_color);
    }

    void* ui_image = g_il2cpp.find_class_image("UnityEngine.UI", "Text");
    void* canvas_image = g_il2cpp.find_class_image("UnityEngine", "Canvas");
    if (ui_image && canvas_image) {
        g_text_type = g_il2cpp.resolve_type_object(ui_image, "UnityEngine.UI", "Text");
        g_canvas_type = g_il2cpp.resolve_type_object(canvas_image, "UnityEngine", "Canvas");
        g_il2cpp.resolve_call(ui_image, "UnityEngine.UI", "Text", "get_pixelsPerUnit", 0,
            reinterpret_cast<void**>(&m_text_ppu), &mi_text_ppu);
        g_il2cpp.resolve_call(ui_image, "UnityEngine.UI", "Text", "get_fontSize", 0,
            reinterpret_cast<void**>(&m_text_size), &mi_text_size);
        g_il2cpp.resolve_call(canvas_image, "UnityEngine", "Canvas", "get_scaleFactor", 0,
            reinterpret_cast<void**>(&m_canvas_scale), &mi_canvas_scale);
        g_il2cpp.resolve_call(canvas_image, "UnityEngine", "Canvas", "get_renderMode", 0,
            reinterpret_cast<void**>(&m_canvas_mode), &mi_canvas_mode);
        g_il2cpp.resolve_call(ui_image, "UnityEngine.UI", "Text", "get_font", 0,
            reinterpret_cast<void**>(&m_text_font), &mi_text_font);
        g_il2cpp.resolve_call(ui_image, "UnityEngine.UI", "Graphic", "SetAllDirty", 0,
            reinterpret_cast<void**>(&m_graphic_dirty), &mi_graphic_dirty);
        void* font_image = g_il2cpp.find_class_image("UnityEngine", "Font");
        if (font_image && g_il2cpp.resolve_call(font_image, "UnityEngine", "Font", "get_dynamic", 0,
            reinterpret_cast<void**>(&m_font_dynamic), &mi_font_dynamic))
            g_il2cpp.hook_method(ui_image, "UnityEngine.UI", "Text", "get_pixelsPerUnit", 0,
                reinterpret_cast<uintptr_t>(&text_ppu_hook), &g_text_ppu_orig);
        g_api->log("BRICKGAME", "Font raster fix enabled=%d hook=%d factor=%.1f",
            g_font_fix_enabled ? 1 : 0, g_text_ppu_orig ? 1 : 0,
            static_cast<double>(kCanvasScale));
    }

    bool hooked = false;
    bool resolution_hooked = false;
    if (ok) {
        g_il2cpp.hook_method(core, "UnityEngine", "GameObject", "SetActive", 1,
                    reinterpret_cast<uintptr_t>(&gameobject_set_active_hook),
                    &g_gameobject_set_active_orig);
        g_il2cpp.hook_method(asm_image, "", "GUIManager", "Update", 0,
                    reinterpret_cast<uintptr_t>(&gui_update_hook), &g_gui_update_orig);
        hooked = g_il2cpp.hook_method(asm_image, "", "GUIManager", "Start", 0,
                             reinterpret_cast<uintptr_t>(&gui_start_hook),
                             &g_gui_start_orig);
        if (!hooked) {
            hooked = g_il2cpp.hook_method(asm_image, "", "GUIManager", "Awake", 0,
                                 reinterpret_cast<uintptr_t>(&gui_start_hook),
                                 &g_gui_start_orig);
        }
        g_il2cpp.hook_method(asm_image, "", "GameManager", "Start", 0,
                    reinterpret_cast<uintptr_t>(&game_manager_start_hook),
                    &g_game_manager_start_orig);
        resolution_hooked = g_il2cpp.hook_method(
            asm_image, "", "GUIManager", "UpdateResolution", 0,
            reinterpret_cast<uintptr_t>(&gui_update_resolution_hook),
            &g_gui_update_resolution_orig);
        g_il2cpp.hook_method(asm_image, "", "GUIManager", "ShowFeedBackMenu", 0,
                    reinterpret_cast<uintptr_t>(&gui_show_feedback_hook),
                    &g_gui_show_feedback_orig);
        g_il2cpp.hook_method(asm_image, "", "GUIManager", "LeaveFeedBack", 0,
                    reinterpret_cast<uintptr_t>(&gui_leave_feedback_hook),
                    &g_gui_leave_feedback_orig);
        g_brickbreaker_interval_offset = g_il2cpp.field_offset_for(asm_image, "BrickBreakerManager", "timeMoveBall");
        if (g_brickbreaker_interval_offset)
            g_il2cpp.hook_method(asm_image, "", "BrickBreakerManager", "StartMoveBallFast", 0,
                reinterpret_cast<uintptr_t>(&brickbreaker_fast_hook), &g_brickbreaker_fast_orig);
        g_tank_move_timer_offset = g_il2cpp.field_offset_for(
            asm_image, "TankPlayerController", "moveTimer");
        g_il2cpp.resolve_call(core, "UnityEngine", "Time", "get_deltaTime", 0,
            reinterpret_cast<void**>(&m_delta_time), &mi_delta_time);
        if (g_tank_move_timer_offset && m_delta_time) {
            const char* names[] = {"MoveRight", "MoveLeft", "MoveUp", "MoveDown"};
            uintptr_t hooks[] = {reinterpret_cast<uintptr_t>(&tank_move_0),
                reinterpret_cast<uintptr_t>(&tank_move_1), reinterpret_cast<uintptr_t>(&tank_move_2),
                reinterpret_cast<uintptr_t>(&tank_move_3)};
            for (int i = 0; i < 4; ++i)
                g_il2cpp.hook_method(asm_image, "", "TankPlayerController", names[i], 0,
                    hooks[i], &g_tank_move_orig[i]);
        }
        if (g_probe_auto_start) {
            g_probe_menu_offset = g_il2cpp.field_offset_for(asm_image, "GUIManager", "menuStatus");
            g_probe_selection_offset = g_il2cpp.field_offset_for(
                asm_image, "GUIManager", "selectGameStatus");
            void* play_fn = nullptr;
            const char* probe_method = std::strcmp(g_probe_game, "Tetris") == 0
                ? "PlayTetris" : std::strcmp(g_probe_game, "TetrisFive") == 0
                ? "PlayTetrisFive" : std::strcmp(g_probe_game, "BrickBreaker") == 0 ? "PlayBrickBreaker" : "PlayTank";
            if (g_il2cpp.resolve_call(asm_image, "", "GameManager", probe_method, 0,
                             &play_fn, &mi_probe_play_game)) {
                m_probe_play_game =
                    reinterpret_cast<uintptr_t>(play_fn);
                g_api->log("BRICKGAME", "resolved GameManager.%s=%p",
                           probe_method, play_fn);
            }
            g_il2cpp.hook_method(asm_image, "", "TankPlayerController", "Start", 0,
                        reinterpret_cast<uintptr_t>(&tank_player_start_hook),
                        &g_tank_player_start_orig);
            g_il2cpp.hook_method(asm_image, "", "TankEnemyController", "Start", 0,
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

static void on_button(int button, int down, void*)
{
    if (button == SDL_CONTROLLER_BUTTON_GUIDE || button == SDL_CONTROLLER_BUTTON_START) {
        if (down) {
            if (button == SDL_CONTROLLER_BUTTON_GUIDE)
                g_pending_guide.store(true);
            else
                g_pending_start.store(true);
        }
        return;
    }
    if (button == SDL_CONTROLLER_BUTTON_A || button == SDL_CONTROLLER_BUTTON_B) {
        bool& owned = button == SDL_CONTROLLER_BUTTON_A
            ? g_exit_a_owned : g_exit_b_owned;
        if (owned) {
            if (!down)
                owned = false;
            return;
        }
        if (down && g_exit_visible.load()) {
            owned = true;
            int expected = 0;
            g_pending_exit_action.compare_exchange_strong(expected,
                button == SDL_CONTROLLER_BUTTON_A ? 1 : 2);
            return;
        }
    }
    if (button == SDL_CONTROLLER_BUTTON_A) {
        if (down && g_finish_visible.load()) {
            g_finish_a_owned = true;
            g_pending_finish_close.store(true);
            return;
        }
        if (!down && g_finish_a_owned) {
            g_finish_a_owned = false;
            return;
        }
    }
    if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
        handle_tetris_down(down != 0))
        return;
    if (handle_tetris_rotate(button, down != 0))
        return;
    if (!g_api || !g_api->inject_touch)
        return;
    for (auto& point : g_points) {
        if (point.button != button)
            continue;
        const bool is_down = down != 0;
        const char* button_name = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(button));
        if (is_down) {
            if (point.pending_up) {
                point.pending_up = false;
                g_api->log("BRICKGAME", "%s down cancels deferred up",
                           button_name ? button_name : "?");
            }
            if (point.active)
                return;
        } else {
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
        if (g_frame_counter >= 90 && g_frame_counter < 140 && g_game_manager_instance &&
            m_probe_play_game && mi_probe_play_game) {
            bool via_gui = false;
            if (g_gui_manager_instance && m_gui_press_start_pause_game &&
                g_probe_menu_offset == 0xc0 && g_probe_selection_offset == 0xc4) {
                auto* gui = reinterpret_cast<unsigned char*>(g_gui_manager_instance);
                auto& menu = *reinterpret_cast<int*>(gui + g_probe_menu_offset);
                // Use the real GUI flow so subsequent Start pauses rather than
                // starting again from a stale SelectingGame menu state.
                if (menu == 1 && !g_probe_logo_dismissed) {
                    g_probe_logo_dismissed = true;
                    invoke_gui_manager(m_gui_press_start_pause_game,
                        mi_gui_press_start_pause_game, "PressStartPauseGame");
                }
                if (menu == 2) {
                    *reinterpret_cast<int*>(gui + g_probe_selection_offset) =
                        std::strcmp(g_probe_game, "Tetris") == 0 ? 1 :
                        std::strcmp(g_probe_game, "TetrisFive") == 0 ? 13 : std::strcmp(g_probe_game, "BrickBreaker") == 0 ? 7 : 2;
                    via_gui = invoke_gui_manager(m_gui_press_start_pause_game,
                        mi_gui_press_start_pause_game, "PressStartPauseGame");
                }
            }
            // Logo dismissal can transition asynchronously. Wait for selection
            // before starting; a faster replay must not bypass the GUI state.
            if (via_gui || g_frame_counter == 139) {
                if (!via_gui)
                    reinterpret_cast<void_method_t>(m_probe_play_game)(
                        reinterpret_cast<void*>(g_game_manager_instance),
                        mi_probe_play_game);
                g_probe_auto_start_stage = 2;
                g_api->log("BRICKGAME", "probe auto-start %s viaGUI=%d",
                           g_probe_game, via_gui ? 1 : 0);
            }
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
    if (!g_api || !g_api->inject_touch)
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
    std::memcpy(&g_api_storage, api,
        std::min<size_t>(api->struct_size, sizeof(g_api_storage)));
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
        const char* probe_game = g_api->getenv("BRICKGAME_PROBE_GAME");
        if (probe_game && *probe_game)
            g_probe_game = probe_game;
        const char* down_trace = g_api->getenv("BRICKGAME_DOWN_TRACE");
        g_down_trace = down_trace && std::strcmp(down_trace, "1") == 0;
        const char* tank_fix = g_api->getenv("BRICKGAME_TANK_SPEED_FIX");
        g_tank_speed_fix = !tank_fix || std::strcmp(tank_fix, "0") != 0;
        const char* down_fix = g_api->getenv("BRICKGAME_DOWN_FIX");
        g_down_fix_enabled = !down_fix || std::strcmp(down_fix, "0") != 0;
        const char* rotate_fix = g_api->getenv("BRICKGAME_ROTATE_FIX");
        g_rotate_fix_enabled = !rotate_fix || std::strcmp(rotate_fix, "0") != 0;
        const char* font_trace = g_api->getenv("BRICKGAME_FONT_TRACE");
        g_font_trace = font_trace && std::strcmp(font_trace, "1") == 0;
        const char* font_fix = g_api->getenv("BRICKGAME_FONT_FIX");
        g_font_fix_enabled = !font_fix || std::strcmp(font_fix, "0") != 0;
        const char* ui_trace = g_api->getenv("BRICKGAME_UI_TRACE");
        g_ui_trace = ui_trace && std::strcmp(ui_trace, "1") == 0;
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
               "plugin armed touch=1 refresh=1 minHold=%ums/%lluf "
               "design=%.0fx%.0f canvasScale=%.1f canvasY=%.0f",
               static_cast<unsigned>(kMinTouchHoldMs),
               static_cast<unsigned long long>(kMinTouchHoldFrames),
               static_cast<double>(g_design_width),
               static_cast<double>(g_design_height),
               static_cast<double>(kCanvasScale),
               static_cast<double>(kCanvasPositionY));
    return BOGO_PLUGIN_OK;
}

} // namespace brickgamepro

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version < 8 ||
        api->struct_size < offsetof(BogoPluginApi, set_present_viewport_filter) +
            sizeof(api->set_present_viewport_filter) || !api->log ||
        !api->so_symbol)
        return -1;
    if (!api->il2cpp) {
        // The loader invokes plugins once before IL2CPP is loaded.  Register
        // the static Android splash here so it can cover the native library
        // loading phase; the full plugin is initialized on the second pass.
        const char* package = api->config_get_string
            ? api->config_get_string("package.packageName", "") : "";
        if (package && std::strcmp(package, brickgamepro::kPackageName) == 0 &&
            api->struct_size >= offsetof(BogoPluginApi, set_startup_video) +
                sizeof(api->set_startup_video) && api->set_startup_video)
            api->set_startup_video(brickgamepro::kStartupVideo, 3.0f / 4.0f);
        return BOGO_PLUGIN_DEFERRED;
    }
    return brickgamepro::init(api);
}
