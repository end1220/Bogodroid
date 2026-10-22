// UI actions use the original touch path; only flight/fire are gameplay input.
#include "plugin_api.h"
#include "plugin_il2cpp.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace spacesquids {
// Layout checks below restrict the adapter to the inspected APK. Gameplay
// methods retain the original ammunition, cooldown and sound behavior.
static constexpr int kTouchPointer = 29;
static constexpr int kPlayerInputOffset = 0x30;
static BogoPluginApi api{};
static bogodroid::Il2cppRuntime rt;
struct V2 { float x, y; };
struct V3 { float x, y, z; };
struct Rect { float x, y, w, h; };
struct Array { uintptr_t klass, monitor, bounds, length; void* items[1]; };
struct String { uintptr_t klass, monitor; int32_t length; uint16_t chars[1]; };
template<class F> struct Method {
    F fn{};
    void* mi{};
    bool bind(const char* ns, const char* cls, const char* name, int argc) {
        return rt.resolve_call(rt.find_class_image(ns, cls), ns, cls, name,
                               argc, reinterpret_cast<void**>(&fn), &mi);
    }
    template<class... Args> auto operator()(Args... args) const {
        return fn(args..., mi);
    }
};
using Obj = void* (*)(void*, void*);
using Bool = bool (*)(void*, void*);
static Method<Obj> get_transform, get_go, get_name, parent;
static Method<void* (*)(void*, void*, void*)> get_component;
static Method<Obj> canvas_camera;
static Method<Obj> target_graphic, graphic_renderer;
static Method<Obj> scroll_content, scroll_viewport, slider_handle;
static Method<V2 (*)(void*, void*)> anchored_position;
static Method<void (*)(void*, V2, void*)> set_anchored_position;
static Method<void (*)(void*, void*)> stop_scroll;
static Method<Bool> renderer_culled;
static Method<bool (*)(void*, V2, void*, void*)> graphic_raycast;
static Method<Bool> active, is_active, interactable;
static Method<Obj> find_all;
static Method<Rect (*)(void*, void*)> get_rect;
static Method<V3 (*)(void*, V3, void*)> transform_point;
static Method<V2 (*)(void*, V3, void*)> world_to_screen;
static Method<int (*)(void*)> screen_w, screen_h, frame_count;
static Method<void (*)(void*, void*)> shoot, rocket;
static void* selectable_type{};
static void* canvas_type{};
static void* scroll_type{};
static void* slider_type{};
static int hp_offset{}, paused_offset{}, ammo_offset{};
static uintptr_t original_menu{}, original_game{}, original_player{};
static std::atomic<uint32_t> held{0}, edges{0};
static std::atomic<int> axes[6]{};
static uint32_t blocked_buttons{};
static bool blocked_axes = true;
enum class Mode { Transition, UI, Combat };
static Mode mode = Mode::Transition, candidate = Mode::Transition;
static int stable{}, last_frame = -1;
static void* player{};
static void* game{};
static int player_frame = -100, game_frame = -100;
static int player_hp = -1;
static bool game_paused{};
static int last_scan_frame = -100;
static float width = 1280, height = 720, cursor_x = 640, cursor_y = 360;
static bool touch_active{}, touch_auto{};
static int touch_frame{};
static float touch_x{}, touch_y{};
static uint32_t touch_ticks{}, last_ticks{}, repeat_at{};
static int previous_direction{};
struct Target { void* object; std::string path; float x, y, w, h; };
static std::vector<Target> targets;
static std::vector<Target> ship_targets;
static void* ship_scroll{};
static void* ship_camera{};
static std::string snapshot;
static std::string panel_root;
static void* selected{};
static bool free_cursor{};
static bool parked{};
static bool combat_ready{};
static bool rocket_requested{};
static bool trace{};

static std::string name(void* object) {
    auto* s = static_cast<String*>(get_name(object));
    if (!s || s->length < 0 || s->length > 256) return {};
    std::string result;
    for (int i = 0; i < s->length; ++i)
        result += s->chars[i] < 128 ? static_cast<char>(s->chars[i]) : '?';
    return result;
}
static std::string path(void* transform) {
    std::string result;
    for (int depth = 0; transform && depth < 20; ++depth) {
        result = name(transform) + "/" + result;
        transform = parent(transform);
    }
    return result;
}
static bool contains(const std::string& s, const char* word) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(word) != std::string::npos;
}

