#include "plugin_api.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace skul_pad {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static BogoSoModule* g_mod = nullptr;
static bool g_enabled = false;
static std::string g_asset_root;

using p_domain_get = void* (*)();
using p_domain_get_assemblies = void** (*)(void*, size_t*);
using p_assembly_get_image = void* (*)(void*);
using p_image_get_name = const char* (*)(void*);
using p_class_from_name = void* (*)(void*, const char*, const char*);
using p_class_get_methods = void* (*)(void*, void**);
using p_class_from_type = void* (*)(void*);
using p_class_get_name = const char* (*)(void*);
using p_class_get_field_from_name = void* (*)(void*, const char*);
using p_get_method = void* (*)(void*, const char*, int);
using p_method_get_name = const char* (*)(void*);
using p_method_get_param_count = uint32_t (*)(void*);
using p_method_get_param = void* (*)(void*, uint32_t);
using p_field_static_get_value = void (*)(void*, void*);
using p_field_static_set_value = void (*)(void*, void*);
using p_runtime_class_init = void (*)(void*);
using p_gchandle_new = uint32_t (*)(void*, bool);
using p_gchandle_free = void (*)(uint32_t);
using p_string_new = void* (*)(const char*);
using p_string_length = int32_t (*)(void*);
using p_string_chars = uint16_t* (*)(void*);
using p_array_class_get = void* (*)(void*, uint32_t);
using p_array_new = void* (*)(void*, uintptr_t);

static p_domain_get f_domain_get = nullptr;
static p_domain_get_assemblies f_domain_assemblies = nullptr;
static p_assembly_get_image f_assembly_image = nullptr;
static p_image_get_name f_image_name = nullptr;
static p_class_from_name f_class_from_name = nullptr;
static p_class_get_methods f_class_get_methods = nullptr;
static p_class_from_type f_class_from_type = nullptr;
static p_class_get_name f_class_get_name = nullptr;
static p_class_get_field_from_name f_class_get_field_from_name = nullptr;
static p_get_method f_get_method = nullptr;
static p_method_get_name f_method_get_name = nullptr;
static p_method_get_param_count f_method_get_param_count = nullptr;
static p_method_get_param f_method_get_param = nullptr;
static p_field_static_get_value f_field_static_get_value = nullptr;
static p_field_static_set_value f_field_static_set_value = nullptr;
static p_runtime_class_init f_runtime_class_init = nullptr;
static p_gchandle_new f_gchandle_new = nullptr;
static p_gchandle_free f_gchandle_free = nullptr;
static p_string_new f_string_new = nullptr;
static p_string_length f_string_length = nullptr;
static p_string_chars f_string_chars = nullptr;
static p_array_class_get f_array_class_get = nullptr;
static p_array_new f_array_new = nullptr;

static void* find_image(const char* needle)
{
    void* domain = f_domain_get();
    size_t count = 0;
    void** assemblies = f_domain_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        void* image = f_assembly_image(assemblies[i]);
        const char* name = image ? f_image_name(image) : nullptr;
        if (name && std::strstr(name, needle))
            return image;
    }
    return nullptr;
}

static void* find_image_exact(const char* image_name)
{
    void* domain = f_domain_get();
    size_t count = 0;
    void** assemblies = f_domain_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        void* image = f_assembly_image(assemblies[i]);
        const char* name = image ? f_image_name(image) : nullptr;
        if (!name)
            continue;
        if (std::strcmp(name, image_name) == 0)
            return image;
        const size_t length = std::strlen(name);
        const size_t wanted = std::strlen(image_name);
        if (length == wanted + 4 && std::strncmp(name, image_name, wanted) == 0 &&
            std::strcmp(name + wanted, ".dll") == 0)
            return image;
    }
    return nullptr;
}

static std::string managed_string(void* value)
{
    if (!value)
        return {};
    int32_t length = f_string_length(value);
    uint16_t* chars = f_string_chars(value);
    if (length <= 0 || !chars)
        return {};
    std::string result;
    result.reserve(static_cast<size_t>(length));
    for (int32_t i = 0; i < length; ++i) {
        uint16_t c = chars[i];
        result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return result;
}

static std::string bundle_path(const std::string& hash)
{
    if (hash.empty())
        return {};
    std::filesystem::path root = g_asset_root.empty()
        ? std::filesystem::path(g_api->config_get_string("paths.android_files", "../conf"))
        : std::filesystem::path(g_asset_root);
    if (root.is_relative())
        root = std::filesystem::current_path() / root;
    return (root.lexically_normal() / "assetpacks" / "CustomFastFollow" /
            "48" / "48" / "assets" / (hash + ".bundle")).string();
}

static std::string core_asset_path()
{
    return std::filesystem::current_path().lexically_normal().string();
}

static std::string custom_pack_root()
{
    std::filesystem::path root = g_asset_root.empty()
        ? std::filesystem::path(g_api->config_get_string("paths.android_files", "../conf"))
        : std::filesystem::path(g_asset_root);
    if (root.is_relative())
        root = std::filesystem::current_path() / root;
    return (root.lexically_normal() / "assetpacks" / "CustomFastFollow" /
            "48" / "48").string();
}

static bool is_core_pack_name(const std::string& name)
{
    return name == "UnityDataAssetPack" || name == "UnityStreamingAssetsPack";
}

static void* make_names_array()
{
    void* image = find_image("mscorlib");
    void* string_class = image ? f_class_from_name(image, "System", "String") : nullptr;
    void* array_class = string_class ? f_array_class_get(string_class, 1) : nullptr;
    void* array = array_class ? f_array_new(array_class, 1) : nullptr;
    if (!array)
        return nullptr;

    // On arm64, Il2CppArray's managed reference vector starts at +0x20.
    auto elements = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(array) + 0x20);
    elements[0] = f_string_new("CustomFastFollow");
    return array;
}

