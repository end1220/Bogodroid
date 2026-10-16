#include "gameover_icons.h"
#include "gameover_icons_png.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace brickgamepro {
namespace {

struct V2 { float x, y; };
struct V3 { float x, y, z; };
struct Rect { float x, y, w, h; };

const BogoPluginApi* api = nullptr;
bogodroid::Il2cppRuntime* runtime = nullptr;
void* (*invoke)(void*, void*, void**, void**) = nullptr;
void* (*object_new)(void*) = nullptr;
void* (*array_new)(void*, uintptr_t) = nullptr;
void* (*string_new)(const char*) = nullptr;
void* (*unbox)(void*) = nullptr;
uint32_t (*retain)(void*, bool) = nullptr;
void (*release)(uint32_t) = nullptr;

void *go_class, *texture_class, *byte_class, *rect_type, *raw_type;
void *go_ctor, *add_component, *set_parent, *set_active, *destroy;
void *get_min, *get_max, *get_pivot, *get_position, *get_size;
void *set_min, *set_max, *set_pivot, *set_position, *set_size, *set_scale;
void *texture_ctor, *load_image, *set_texture, *set_uv, *set_raycast;
void *texture = nullptr;
uint32_t texture_root = 0;
bool ready = false, texture_attempted = false;
bool installed = false;
constexpr float kIconAspect = 651.0f / 384.0f;
constexpr float kIconScale = 1.15f;
constexpr float kIconDownshift = 8.0f;

void* method(const char* ns, const char* klass, const char* name, int argc)
{
    void* image = runtime->find_class_image(ns, klass);
    void* k = image ? runtime->f_class_from_name(image, ns, klass) : nullptr;
    void* mi = k ? runtime->f_get_method(k, name, argc) : nullptr;
    if (!mi)
        api->log("BRICKGAME", "Finish icons missing %s.%s(%d)", klass, name, argc);
    return mi;
}

void* call(void* mi, void* self, void** args, bool& ok)
{
    if (!ok) return nullptr;
    void* exception = nullptr;
    void* result = invoke(mi, self, args, &exception);
    if (exception) {
        api->log("BRICKGAME", "Finish icons managed call failed method=%p exception=%p", mi, exception);
        ok = false;
    }
    return result;
}

template<typename T> T read(void* mi, void* self, bool& ok)
{
    void* boxed = call(mi, self, nullptr, ok);
    if (!boxed) { ok = false; return {}; }
    return *static_cast<T*>(unbox(boxed));
}

template<typename T> void write(void* mi, void* self, T value, bool& ok)
{
    void* args[] = {&value};
    call(mi, self, args, ok);
}

bool load_texture()
{
    if (texture_attempted) return texture != nullptr;
    texture_attempted = true;
    texture = object_new(texture_class);
    if (!texture) return false;
    texture_root = retain(texture, false);
    bool ok = true;
    int size = 2;
    void* dimensions[] = {&size, &size};
    call(texture_ctor, texture, dimensions, ok);
    void* bytes = array_new(byte_class, sizeof(kFinishIconsPng));
    uint32_t bytes_root = bytes ? retain(bytes, false) : 0;
    if (bytes) {
        // IL2CPP array header: class, monitor, bounds, max_length (ARM64).
        std::memcpy(static_cast<char*>(bytes) + 4 * sizeof(void*),
                    kFinishIconsPng, sizeof(kFinishIconsPng));
        bool discard_cpu_copy = true;
        void* args[] = {texture, bytes, &discard_cpu_copy};
        void* result = call(load_image, nullptr, args, ok);
        ok = ok && result && *static_cast<bool*>(unbox(result));
    } else {
        ok = false;
    }
    if (bytes_root) release(bytes_root);
    if (!ok) {
        bool cleanup = true;
        void* args[] = {texture};
        call(destroy, nullptr, args, cleanup);
        release(texture_root);
        texture_root = 0;
        texture = nullptr;
        api->log("BRICKGAME", "Finish icons PNG load failed");
    }
    return ok;
}

} // namespace