// Unity screen coordinates are bottom-left; injected touch/cursor coordinates
// are top-left. Keep this conversion at the geometry boundary.
static void* camera_for(void* transform) {
    for (void* node = transform; node; node = parent(node)) {
        void* canvas = get_component(node, canvas_type);
        if (canvas) return canvas_camera(canvas);
    }
    return nullptr;
}
static Rect screen_rect(void* transform, void* camera) {
    const Rect r = get_rect(transform);
    const V2 a = world_to_screen(camera,
        transform_point(transform, V3{r.x, r.y, 0}));
    const V2 b = world_to_screen(camera,
        transform_point(transform, V3{r.x + r.w, r.y + r.h, 0}));
    return Rect{std::min(a.x, b.x), height - std::max(a.y, b.y),
                std::abs(b.x - a.x), std::abs(b.y - a.y)};
}

// Touch ownership survives card culling, but never a panel/mode transition.
static void release_touch() {
    if (touch_active) {
        api.inject_touch(kTouchPointer, touch_x, touch_y, width, height, 0, "spacesquids-ui");
        api.log("SQUIDS", "touch up (%.1f,%.1f)", touch_x, touch_y);
    }
    touch_active = false;
}
static void press_touch(float x, float y, bool automatic, int frame) {
    if (touch_active) return;
    touch_x = x; touch_y = y;
    touch_active = api.inject_touch(kTouchPointer, x, y, width, height, 1,
                                    "spacesquids-ui") != 0;
    touch_auto = automatic; touch_frame = frame; touch_ticks = SDL_GetTicks();
    api.log("SQUIDS", "touch down (%.1f,%.1f) frame=%d", x, y, frame);
}
static void scan_ui() {
    targets.clear();
    ship_targets.clear(); ship_scroll = nullptr; ship_camera = nullptr;
    auto* list = static_cast<Array*>(find_all(selectable_type));
    if (!list || list->length > 10000) return;
    for (size_t i = 0; i < list->length; ++i) {
        void* object = list->items[i];
        if (!object || !is_active(object) || !interactable(object)) continue;
        void* go = get_go(object);
        if (!go || !active(go)) continue;
        void* tf = get_transform(object);
        if (!tf) continue;
        void* camera = camera_for(tf);
        const Rect rect = screen_rect(tf, camera);
        const float w = rect.w, h = rect.h;
        float x = rect.x + w * .5f, y = rect.y + h * .5f;
        const std::string object_path = path(tf);
        if (contains(object_path, "/contentpanel/") &&
            contains(object_path, "/iapbutton/") && std::isfinite(x) &&
            std::isfinite(y) && w >= 2 && h >= 2) {
            // Keep offscreen cards for navigation; only visible cards can be clicked.
            ship_targets.push_back({object, object_path, x, y, w, h});
            if (!ship_scroll) {
                for (void* node = tf; node; node = parent(node)) {
                    if ((ship_scroll = get_component(node, scroll_type))) break;
                }
                ship_camera = camera;
            }
        }
        if (!std::isfinite(x) || !std::isfinite(y) || w < 2 || h < 2 ||
            x + w * .5f < 1 || y + h * .5f < 1 ||
            x - w * .5f > width - 1 || y - h * .5f > height - 1) continue;
        x = (std::max(0.f, x - w * .5f) + std::min(width - 1, x + w * .5f)) * .5f;
        y = (std::max(0.f, y - h * .5f) + std::min(height - 1, y + h * .5f)) * .5f;
        void* graphic = target_graphic(object);
        if (graphic) {
            void* renderer = graphic_renderer(graphic);
            if (renderer && renderer_culled(renderer)) continue;
            if (!graphic_raycast(graphic, V2{x, height - y}, camera)) continue;
        }
        targets.push_back({object, object_path, x, y, w, h});
    }
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
        return a.path < b.path;
    });
    std::string next;
    for (const auto& t : targets)
        next += t.path + ":" + std::to_string(reinterpret_cast<uintptr_t>(t.object)) + ";";
    std::string root = targets.empty() ? "" : targets.front().path;
    for (const auto& t : targets) {
        size_t n = 0;
        while (n < root.size() && n < t.path.size() && root[n] == t.path[n]) ++n;
        root.resize(n);
    }
    const size_t slash = root.find_last_of('/');
    if (slash != std::string::npos) root.resize(slash + 1);
    if (next != snapshot) {
        // Scrolling can reveal/cull cards in the same panel. Keep the original
        // DOWN while those targets change; a different panel cancels the drag.
        const bool keep_drag = touch_active && !touch_auto &&
            !root.empty() && root == panel_root;
        snapshot = next;
        api.log("SQUIDS", "UI snapshot controls=%zu", targets.size());
        if (trace && !ship_targets.empty())
            api.log("SQUIDS", "shop cards=%zu scroll=%p", ship_targets.size(), ship_scroll);
        for (const auto& t : targets)
            api.log("SQUIDS", "UI %s center=(%.1f,%.1f) size=(%.1f,%.1f)",
                    t.path.c_str(), t.x, t.y, t.w, t.h);
        if (!keep_drag) {
            // Card culling during a scroll is not a new panel.
            const bool same_panel = !root.empty() && root == panel_root;
            const bool still_visible = std::any_of(targets.begin(), targets.end(),
                [](const Target& t) { return t.object == selected; });
            if (!same_panel || (!still_visible && !free_cursor)) {
                selected = nullptr; free_cursor = false; parked = false;
            }
            if (!same_panel) {
                blocked_buttons |= held.load(); edges.store(0); release_touch();
            }
        } else {
            selected = nullptr; free_cursor = true;
        }
    }
    panel_root = root;
}
static void select_target(const Target& t) {
    selected = t.object; cursor_x = t.x; cursor_y = t.y;
    free_cursor = false; parked = false;
    if (contains(t.path, "slider")) {
        void* slider = get_component(t.object, slider_type);
        void* handle = slider ? slider_handle(slider) : nullptr;
        if (handle) {
            void* camera = camera_for(handle);
            const Rect r = get_rect(handle);
            const V2 p = world_to_screen(camera, transform_point(handle,
                V3{r.x + r.w * .5f, r.y + r.h * .5f, 0}));
            cursor_x = p.x; cursor_y = height - p.y;
        }
    }
    api.log("SQUIDS", "cursor -> %s (%.1f,%.1f)", t.path.c_str(), cursor_x, cursor_y);
}
static const Target* find_named(const char* word) {
    for (const auto& t : targets) if (contains(t.path, word)) return &t;
    return nullptr;
}
static void initialize_cursor(bool death_panel) {
    if (!selected && !free_cursor && !targets.empty()) {
        if (death_panel) {
            cursor_x = width * .5f; cursor_y = height * .08f;
            // Choose a point outside every currently clickable rectangle.
            for (int y = 1; y < 20; ++y) {
                cursor_y = height * y / 20.f;
                const bool occupied = std::any_of(targets.begin(), targets.end(),
                    [](const Target& t) { return std::abs(cursor_x-t.x) <= t.w*.5f+4 &&
                        std::abs(cursor_y-t.y) <= t.h*.5f+4; });
                if (!occupied) break;
            }
            free_cursor = true; parked = true;
            api.log("SQUIDS", "death cursor parked (%.1f,%.1f); select before confirm", cursor_x,cursor_y);
        } else {
            const Target* initial = find_named("/start_game_button/");
            if (!initial && !ship_targets.empty()) {
                for (const auto& t : targets) if (contains(t.path,"/iapbutton/")) {
                    initial = &t; break;
                }
            }
            if (!initial) initial = find_named("resume_button");
            if (!initial) {
                float distance = 1e30f;
                for (const auto& t : targets) {
                    const float d = std::hypot(t.x - width * .5f, t.y - height * .5f);
                    if (d < distance) { distance = d; initial = &t; }
                }
            }
            select_target(*initial);
        }
    }
}
static bool navigate_ship(int dx) {
    const Target* current = nullptr;
    for (const auto& t : ship_targets) if (t.object == selected) current = &t;
    if (!current || !ship_scroll) return false;
    const Target* next = nullptr;
    float distance = 1e30f;
    for (const auto& t : ship_targets) {
        const float d = (t.x - current->x) * dx;
        if (d > 3 && d < distance) { next = &t; distance = d; }
    }
    if (!next) return true; // End of the list, do not jump to Back.
    const Target desired = *next;
    void* content = scroll_content(ship_scroll);
    void* viewport = scroll_viewport(ship_scroll);
    if (trace) api.log("SQUIDS", "shop next=%s content=%p viewport=%p",
        desired.path.c_str(), content, viewport);
    if (!viewport) viewport = get_transform(ship_scroll);
    if (!content || !viewport) return true;
    const Rect v = screen_rect(viewport, ship_camera);
    const V2 origin = world_to_screen(ship_camera, transform_point(content, V3{0, 0, 0}));
    const V2 unit = world_to_screen(ship_camera, transform_point(content, V3{1, 0, 0}));
    const float scale = unit.x - origin.x;
    if (std::abs(scale) > .001f) {
        // This game's unrestricted ScrollRect keeps a one-card content rect;
        // its child cards extend past that rect, so normalized bounds cannot
        // describe the list. Move its content to centre an actual adjacent card.
        stop_scroll(ship_scroll);
        V2 position = anchored_position(content);
        position.x += ((v.x + v.w * .5f) - desired.x) / scale;
        set_anchored_position(content, position);
        api.log("SQUIDS", "scroll -> %s contentX=%.1f", desired.path.c_str(), position.x);
        scan_ui();
    }
    // CanvasRenderer.cull may lag until the next render; the card geometry
    // already reflects the scroll and provides the correct cursor position.
    for (const auto& t : ship_targets) if (t.object == desired.object) {
        select_target(t); return true;
    }
    return true;
}
static void navigate(int dx, int dy) {
    if (dx && !dy && navigate_ship(dx)) return;
    const Target* best = nullptr;
    float score = 1e30f;
    for (const auto& t : targets) {
        if (t.object == selected) continue;
        const float x = t.x - cursor_x, y = t.y - cursor_y;
        const float forward = x * dx + y * dy;
        if (forward < 3) continue;
        const float side = std::abs(x * dy - y * dx);
        const float cost = forward + side * 2.f + side * side / forward;
        if (cost < score) { score = cost; best = &t; }
    }
    if (best) select_target(*best);
}
static void update(int frame) {
    if (frame == last_frame) return;
    last_frame = frame;
    const uint32_t now = SDL_GetTicks();
    const float dt = last_ticks ? std::min(.05f, (now - last_ticks) / 1000.f) : .016f;
    last_ticks = now;
    width = static_cast<float>(screen_w()); height = static_cast<float>(screen_h());
    if (width <= 0 || height <= 0) return;
    if (frame - last_scan_frame >= 8 || mode == Mode::Transition) {
        scan_ui();
        last_scan_frame = frame;
    }
    const bool living = player && frame - player_frame <= 2 && player_hp > 0;
    const bool paused = game && frame - game_frame <= 2 && game_paused;
    bool blocking_ui = false;
    for (const auto& t : targets)
        if (!(contains(t.path, "/shooting2button/") || contains(t.path, "/pausebutton/") ||
              contains(t.path, "joystick") || contains(t.path, "movement")))
            blocking_ui = true;
    const Mode wanted = living && !paused && !blocking_ui ? Mode::Combat :
        blocking_ui ? Mode::UI : Mode::Transition;
    if (candidate != wanted) { candidate = wanted; stable = 0; }
    if (++stable >= 3 && mode != wanted) {
        release_touch(); mode = wanted;
        blocked_buttons = held.load(); blocked_axes = true; edges.store(0);
        rocket_requested = false;
        selected = nullptr; free_cursor = false; parked = false;
        api.log("SQUIDS", "mode=%s alive=%d paused=%d frame=%d",
            mode == Mode::UI ? "UI" : mode == Mode::Combat ? "COMBAT" : "TRANSITION",
            living, paused, frame);
    }
    const uint32_t buttons = held.load();
    blocked_buttons &= buttons;
    const uint32_t pressed = edges.exchange(0) & ~blocked_buttons;
    const int ax = axes[0].load(), ay = axes[1].load();
    bool neutral = true;
    for (const auto& axis : axes) neutral = neutral && std::abs(axis.load()) < 8000;
    if (neutral) blocked_axes = false;
    combat_ready = mode == Mode::Combat && wanted == Mode::Combat;
    api.set_controller_capture(!combat_ready || blocked_buttons || blocked_axes);
    api.set_present_cursor(mode == Mode::UI, cursor_x, cursor_y, width, height);
    if (touch_active && frame > touch_frame && now - touch_ticks >= 50 &&
        (touch_auto || !(buttons & (1u << SDL_CONTROLLER_BUTTON_A)))) {
        release_touch();
    }
    if (combat_ready) {
        if (pressed & (1u << SDL_CONTROLLER_BUTTON_START)) {
            const Target* pause = find_named("pausebutton");
            if (pause) press_touch(pause->x, pause->y, true, frame);
        } else if (!blocked_axes && (pressed & (1u << SDL_CONTROLLER_BUTTON_X))) {
            // Consume the edge on the player Update hook, on Unity's thread.
            rocket_requested = true;
        }
        return;
    }
    if (mode != Mode::UI) return;
    const bool death_panel = find_named("gameover") != nullptr;
    initialize_cursor(death_panel);
    if (pressed & (1u << SDL_CONTROLLER_BUTTON_GUIDE)) {
        const Target* settings = find_named("/optionsbutton/");
        if (settings) { select_target(*settings); press_touch(cursor_x, cursor_y, true, frame); }
    }
    if (!death_panel && (pressed & (1u << SDL_CONTROLLER_BUTTON_B))) {
        const Target* back = find_named("backbutton");
        if (!back) back = find_named("continue");
        if (!back) back = find_named("resume_button");
        if (back) { select_target(*back); press_touch(cursor_x, cursor_y, true, frame); }
    }
    int dx = 0, dy = 0;
    const uint32_t usable = buttons & ~blocked_buttons;
    if (usable & (1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT)) dx = -1;
    if (usable & (1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx = 1;
    if (usable & (1u << SDL_CONTROLLER_BUTTON_DPAD_UP)) dy = -1;
    if (usable & (1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN)) dy = 1;
    if (!blocked_axes) {
        if (std::abs(ax) > 16000) dx = ax < 0 ? -1 : 1;
        if (std::abs(ay) > 16000) dy = ay < 0 ? -1 : 1;
    }
    const float rx = axes[2].load() / 32768.f, ry = axes[3].load() / 32768.f;
    if (!blocked_axes && (std::abs(rx) > .2f || std::abs(ry) > .2f)) {
        selected = nullptr; free_cursor = true; parked = false;
        cursor_x = std::clamp(cursor_x + rx * width * .65f * dt, 0.f, width - 1);
        cursor_y = std::clamp(cursor_y + ry * height * .65f * dt, 0.f, height - 1);
    }
    const int direction = (dx + 1) + (dy + 1) * 3;
    const Target* current = nullptr;
    for (const auto& t : targets) if (t.object == selected) current = &t;
    const bool dragging = touch_active && !touch_auto;
    if (dx || dy) {
        if (dragging) {
            cursor_x = std::clamp(cursor_x + dx * width * .35f * dt, 0.f, width - 1);
            cursor_y = std::clamp(cursor_y + dy * height * .35f * dt, 0.f, height - 1);
            if (current && contains(current->path, "slider")) {
                cursor_x = std::clamp(cursor_x, current->x - current->w * .5f,
                                      current->x + current->w * .5f);
                cursor_y = current->y;
            }
        } else if (direction != previous_direction || now >= repeat_at) {
            navigate(dx, dy);
            repeat_at = now + (direction != previous_direction ? 380 : 170);
        }
    }
    previous_direction = dx || dy ? direction : 0;
    if (!parked && (pressed & (1u << SDL_CONTROLLER_BUTTON_A)))
        press_touch(cursor_x, cursor_y, false, frame);
    if (touch_active && !touch_auto &&
        (buttons & (1u << SDL_CONTROLLER_BUTTON_A))) {
        touch_x = cursor_x; touch_y = cursor_y;
        api.inject_touch(kTouchPointer, touch_x, touch_y, width, height, 1, "spacesquids-ui");
    }
    api.set_present_cursor(1, cursor_x, cursor_y, width, height);
}
static void menu_update(void* self, void* mi) {
    reinterpret_cast<void (*)(void*, void*)>(original_menu)(self, mi);
    update(frame_count());
}
static void game_update(void* self, void* mi) {
    game = self; game_frame = frame_count();
    reinterpret_cast<void (*)(void*, void*)>(original_game)(self, mi);
    game_paused = self && paused_offset &&
        *reinterpret_cast<bool*>(static_cast<char*>(self) + paused_offset);
    update(frame_count());
}
static void player_update(void* self, void* mi) {
    player = self; player_frame = frame_count();
    reinterpret_cast<void (*)(void*, void*)>(original_player)(self, mi);
    player_hp = self && hp_offset
        ? *reinterpret_cast<int*>(static_cast<char*>(self) + hp_offset) : -1;
    update(frame_count());
    const uint32_t buttons = held.load() & ~blocked_buttons;
    const bool launch_rocket = rocket_requested;
    rocket_requested = false;
    if (launch_rocket && combat_ready && !blocked_axes && self && player_hp > 0) {
        const int before = trace && ammo_offset
            ? *reinterpret_cast<int*>(static_cast<char*>(self) + ammo_offset) : -1;
        rocket(self); // The game owns ammunition, cooldown, sound and HUD updates.
        if (trace && ammo_offset)
            api.log("SQUIDS", "rocket ammo=%d->%d hp=%d frame=%d", before,
                *reinterpret_cast<int*>(static_cast<char*>(self) + ammo_offset),
                player_hp, player_frame);
    }
    if (combat_ready && !blocked_axes && self &&
        player_hp > 0 &&
        ((buttons & ((1u << SDL_CONTROLLER_BUTTON_A) |
                     (1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))) ||
         axes[5].load() > 16384)) {
        shoot(self);
        static int fire_logs = 0;
        if (trace && fire_logs++ < 8)
            api.log("SQUIDS", "fire hp=%d frame=%d", player_hp, player_frame);
    }
    if (trace && self && player_frame % 30 == 0) {
        const auto v = *reinterpret_cast<V3*>(static_cast<char*>(self) + kPlayerInputOffset);
        api.log("SQUIDS", "player hp=%d input=(%.2f,%.2f) frame=%d",
                player_hp, v.x, v.y, player_frame);
    }
}
static void on_button(int button, int down, void*) {
    if (button < 0 || button >= 32) return;
    const uint32_t bit = 1u << button;
    if (down) { if (!(held.fetch_or(bit) & bit)) edges.fetch_or(bit); }
    else held.fetch_and(~bit);
}
static void on_axis(int axis, int value, void*) {
    if (axis >= 0 && axis < 6) axes[axis].store(value);
}
static void post_init(void*) {
    if (!rt.bind(&api, "SQUIDS")) return;
    trace = api.getenv("SPACESQUIDS_TRACE") != nullptr;
    bool ok = true;
    ok &= get_transform.bind("UnityEngine", "Component", "get_transform", 0);
    ok &= get_go.bind("UnityEngine", "Component", "get_gameObject", 0);
    ok &= get_name.bind("UnityEngine", "Object", "get_name", 0);
    ok &= get_component.bind("UnityEngine", "Component", "GetComponent", 1);
    ok &= canvas_camera.bind("UnityEngine", "Canvas", "get_worldCamera", 0);
    ok &= parent.bind("UnityEngine", "Transform", "get_parent", 0);
    ok &= active.bind("UnityEngine", "GameObject", "get_activeInHierarchy", 0);
    if (!find_all.bind("UnityEngine", "Resources", "FindObjectsOfTypeAll", 1))
        ok &= find_all.bind("UnityEngine", "Object", "FindObjectsOfType", 1);
    ok &= get_rect.bind("UnityEngine", "RectTransform", "get_rect", 0);
    ok &= transform_point.bind("UnityEngine", "Transform", "TransformPoint", 1);
    ok &= world_to_screen.bind("UnityEngine", "RectTransformUtility", "WorldToScreenPoint", 2);
    ok &= screen_w.bind("UnityEngine", "Screen", "get_width", 0);
    ok &= screen_h.bind("UnityEngine", "Screen", "get_height", 0);
    ok &= frame_count.bind("UnityEngine", "Time", "get_frameCount", 0);
    ok &= is_active.bind("UnityEngine.UI", "Selectable", "IsActive", 0);
    ok &= interactable.bind("UnityEngine.UI", "Selectable", "IsInteractable", 0);
    ok &= target_graphic.bind("UnityEngine.UI", "Selectable", "get_targetGraphic", 0);
    ok &= graphic_renderer.bind("UnityEngine.UI", "Graphic", "get_canvasRenderer", 0);
    ok &= renderer_culled.bind("UnityEngine", "CanvasRenderer", "get_cull", 0);
    ok &= graphic_raycast.bind("UnityEngine.UI", "Graphic", "Raycast", 2);
    ok &= scroll_content.bind("UnityEngine.UI", "ScrollRect", "get_content", 0);
    ok &= scroll_viewport.bind("UnityEngine.UI", "ScrollRect", "get_viewport", 0);
    ok &= anchored_position.bind("UnityEngine", "RectTransform", "get_anchoredPosition", 0);
    ok &= set_anchored_position.bind("UnityEngine", "RectTransform", "set_anchoredPosition", 1);
    ok &= stop_scroll.bind("UnityEngine.UI", "ScrollRect", "StopMovement", 0);
    ok &= slider_handle.bind("UnityEngine.UI", "Slider", "get_handleRect", 0);
    ok &= shoot.bind("", "PlayerScript", "Sparo", 0);
    ok &= rocket.bind("", "PlayerScript", "Razzo", 0);
    selectable_type = rt.resolve_type_object(rt.find_class_image("UnityEngine.UI", "Selectable"),
                                             "UnityEngine.UI", "Selectable");
    canvas_type = rt.resolve_type_object(rt.find_class_image("UnityEngine", "Canvas"),
                                         "UnityEngine", "Canvas");
    scroll_type = rt.resolve_type_object(rt.find_class_image("UnityEngine.UI", "ScrollRect"),
                                         "UnityEngine.UI", "ScrollRect");
    slider_type = rt.resolve_type_object(rt.find_class_image("UnityEngine.UI", "Slider"),
                                         "UnityEngine.UI", "Slider");
    void* image = rt.find_image("Assembly-CSharp");
    if (api.getenv("SPACESQUIDS_DUMP")) {
        rt.log_class(image, "", "PlayerScript"); rt.log_class(image, "", "GameGestor");
        rt.log_class(image, "", "MainMenu_Script");
    }
    hp_offset = rt.field_offset_for(image, "PlayerScript", "HP");
    ammo_offset = rt.field_offset_for(image, "PlayerScript", "numeroRazzi");
    paused_offset = rt.field_offset_for(image, "GameGestor", "pause");
    if (!ok || !selectable_type || !canvas_type || !scroll_type || !slider_type ||
        hp_offset != 0x24 || paused_offset != 0x28) {
        api.log("SQUIDS", "UI binding/layout failed; adapter disabled"); return;
    }
    rt.hook_method(image, "", "MainMenu_Script", "Update", 0,
                   reinterpret_cast<uintptr_t>(&menu_update), &original_menu);
    rt.hook_method(image, "", "GameGestor", "Update", 0,
                   reinterpret_cast<uintptr_t>(&game_update), &original_game);
    rt.hook_method(image, "", "PlayerScript", "Update", 0,
                   reinterpret_cast<uintptr_t>(&player_update), &original_player);
    BogoInputObserver observer{}; observer.button = on_button; observer.axis = on_axis;
    api.register_input_observer(&observer);
    api.log("SQUIDS", "touch UI adapter ready: A click/fire, X rocket, directions snap, right stick cursor, Guide settings");
}
} // namespace spacesquids
extern "C" int bogodroid_plugin_init(const BogoPluginApi* api) {
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(BogoPluginApi) ||
        !api->set_controller_capture || !api->set_present_cursor) return -1;
    spacesquids::api = *api;
    if (!api->il2cpp) return BOGO_PLUGIN_DEFERRED;
    return api->register_il2cpp_post_init(spacesquids::post_init, nullptr)
        ? BOGO_PLUGIN_OK : -1;
}