using orig8_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                              uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using orig2_void_t = void (*)(uintptr_t, uintptr_t);
using orig3_void_t = void (*)(uintptr_t, uintptr_t, uintptr_t);
using orig3_ptr_t = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using orig2_bool_t = uint8_t (*)(uintptr_t, uintptr_t);
using orig2_float_t = float (*)(uintptr_t, uintptr_t);
using orig3_bool_void_t = void (*)(uintptr_t, uint8_t, uintptr_t);
using orig_load_scene_internal_t = uintptr_t (*)(uintptr_t, int32_t, uintptr_t,
                                                  uint8_t, uintptr_t);
static orig2_void_t g_check_and_load = nullptr;
static orig2_void_t g_start_custom_downloading = nullptr;
static orig3_void_t g_initialize_bundle_map = nullptr;
static orig2_void_t g_load_scene_string = nullptr;
static orig3_ptr_t g_app_bundle_transform = nullptr;
static orig_load_scene_internal_t g_load_scene_internal = nullptr;
static orig2_bool_t g_async_is_done = nullptr;
static orig2_float_t g_async_progress = nullptr;
static orig2_bool_t g_async_get_allow_activation = nullptr;
static orig3_bool_void_t g_async_set_allow_activation = nullptr;
static uintptr_t g_async_is_done_method = 0;
static uintptr_t g_async_progress_method = 0;
static uintptr_t g_async_get_allow_method = 0;
static uintptr_t g_async_set_allow_method = 0;
static uintptr_t g_scene_operation = 0;
static uint32_t g_scene_operation_handle = 0;
static void* g_scene_manager_class = nullptr;
static void* g_allow_load_scene_field = nullptr;
static bool g_bundle_map_initialized = false;
static bool g_logged_map_completion = false;
static bool g_logged_load_dispatch = false;
static bool g_logged_core_pack_files = false;
static uint32_t g_check_and_load_frames = 0;
static uint32_t g_operation_poll_calls = 0;
static uint32_t g_transform_calls = 0;
static float g_last_scene_progress = -1.0f;
static bool g_activation_pulsed = false;

static bool ensure_scene_load_allowed()
{
    if (!g_scene_manager_class || !g_allow_load_scene_field)
        return false;
    f_runtime_class_init(g_scene_manager_class);
    uint8_t allowed = 0;
    f_field_static_get_value(g_allow_load_scene_field, &allowed);
    if (!allowed) {
        const uint8_t enabled = 1;
        f_field_static_set_value(g_allow_load_scene_field,
                                 const_cast<uint8_t*>(&enabled));
        g_api->log("SKULPAD", "SceneManager.s_AllowLoadScene restored 0 -> 1");
    }
    return allowed != 0;
}

static int32_t managed_list_count(uintptr_t list)
{
    // System.Collections.Generic.List<T> stores _size at +0x18 on arm64.
    return list ? *reinterpret_cast<int32_t*>(list + 0x18) : -1;
}

static uintptr_t hook_downloaded(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return 1;
}