void bind_gameover_icons(bogodroid::Il2cppRuntime& rt, const BogoPluginApi* plugin_api)
{
    runtime = &rt;
    api = plugin_api;
#define EXPORT(name, symbol) name = reinterpret_cast<decltype(name)>(api->so_symbol(api->il2cpp, symbol))
    EXPORT(invoke, "il2cpp_runtime_invoke");
    EXPORT(object_new, "il2cpp_object_new");
    EXPORT(array_new, "il2cpp_array_new");
    EXPORT(string_new, "il2cpp_string_new");
    EXPORT(unbox, "il2cpp_object_unbox");
    EXPORT(retain, "il2cpp_gchandle_new");
    EXPORT(release, "il2cpp_gchandle_free");
#undef EXPORT
    if (!invoke || !object_new || !array_new || !string_new || !unbox || !retain || !release)
        return;
    void* core = rt.find_class_image("UnityEngine", "GameObject");
    void* ui = rt.find_class_image("UnityEngine.UI", "RawImage");
    void* corlib = rt.find_class_image("System", "Byte");
    if (!core || !ui || !corlib) return;
    go_class = rt.f_class_from_name(core, "UnityEngine", "GameObject");
    texture_class = rt.f_class_from_name(core, "UnityEngine", "Texture2D");
    byte_class = rt.f_class_from_name(corlib, "System", "Byte");
    rect_type = rt.resolve_type_object(core, "UnityEngine", "RectTransform");
    raw_type = rt.resolve_type_object(ui, "UnityEngine.UI", "RawImage");
    bool ok = go_class && texture_class && byte_class && rect_type && raw_type;
#define METHOD(slot, ns, klass, name, argc) slot = method(ns, klass, name, argc); ok = slot && ok
    METHOD(go_ctor, "UnityEngine", "GameObject", ".ctor", 1);
    METHOD(add_component, "UnityEngine", "GameObject", "AddComponent", 1);
    METHOD(set_active, "UnityEngine", "GameObject", "SetActive", 1);
    METHOD(destroy, "UnityEngine", "Object", "Destroy", 1);
    METHOD(set_parent, "UnityEngine", "Transform", "SetParent", 2);
    METHOD(set_scale, "UnityEngine", "Transform", "set_localScale", 1);
    METHOD(get_min, "UnityEngine", "RectTransform", "get_anchorMin", 0);
    METHOD(get_max, "UnityEngine", "RectTransform", "get_anchorMax", 0);
    METHOD(get_pivot, "UnityEngine", "RectTransform", "get_pivot", 0);
    METHOD(get_position, "UnityEngine", "RectTransform", "get_anchoredPosition", 0);
    METHOD(get_size, "UnityEngine", "RectTransform", "get_sizeDelta", 0);
    METHOD(set_min, "UnityEngine", "RectTransform", "set_anchorMin", 1);
    METHOD(set_max, "UnityEngine", "RectTransform", "set_anchorMax", 1);
    METHOD(set_pivot, "UnityEngine", "RectTransform", "set_pivot", 1);
    METHOD(set_position, "UnityEngine", "RectTransform", "set_anchoredPosition", 1);
    METHOD(set_size, "UnityEngine", "RectTransform", "set_sizeDelta", 1);
    METHOD(texture_ctor, "UnityEngine", "Texture2D", ".ctor", 2);
    METHOD(load_image, "UnityEngine", "ImageConversion", "LoadImage", 3);
    METHOD(set_texture, "UnityEngine.UI", "RawImage", "set_texture", 1);
    METHOD(set_uv, "UnityEngine.UI", "RawImage", "set_uvRect", 1);
    METHOD(set_raycast, "UnityEngine.UI", "Graphic", "set_raycastTarget", 1);
#undef METHOD
    ready = ok;
    api->log("BRICKGAME", "Finish icons bindings ready=%d", ready ? 1 : 0);
}

void add_gameover_icon(void* source, void* parent, const char* action)
{
    // The original Share button is only a placement reference. All original
    // actions remain hidden; the single decoration lives beside them.
    if (!ready || std::strcmp(action, "ShareBtn") != 0 || installed || !load_texture()) return;
    bool ok = true;
    const V2 anchor_min = read<V2>(get_min, source, ok);
    const V2 anchor_max = read<V2>(get_max, source, ok);
    const V2 pivot = read<V2>(get_pivot, source, ok);
    V2 position = read<V2>(get_position, source, ok);
    V2 size = read<V2>(get_size, source, ok);
    if (!ok || size.x <= 0 || size.y <= 0) return;
    // Enlarge the previous single icon by 15% while preserving its aspect.
    // Retain the full-resolution transparent PNG; no offline downsampling.
    const float scale = 2.0f * std::min(size.x / 270.0f, size.y / 180.0f);
    const float height = 168.0f * scale * kIconScale;
    const V2 fitted = {height * kIconAspect, height};
    position.x = 0;
    position.y += (0.5f - pivot.y) * size.y - kIconDownshift;
    void* go = object_new(go_class);
    if (!go) return;
    const uint32_t go_root = retain(go, false);
    void* ctor_args[] = {string_new("FinishGameOverIcon")};
    call(go_ctor, go, ctor_args, ok);
    write(set_active, go, false, ok);
    void* rt_args[] = {rect_type};
    void* rect = call(add_component, go, rt_args, ok);
    bool world_position_stays = false;
    void* parent_args[] = {parent, &world_position_stays};
    call(set_parent, rect, parent_args, ok);
    write(set_scale, rect, V3{1, 1, 1}, ok);
    write(set_min, rect, V2{0.5f, anchor_min.y}, ok);
    write(set_max, rect, V2{0.5f, anchor_max.y}, ok);
    write(set_pivot, rect, V2{0.5f, 0.5f}, ok);
    write(set_size, rect, fitted, ok);
    write(set_position, rect, position, ok);
    void* raw_args[] = {raw_type};
    void* raw = call(add_component, go, raw_args, ok);
    void* texture_args[] = {texture};
    call(set_texture, raw, texture_args, ok);
    write(set_uv, raw, Rect{0, 0, 1, 1}, ok);
    write(set_raycast, raw, false, ok);
    write(set_active, go, true, ok);
    if (!ok) {
        bool cleanup = true;
        void* args[] = {go};
        call(destroy, nullptr, args, cleanup);
    }
    release(go_root);
    installed = ok;
    api->log("BRICKGAME", "Finish icon GameOver installed=%d center=(%.1f,%.1f) size=(%.1f,%.1f)",
             ok ? 1 : 0, position.x, position.y, fitted.x, fitted.y);
    if (texture_root && installed) {
        // RawImage.texture owns the managed reference for this scene.
        release(texture_root);
        texture_root = 0;
    }
}

} // namespace brickgamepro