static void poll_scene_operation(const char* source)
{
    if (!g_scene_operation || !g_async_is_done || !g_async_progress ||
        !g_async_get_allow_activation || !g_async_set_allow_activation)
        return;

    ++g_operation_poll_calls;
    const float progress = g_async_progress(
        g_scene_operation, g_async_progress_method);
    const bool done = g_async_is_done(
        g_scene_operation, g_async_is_done_method) != 0;
    bool allow_activation = g_async_get_allow_activation(
        g_scene_operation, g_async_get_allow_method) != 0;
    if (progress >= 0.89f && !done && !g_activation_pulsed) {
        g_async_set_allow_activation(
            g_scene_operation, 0, g_async_set_allow_method);
        g_async_set_allow_activation(
            g_scene_operation, 1, g_async_set_allow_method);
        g_activation_pulsed = true;
        allow_activation = true;
        g_api->log("SKULPAD", "pulsed PlatformLoader scene activation 1 -> 0 -> 1");
    } else if (!allow_activation) {
        g_async_set_allow_activation(
            g_scene_operation, 1, g_async_set_allow_method);
        allow_activation = true;
        g_api->log("SKULPAD", "enabled PlatformLoader scene activation");
    }
    if (g_operation_poll_calls <= 6 ||
        g_operation_poll_calls == 30 ||
        g_operation_poll_calls == 180 ||
        progress != g_last_scene_progress || done) {
        g_api->log("SKULPAD", "PlatformLoader operation poll=%u source=%s progress=%.3f done=%d allowActivation=%d native=%p",
                   g_operation_poll_calls, source ? source : "?",
                   progress, done ? 1 : 0, allow_activation ? 1 : 0,
                   reinterpret_cast<void*>(
                       *reinterpret_cast<uintptr_t*>(g_scene_operation + 0x10)));
        g_last_scene_progress = progress;
    }
    if (done && g_scene_operation_handle) {
        f_gchandle_free(g_scene_operation_handle);
        g_scene_operation_handle = 0;
        g_scene_operation = 0;
    }
}

static float hook_percentage(uintptr_t, uintptr_t)
{
    poll_scene_operation("Percentage");
    return 1.0f;
}

static void hook_start_custom_downloading(uintptr_t downloader, uintptr_t method)
{
    if (!g_start_custom_downloading)
        return;
    const uintptr_t pending_before = downloader
        ? *reinterpret_cast<uintptr_t*>(downloader + 0x28) : 0;
    g_api->log("SKULPAD", "StartCustomAssetDownloading enter pending=%d flags=%d/%d",
               managed_list_count(pending_before),
               downloader ? *reinterpret_cast<uint8_t*>(downloader + 0x38) : 0,
               downloader ? *reinterpret_cast<uint8_t*>(downloader + 0x39) : 0);
    g_start_custom_downloading(downloader, method);
    const uintptr_t pending_after = downloader
        ? *reinterpret_cast<uintptr_t*>(downloader + 0x28) : 0;
    g_api->log("SKULPAD", "StartCustomAssetDownloading leave pending=%d flags=%d/%d",
               managed_list_count(pending_after),
               downloader ? *reinterpret_cast<uint8_t*>(downloader + 0x38) : 0,
               downloader ? *reinterpret_cast<uint8_t*>(downloader + 0x39) : 0);
}

static void hook_initialize_bundle_map(uintptr_t downloader, uintptr_t contents,
                                       uintptr_t method)
{
    g_api->log("SKULPAD", "InitializeBundleToAssetPackMap enter jsonLength=%d",
               contents ? f_string_length(reinterpret_cast<void*>(contents)) : -1);
    if (g_initialize_bundle_map)
        g_initialize_bundle_map(downloader, contents, method);
    // The original caller installs the Addressables transform and writes the
    // processed flag immediately after this method returns. CheckAndLoad runs
    // on a later Update, at which point it is safe to complete the local pack.
    g_bundle_map_initialized = true;
    g_api->log("SKULPAD", "InitializeBundleToAssetPackMap leave");
}

static void hook_check_and_load(uintptr_t scene_loader, uintptr_t method)
{
    ++g_check_and_load_frames;
    if (scene_loader) {
        // AssetPackSceneLoader.downloader is at +0x30. In this exact Skul
        // build AssetPackDownloader's final two fields are the completion
        // gates at +0x38/+0x39 (verified from metadata and disassembly).
        uintptr_t downloader = *reinterpret_cast<uintptr_t*>(scene_loader + 0x30);
        const bool loading = *reinterpret_cast<uint8_t*>(scene_loader + 0x38) != 0;
        auto scene_field = reinterpret_cast<uintptr_t*>(scene_loader + 0x18);
        std::string scene = managed_string(reinterpret_cast<void*>(*scene_field));
        // data.unity3d identifies PlatformLoader as the scene following
        // AndroidDownloadPacks. Preserve the serialized value whenever it is
        // present, but recover from a missing field instead of asking Unity to
        // load an empty scene name.
        if (scene.empty()) {
            *scene_field = reinterpret_cast<uintptr_t>(f_string_new("PlatformLoader"));
            scene = "PlatformLoader";
            g_api->log("SKULPAD", "SceneToLoad was empty; restored '%s'", scene.c_str());
        }
        if (g_scene_operation) {
            poll_scene_operation("CheckAndLoad");
            return;
        }
        if (downloader && (g_check_and_load_frames <= 3 ||
                           g_check_and_load_frames == 30 ||
                           g_check_and_load_frames == 180)) {
            const uintptr_t pending = *reinterpret_cast<uintptr_t*>(downloader + 0x28);
            g_api->log("SKULPAD", "CheckAndLoad call=%u scene='%s' loading=%d pending=%d flags=%d/%d map=%d",
                       g_check_and_load_frames, scene.c_str(), loading ? 1 : 0,
                       managed_list_count(pending),
                       *reinterpret_cast<uint8_t*>(downloader + 0x38),
                       *reinterpret_cast<uint8_t*>(downloader + 0x39),
                       g_bundle_map_initialized ? 1 : 0);
        }
        // Never bypass into the next scene before the JSON parser has built
        // the Addressables bundle-to-pack transform: doing so produces the
        // observed black screen. Once mapping is installed, both packs are
        // local and the completion gates can safely be finalized.
        if (downloader && g_bundle_map_initialized) {
            *reinterpret_cast<uint8_t*>(downloader + 0x38) = 1;
            *reinterpret_cast<uint8_t*>(downloader + 0x39) = 1;
            if (!g_logged_map_completion) {
                g_api->log("SKULPAD", "completed downloader after bundle map initialization");
                g_logged_map_completion = true;
            }
        }
        if (downloader && g_bundle_map_initialized && !loading &&
            *reinterpret_cast<uint8_t*>(downloader + 0x38) &&
            *reinterpret_cast<uint8_t*>(downloader + 0x39) &&
            !g_logged_load_dispatch) {
            g_api->log("SKULPAD", "dispatching completed PAD scene='%s'", scene.c_str());
            g_logged_load_dispatch = true;
            // Let the game's completion branch preserve its original scene
            // loading and component lifetime semantics. The earlier manual
            // LoadScene path kept this bootstrap component alive and could
            // interfere with Unity's own scene transition bookkeeping.
            if (g_check_and_load) {
                g_check_and_load(scene_loader, method);
                return;
            }
        }
    }
    if (g_check_and_load)
        g_check_and_load(scene_loader, method);
}

static void hook_load_scene_string(uintptr_t scene_name, uintptr_t method)
{
    const std::string scene = managed_string(reinterpret_cast<void*>(scene_name));
    const bool was_allowed = ensure_scene_load_allowed();
    g_api->log("SKULPAD", "SceneManager.LoadScene(string) scene='%s' allowedBefore=%d",
               scene.c_str(), was_allowed ? 1 : 0);
    if (g_load_scene_string)
        g_load_scene_string(scene_name, method);
    g_api->log("SKULPAD", "SceneManager.LoadScene(string) returned scene='%s'", scene.c_str());
}

static uintptr_t hook_load_scene_internal(uintptr_t scene_name,
                                          int32_t scene_build_index,
                                          uintptr_t parameters,
                                          uint8_t must_complete_next_frame,
                                          uintptr_t method)
{
    ensure_scene_load_allowed();
    const std::string scene = managed_string(reinterpret_cast<void*>(scene_name));
    // Preserve the game's synchronous next-frame scene commit now that the
    // Skul-only CPU framebuffer presenter prevents the following fbdev flip
    // from deadlocking.  The earlier async conversion reached 0.9 but never
    // ran Unity's synchronous activation path. Keep it as an opt-in diagnostic
    // switch rather than changing the title's normal bootstrap semantics.
    const bool force_async = scene == "PlatformLoader" &&
        g_api->config_get_bool(
            "game_patches.skul_pad.force_async_platform_loader", 0);
    const uint8_t effective_complete_next_frame =
        force_async ? 0 : must_complete_next_frame;
    uintptr_t effective_scene_name = scene_name;
    std::string effective_scene = scene;
    if (scene == "PlatformLoader" && g_api->config_get_bool(
            "game_patches.skul_pad.direct_title_scene", 0)) {
        effective_scene = "Title";
        effective_scene_name = reinterpret_cast<uintptr_t>(
            f_string_new(effective_scene.c_str()));
        g_api->log("SKULPAD", "redirecting scene PlatformLoader -> Title");
    }
    if (scene == "PlatformLoader") {
        int64_t pause_ms = g_api->config_get_i64(
            "game_patches.skul_pad.transition_swap_pause_ms", 3000);
        if (pause_ms > 0) {
            if (pause_ms > 15000)
                pause_ms = 15000;
            const std::string duration = std::to_string(pause_ms);
            setenv("BD_EGL_SWAP_PAUSE_MS", duration.c_str(), 1);
            setenv("BD_EGL_SWAP_PAUSE", "1", 1);
            g_api->log("SKULPAD", "armed PlatformLoader swap pause for %lld ms",
                       static_cast<long long>(pause_ms));
        }
        if (g_api->config_get_bool(
                "game_patches.skul_pad.cpu_framebuffer_present", 1)) {
            setenv("BD_EGL_CPU_PRESENT", "1", 1);
            g_api->log("SKULPAD", "enabled PlatformLoader CPU framebuffer present");
        }
    }
    uintptr_t operation = g_load_scene_internal
        ? g_load_scene_internal(effective_scene_name, scene_build_index, parameters,
                                effective_complete_next_frame, method)
        : 0;
    if (scene == "PlatformLoader") {
        g_scene_operation = operation;
        g_operation_poll_calls = 0;
        g_last_scene_progress = -1.0f;
        g_activation_pulsed = false;
        if (operation && !g_scene_operation_handle) {
            g_scene_operation_handle = f_gchandle_new(
                reinterpret_cast<void*>(operation), false);
            g_api->log("SKULPAD", "rooted PlatformLoader operation gchandle=%u",
                       g_scene_operation_handle);
        }
    }
    const uintptr_t native_operation = operation
        ? *reinterpret_cast<uintptr_t*>(operation + 0x10) : 0;
    g_api->log("SKULPAD", "LoadSceneAsyncNameIndexInternal scene='%s' effective='%s' index=%d completeNextFrame=%d->%d operation=%p native=%p",
               scene.c_str(), effective_scene.c_str(), scene_build_index,
               must_complete_next_frame ? 1 : 0,
               effective_complete_next_frame ? 1 : 0,
               reinterpret_cast<void*>(operation),
               reinterpret_cast<void*>(native_operation));
    return operation;
}

static uintptr_t hook_app_bundle_transform(uintptr_t downloader,
                                           uintptr_t location,
                                           uintptr_t method)
{
    uintptr_t transformed = g_app_bundle_transform
        ? g_app_bundle_transform(downloader, location, method) : 0;
    ++g_transform_calls;
    if (g_transform_calls <= 24) {
        const std::string path = managed_string(reinterpret_cast<void*>(transformed));
        std::error_code ec;
        const bool exists = !path.empty() && std::filesystem::exists(path, ec);
        g_api->log("SKULPAD", "AppBundleTransformFunc call=%u path='%s' exists=%d error=%d",
                   g_transform_calls, path.c_str(), exists ? 1 : 0,
                   ec ? ec.value() : 0);
    }
    return transformed;
}

static uintptr_t hook_empty_string(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                   uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(f_string_new(""));
}

static uintptr_t hook_pack_names(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                 uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(make_names_array());
}

static uintptr_t hook_pack_path(uintptr_t asset_pack_name, uintptr_t, uintptr_t,
                                uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    std::string name = managed_string(reinterpret_cast<void*>(asset_pack_name));
    std::string path;
    if (is_core_pack_name(name)) {
        path = core_asset_path();
        if (!g_logged_core_pack_files) {
            const std::filesystem::path root = path;
            std::error_code ec;
            const bool data_exists = std::filesystem::exists(
                root / "bin" / "Data" / "data.unity3d", ec);
            const bool datapack_exists = std::filesystem::exists(
                root / "bin" / "Data" / "datapack.unity3d", ec);
            const bool data_nested_exists = std::filesystem::exists(
                root / "assets" / "bin" / "Data" / "data.unity3d", ec);
            g_api->log("SKULPAD", "core pack root='%s' data=%d datapack=%d nestedData=%d",
                       path.c_str(), data_exists ? 1 : 0,
                       datapack_exists ? 1 : 0,
                       data_nested_exists ? 1 : 0);
            g_logged_core_pack_files = true;
        }
    } else if (name == "CustomFastFollow") {
        path = custom_pack_root();
    } else {
        path = bundle_path(name);
    }
    g_api->log("SKULPAD", "GetAssetPackPath name=%s -> %s",
               name.c_str(), path.c_str());
    return path.empty() ? 0 : reinterpret_cast<uintptr_t>(f_string_new(path.c_str()));
}

static bool hook_method(void* image, const char* name, int argc, uintptr_t hook)
{
    void* klass = f_class_from_name(image, "UnityEngine.Android", "AndroidAssetPacks");
    if (!klass) {
        g_api->log("SKULPAD", "class UnityEngine.Android.AndroidAssetPacks not found");
        return false;
    }
    void* method = f_get_method(klass, name, argc);
    if (!method) {
        g_api->log("SKULPAD", "method %s(%d) not found", name, argc);
        return false;
    }
    uintptr_t address = *reinterpret_cast<uintptr_t*>(method);
    uintptr_t original = 0;
    g_api->hook_address_detour(g_mod, address, hook, &original);
    if (!original) {
        g_api->log("SKULPAD", "detour failed for %s(%d)", name, argc);
        return false;
    }
    g_api->log("SKULPAD", "hooked AndroidAssetPacks.%s(%d) @ %p",
               name, argc, reinterpret_cast<void*>(address));
    return true;
}

static bool hook_managed_method(void* image, const char* namespaze,
                                const char* class_name, const char* name,
                                int argc, uintptr_t hook,
                                uintptr_t* original_out = nullptr)
{
    void* klass = f_class_from_name(image, namespaze, class_name);
    if (!klass) {
        g_api->log("SKULPAD", "class %s.%s not found", namespaze, class_name);
        return false;
    }
    void* method = f_get_method(klass, name, argc);
    if (!method) {
        g_api->log("SKULPAD", "method %s.%s.%s(%d) not found",
                   namespaze, class_name, name, argc);
        return false;
    }
    uintptr_t address = *reinterpret_cast<uintptr_t*>(method);
    uintptr_t original = 0;
    g_api->hook_address_detour(g_mod, address, hook, &original);
    if (!original) {
        g_api->log("SKULPAD", "detour failed for %s.%s.%s(%d)",
                   namespaze, class_name, name, argc);
        return false;
    }
    g_api->log("SKULPAD", "hooked %s.%s.%s(%d) @ %p",
               namespaze, class_name, name, argc,
               reinterpret_cast<void*>(address));
    if (original_out)
        *original_out = original;
    return true;
}

static bool hook_managed_method_by_first_param(void* image,
                                                const char* namespaze,
                                                const char* class_name,
                                                const char* name,
                                                const char* param_class_name,
                                                uintptr_t hook,
                                                uintptr_t* original_out)
{
    void* klass = f_class_from_name(image, namespaze, class_name);
    if (!klass) {
        g_api->log("SKULPAD", "class %s.%s not found", namespaze, class_name);
        return false;
    }

    void* iterator = nullptr;
    void* selected = nullptr;
    while (void* candidate = f_class_get_methods(klass, &iterator)) {
        const char* candidate_name = f_method_get_name(candidate);
        if (!candidate_name || std::strcmp(candidate_name, name) != 0 ||
            f_method_get_param_count(candidate) != 1)
            continue;
        void* param_type = f_method_get_param(candidate, 0);
        void* param_class = param_type ? f_class_from_type(param_type) : nullptr;
        const char* actual_name = param_class ? f_class_get_name(param_class) : nullptr;
        if (actual_name && std::strcmp(actual_name, param_class_name) == 0) {
            selected = candidate;
            break;
        }
    }
    if (!selected) {
        g_api->log("SKULPAD", "method %s.%s.%s(%s) not found",
                   namespaze, class_name, name, param_class_name);
        return false;
    }

    uintptr_t address = *reinterpret_cast<uintptr_t*>(selected);
    uintptr_t original = 0;
    g_api->hook_address_detour(g_mod, address, hook, &original);
    if (!original) {
        g_api->log("SKULPAD", "detour failed for %s.%s.%s(%s)",
                   namespaze, class_name, name, param_class_name);
        return false;
    }
    *original_out = original;
    g_api->log("SKULPAD", "hooked %s.%s.%s(%s) @ %p",
               namespaze, class_name, name, param_class_name,
               reinterpret_cast<void*>(address));
    return true;
}

static void install(void*)
{
    if (!g_enabled)
        return;
    void* image = find_image("UnityEngine.AndroidJNIModule");
    if (!image) {
        g_api->log("SKULPAD", "UnityEngine.AndroidJNIModule image not found");
        return;
    }
    hook_method(image, "get_coreUnityAssetPacksDownloaded", 0,
                reinterpret_cast<uintptr_t>(&hook_downloaded));
    hook_method(image, "GetDataPackName", 0,
                reinterpret_cast<uintptr_t>(&hook_empty_string));
    hook_method(image, "GetStreamingAssetsPackName", 0,
                reinterpret_cast<uintptr_t>(&hook_empty_string));
    hook_method(image, "GetCoreUnityAssetPackNames", 0,
                reinterpret_cast<uintptr_t>(&hook_pack_names));
    hook_method(image, "GetAssetPackPath", 1,
                reinterpret_cast<uintptr_t>(&hook_pack_path));

    // The game's own downloading scene waits on AssetPackDownloader rather
    // than AndroidAssetPacks directly. All six CustomFastFollow bundles are
    // already installed locally, so keep the original initialization/path
    // transform but force the scene-level completion gates to success.
    // Exact match is important: Assembly-CSharp-firstpass is enumerated first
    // in this title but AssetPackDownloader lives in Assembly-CSharp.dll.
    void* game_image = find_image_exact("Assembly-CSharp");
    if (!game_image) {
        g_api->log("SKULPAD", "Assembly-CSharp image not found");
        return;
    }
    hook_managed_method(game_image, "AssetPacks", "AssetPackDownloader",
                        "get_IsReady", 0,
                        reinterpret_cast<uintptr_t>(&hook_downloaded));
    hook_managed_method(game_image, "AssetPacks", "AssetPackDownloader",
                        "get_Percentage", 0,
                        reinterpret_cast<uintptr_t>(&hook_percentage));
    hook_managed_method(game_image, "AssetPacks", "AssetPackDownloader",
                        "StartCustomAssetDownloading", 0,
                        reinterpret_cast<uintptr_t>(&hook_start_custom_downloading),
                        reinterpret_cast<uintptr_t*>(&g_start_custom_downloading));
    hook_managed_method(game_image, "AssetPacks", "AssetPackDownloader",
                        "InitializeBundleToAssetPackMap", 1,
                        reinterpret_cast<uintptr_t>(&hook_initialize_bundle_map),
                        reinterpret_cast<uintptr_t*>(&g_initialize_bundle_map));
    hook_managed_method(game_image, "AssetPacks", "AssetPackDownloader",
                        "AppBundleTransformFunc", 1,
                        reinterpret_cast<uintptr_t>(&hook_app_bundle_transform),
                        reinterpret_cast<uintptr_t*>(&g_app_bundle_transform));
    hook_managed_method(game_image, "AssetPacks", "AssetPackSceneLoader",
                        "CheckAndLoad", 0,
                        reinterpret_cast<uintptr_t>(&hook_check_and_load),
                        reinterpret_cast<uintptr_t*>(&g_check_and_load));

    void* core_image = find_image_exact("UnityEngine.CoreModule");
    if (!core_image) {
        g_api->log("SKULPAD", "UnityEngine.CoreModule image not found");
        return;
    }
    g_scene_manager_class = f_class_from_name(
        core_image, "UnityEngine.SceneManagement", "SceneManager");
    g_allow_load_scene_field = g_scene_manager_class
        ? f_class_get_field_from_name(g_scene_manager_class, "s_AllowLoadScene")
        : nullptr;
    if (!g_scene_manager_class || !g_allow_load_scene_field) {
        g_api->log("SKULPAD", "SceneManager.s_AllowLoadScene field not found");
    } else {
        ensure_scene_load_allowed();
    }
    hook_managed_method_by_first_param(
        core_image, "UnityEngine.SceneManagement", "SceneManager", "LoadScene",
        "String", reinterpret_cast<uintptr_t>(&hook_load_scene_string),
        reinterpret_cast<uintptr_t*>(&g_load_scene_string));
    hook_managed_method(
        core_image, "UnityEngine.SceneManagement", "SceneManager",
        "LoadSceneAsyncNameIndexInternal", 4,
        reinterpret_cast<uintptr_t>(&hook_load_scene_internal),
        reinterpret_cast<uintptr_t*>(&g_load_scene_internal));

    void* async_class = f_class_from_name(core_image, "UnityEngine", "AsyncOperation");
    void* is_done_method = async_class
        ? f_get_method(async_class, "get_isDone", 0) : nullptr;
    void* progress_method = async_class
        ? f_get_method(async_class, "get_progress", 0) : nullptr;
    void* get_allow_method = async_class
        ? f_get_method(async_class, "get_allowSceneActivation", 0) : nullptr;
    void* set_allow_method = async_class
        ? f_get_method(async_class, "set_allowSceneActivation", 1) : nullptr;
    if (!is_done_method || !progress_method || !get_allow_method ||
        !set_allow_method) {
        g_api->log("SKULPAD", "AsyncOperation polling methods not found");
    } else {
        g_async_is_done = reinterpret_cast<orig2_bool_t>(
            *reinterpret_cast<uintptr_t*>(is_done_method));
        g_async_progress = reinterpret_cast<orig2_float_t>(
            *reinterpret_cast<uintptr_t*>(progress_method));
        g_async_get_allow_activation = reinterpret_cast<orig2_bool_t>(
            *reinterpret_cast<uintptr_t*>(get_allow_method));
        g_async_set_allow_activation = reinterpret_cast<orig3_bool_void_t>(
            *reinterpret_cast<uintptr_t*>(set_allow_method));
        g_async_is_done_method = reinterpret_cast<uintptr_t>(is_done_method);
        g_async_progress_method = reinterpret_cast<uintptr_t>(progress_method);
        g_async_get_allow_method = reinterpret_cast<uintptr_t>(get_allow_method);
        g_async_set_allow_method = reinterpret_cast<uintptr_t>(set_allow_method);
        g_api->log("SKULPAD", "AsyncOperation polling armed");
    }
}

static bool enabled()
{
    const char* env = g_api->getenv("BD_SKUL_PAD");
    if (env && *env && std::strcmp(env, "0") != 0)
        return true;
    return g_api->config_get_bool("game_patches.skul_pad.enabled",
           g_api->config_get_bool("skul_pad.enabled", 0)) != 0;
}

static int init(BogoSoModule* lil2cpp)
{
    if (!enabled())
        return BOGO_PLUGIN_OK;

    g_mod = lil2cpp;
    g_enabled = true;
    f_domain_get = reinterpret_cast<p_domain_get>(
        g_api->so_symbol(lil2cpp, "il2cpp_domain_get"));
    f_domain_assemblies = reinterpret_cast<p_domain_get_assemblies>(
        g_api->so_symbol(lil2cpp, "il2cpp_domain_get_assemblies"));
    f_assembly_image = reinterpret_cast<p_assembly_get_image>(
        g_api->so_symbol(lil2cpp, "il2cpp_assembly_get_image"));
    f_image_name = reinterpret_cast<p_image_get_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_image_get_name"));
    f_class_from_name = reinterpret_cast<p_class_from_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_from_name"));
    f_class_get_methods = reinterpret_cast<p_class_get_methods>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_methods"));
    f_class_from_type = reinterpret_cast<p_class_from_type>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_from_type"));
    f_class_get_name = reinterpret_cast<p_class_get_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_name"));
    f_class_get_field_from_name = reinterpret_cast<p_class_get_field_from_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_field_from_name"));
    f_get_method = reinterpret_cast<p_get_method>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_method_from_name"));
    f_method_get_name = reinterpret_cast<p_method_get_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_method_get_name"));
    f_method_get_param_count = reinterpret_cast<p_method_get_param_count>(
        g_api->so_symbol(lil2cpp, "il2cpp_method_get_param_count"));
    f_method_get_param = reinterpret_cast<p_method_get_param>(
        g_api->so_symbol(lil2cpp, "il2cpp_method_get_param"));
    f_field_static_get_value = reinterpret_cast<p_field_static_get_value>(
        g_api->so_symbol(lil2cpp, "il2cpp_field_static_get_value"));
    f_field_static_set_value = reinterpret_cast<p_field_static_set_value>(
        g_api->so_symbol(lil2cpp, "il2cpp_field_static_set_value"));
    f_runtime_class_init = reinterpret_cast<p_runtime_class_init>(
        g_api->so_symbol(lil2cpp, "il2cpp_runtime_class_init"));
    f_gchandle_new = reinterpret_cast<p_gchandle_new>(
        g_api->so_symbol(lil2cpp, "il2cpp_gchandle_new"));
    f_gchandle_free = reinterpret_cast<p_gchandle_free>(
        g_api->so_symbol(lil2cpp, "il2cpp_gchandle_free"));
    f_string_new = reinterpret_cast<p_string_new>(
        g_api->so_symbol(lil2cpp, "il2cpp_string_new_wrapper"));
    if (!f_string_new)
        f_string_new = reinterpret_cast<p_string_new>(
            g_api->so_symbol(lil2cpp, "il2cpp_string_new"));
    f_string_length = reinterpret_cast<p_string_length>(
        g_api->so_symbol(lil2cpp, "il2cpp_string_length"));
    f_string_chars = reinterpret_cast<p_string_chars>(
        g_api->so_symbol(lil2cpp, "il2cpp_string_chars"));
    f_array_class_get = reinterpret_cast<p_array_class_get>(
        g_api->so_symbol(lil2cpp, "il2cpp_array_class_get"));
    f_array_new = reinterpret_cast<p_array_new>(
        g_api->so_symbol(lil2cpp, "il2cpp_array_new"));

    if (!f_domain_get || !f_domain_assemblies || !f_assembly_image ||
        !f_image_name || !f_class_from_name || !f_get_method ||
        !f_class_get_methods || !f_class_from_type || !f_class_get_name ||
        !f_class_get_field_from_name || !f_field_static_get_value ||
        !f_field_static_set_value || !f_runtime_class_init ||
        !f_gchandle_new || !f_gchandle_free ||
        !f_method_get_name || !f_method_get_param_count || !f_method_get_param ||
        !f_string_new || !f_string_length || !f_string_chars ||
        !f_array_class_get || !f_array_new) {
        g_api->log("SKULPAD", "required IL2CPP exports missing; disabled");
        g_enabled = false;
        return BOGO_PLUGIN_OK;
    }

    g_asset_root = g_api->config_get_string("paths.android_files", "");
    if (g_asset_root.empty())
        g_asset_root = g_api->config_get_string("paths.game_files", "");
    g_asset_root = std::filesystem::absolute(g_asset_root).lexically_normal().string();
    if (!g_api->register_il2cpp_post_init(&install, nullptr)) {
        g_api->log("SKULPAD", "failed to register post-init callback");
        g_enabled = false;
        return BOGO_PLUGIN_OK;
    }
    g_api->log("SKULPAD", "armed: asset_root=%s", g_asset_root.c_str());
    return BOGO_PLUGIN_OK;
}

} // namespace skul_pad

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(BogoPluginApi))
        return -1;
    skul_pad::g_api_storage = *api;
    skul_pad::g_api = &skul_pad::g_api_storage;
    if (!api->il2cpp)
        return BOGO_PLUGIN_DEFERRED;
    return skul_pad::init(api->il2cpp);
}
