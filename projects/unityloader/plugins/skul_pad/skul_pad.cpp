#include "plugin_api.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace skul_pad {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static BogoSoModule* g_mod = nullptr;
static bool g_enabled = false;
static std::string g_asset_root;

static long read_status_kb(const char* key)
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    const size_t key_len = std::strlen(key);
    long value = -1;
    while (fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            if (std::sscanf(line + key_len + 1, "%ld", &value) == 1)
                break;
        }
    }
    fclose(f);
    return value;
}

static long read_meminfo_kb(const char* key)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[256];
    const size_t key_len = std::strlen(key);
    long value = -1;
    while (fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            if (std::sscanf(line + key_len + 1, "%ld", &value) == 1)
                break;
        }
    }
    fclose(f);
    return value;
}

// Scene transitions (esp. gameBase) are the OOM suspects — always sample RSS.
static void log_process_memory(const char* why)
{
    if (!g_api || !g_api->log)
        return;
    const long rss = read_status_kb("VmRSS");
    const long hwm = read_status_kb("VmHWM");
    const long size = read_status_kb("VmSize");
    const long avail = read_meminfo_kb("MemAvailable");
    const long total = read_meminfo_kb("MemTotal");
    g_api->log("MEM", "pid=%d rss=%.1fMB hwm=%.1fMB vsz=%.1fMB sys_avail=%.1fMB/%ldMB @ %s",
               (int)getpid(),
               rss >= 0 ? rss / 1024.0 : -1.0,
               hwm >= 0 ? hwm / 1024.0 : -1.0,
               size >= 0 ? size / 1024.0 : -1.0,
               avail >= 0 ? avail / 1024.0 : -1.0,
               total >= 0 ? total / 1024 : -1L,
               why ? why : "?");
}

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

static std::string data_pack_assets_path()
{
    // Must contain ".apk/" so Unity MountDataArchive uses ZipCentralDirectory.
    return (std::filesystem::current_path() / "UnityDataAssetPack.apk" / "assets")
        .lexically_normal()
        .string();
}

static std::string streaming_assets_path()
{
    return (std::filesystem::current_path() / "assets").lexically_normal().string();
}

static std::string custom_pack_root()
{
    std::filesystem::path root = g_asset_root.empty()
        ? std::filesystem::path(g_api->config_get_string("paths.android_files", "../conf"))
        : std::filesystem::path(g_asset_root);
    if (root.is_relative())
        root = std::filesystem::current_path() / root;
    // Play Asset Delivery assetsPath points at the pack's assets/ directory.
    // Addressables then appends "<hash>.bundle", so this must include assets/.
    return (root.lexically_normal() / "assetpacks" / "CustomFastFollow" /
            "48" / "48" / "assets").string();
}

static std::string strip_file_uri(std::string path)
{
    if (path.rfind("file:///", 0) == 0)
        return path.substr(7);
    if (path.rfind("file://", 0) == 0)
        return path.substr(6);
    return path;
}

static std::string fix_custom_fast_follow_bundle_path(std::string path)
{
    path = strip_file_uri(std::move(path));
    if (path.empty())
        return path;
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return path;

    // Older transform output omitted the assets/ segment:
    //   .../CustomFastFollow/48/48/<hash>.bundle
    // Real files live at:
    //   .../CustomFastFollow/48/48/assets/<hash>.bundle
    const std::string marker = "/CustomFastFollow/48/48/";
    const auto pos = path.find(marker);
    if (pos == std::string::npos)
        return path;
    const auto after = pos + marker.size();
    if (after >= path.size())
        return path;
    if (path.compare(after, 7, "assets/") == 0)
        return path;
    std::string candidate = path.substr(0, after) + "assets/" + path.substr(after);
    if (std::filesystem::exists(candidate, ec))
        return candidate;
    return path;
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
static bool g_platform_loader_done = false;
static uint32_t g_post_load_frames = 0;
using scene_count_fn = int32_t (*)(uintptr_t);
using scene_active_fn = void (*)(void*, uintptr_t);
using scene_at_fn = void (*)(int32_t, void*, uintptr_t);
using scene_name_fn = void* (*)(void*, uintptr_t);
static scene_count_fn g_scene_count = nullptr;
static scene_active_fn g_scene_active_injected = nullptr;
static scene_at_fn g_scene_at_injected = nullptr;
static scene_name_fn g_scene_get_name = nullptr;
static uintptr_t g_scene_count_method = 0;
static uintptr_t g_scene_active_method = 0;
static uintptr_t g_scene_at_method = 0;
static uintptr_t g_scene_name_method = 0;
static std::string g_last_logged_scenes;

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
        g_api->log("SKULPAD", "PlatformLoader AsyncOperation completed");
        g_platform_loader_done = true;
        g_post_load_frames = 0;
        // Stop the Skul-only CPU present path once bootstrap finished so the
        // title scene can use normal swaps and avoid an extra fullscreen copy.
        setenv("BD_EGL_CPU_PRESENT", "0", 1);
        setenv("BD_EGL_SWAP_PAUSE", "0", 1);
        f_gchandle_free(g_scene_operation_handle);
        g_scene_operation_handle = 0;
        g_scene_operation = 0;
    }
}

static void log_loaded_scenes(const char* source)
{
    if (!g_scene_count || !g_scene_get_name)
        return;
    if (!g_scene_active_injected && !g_scene_at_injected)
        return;
    const int32_t count = g_scene_count(g_scene_count_method);
    std::string summary = "count=" + std::to_string(count);
    if (g_scene_active_injected) {
        alignas(8) uint8_t active_storage[16] = {};
        g_scene_active_injected(active_storage, g_scene_active_method);
        void* name_obj = g_scene_get_name(active_storage, g_scene_name_method);
        summary += " active='";
        summary += managed_string(name_obj);
        summary += "'";
    }
    if (g_scene_at_injected) {
        for (int32_t i = 0; i < count && i < 8; ++i) {
            alignas(8) uint8_t scene_storage[16] = {};
            g_scene_at_injected(i, scene_storage, g_scene_at_method);
            void* name_obj = g_scene_get_name(scene_storage, g_scene_name_method);
            summary += " [";
            summary += std::to_string(i);
            summary += "]='";
            summary += managed_string(name_obj);
            summary += "'";
        }
    }
    if (summary == g_last_logged_scenes)
        return;
    g_last_logged_scenes = summary;
    g_api->log("SKULPAD", "scenes source=%s %s", source ? source : "?",
               summary.c_str());
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
    log_process_memory(("LoadScene:" + scene).c_str());
    if (g_load_scene_string)
        g_load_scene_string(scene_name, method);
    g_api->log("SKULPAD", "SceneManager.LoadScene(string) returned scene='%s'", scene.c_str());
    log_process_memory(("LoadSceneDone:" + scene).c_str());
}

static uintptr_t hook_load_scene_internal(uintptr_t scene_name,
                                          int32_t scene_build_index,
                                          uintptr_t parameters,
                                          uint8_t must_complete_next_frame,
                                          uintptr_t method)
{
    ensure_scene_load_allowed();
    const std::string scene = managed_string(reinterpret_cast<void*>(scene_name));
    // Preserve the game's synchronous next-frame scene commit for most scenes.
    // PlatformLoader itself historically wedges on mustCompleteNextFrame=1 while
    // the async path reaches progress 0.9 and can be finished by allowSceneActivation
    // pulsing from the EGL present poll. Keep Title-direct as an opt-in failure.
    const bool force_async = scene == "PlatformLoader" &&
        g_api->config_get_bool(
            "game_patches.skul_pad.force_async_platform_loader", 1);
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
    log_process_memory(("LoadSceneAsync:" + effective_scene).c_str());
    return operation;
}

static uintptr_t hook_app_bundle_transform(uintptr_t downloader,
                                           uintptr_t location,
                                           uintptr_t method)
{
    uintptr_t transformed = g_app_bundle_transform
        ? g_app_bundle_transform(downloader, location, method) : 0;
    ++g_transform_calls;
    std::string path = managed_string(reinterpret_cast<void*>(transformed));
    const std::string fixed = fix_custom_fast_follow_bundle_path(path);
    if (fixed != path && !fixed.empty()) {
        if (g_transform_calls <= 24) {
            g_api->log("SKULPAD", "AppBundleTransformFunc rewrite '%s' -> '%s'",
                       path.c_str(), fixed.c_str());
        }
        path = fixed;
        transformed = reinterpret_cast<uintptr_t>(f_string_new(path.c_str()));
    }
    if (g_transform_calls <= 24) {
        std::error_code ec;
        const std::string check = strip_file_uri(path);
        const bool exists = !check.empty() && std::filesystem::exists(check, ec);
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
    if (name == "UnityDataAssetPack") {
        path = data_pack_assets_path();
        if (!g_logged_core_pack_files) {
            const std::filesystem::path apk =
                std::filesystem::current_path() / "UnityDataAssetPack.apk";
            std::error_code ec;
            const bool apk_exists = std::filesystem::exists(apk, ec);
            const bool loose_datapack = std::filesystem::exists(
                std::filesystem::current_path() / "assets" / "bin" / "Data" /
                    "datapack.unity3d",
                ec);
            g_api->log("SKULPAD",
                       "data pack assets='%s' apkExists=%d looseDatapack=%d",
                       path.c_str(), apk_exists ? 1 : 0,
                       loose_datapack ? 1 : 0);
            g_logged_core_pack_files = true;
        }
    } else if (name == "UnityStreamingAssetsPack") {
        path = streaming_assets_path();
        std::error_code ec;
        const bool json_exists = std::filesystem::exists(
            std::filesystem::path(path) / "CustomAssetPacksData.json", ec);
        g_api->log("SKULPAD", "streaming pack root='%s' CustomAssetPacksData.json=%d",
                   path.c_str(), json_exists ? 1 : 0);
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

    void* scene_class = f_class_from_name(
        core_image, "UnityEngine.SceneManagement", "Scene");
    void* scene_count_method = g_scene_manager_class
        ? f_get_method(g_scene_manager_class, "get_sceneCount", 0) : nullptr;
    void* scene_active_method = g_scene_manager_class
        ? f_get_method(g_scene_manager_class, "GetActiveScene_Injected", 1) : nullptr;
    void* scene_at_method = g_scene_manager_class
        ? f_get_method(g_scene_manager_class, "GetSceneAt_Injected", 2) : nullptr;
    void* scene_name_method = scene_class
        ? f_get_method(scene_class, "get_name", 0) : nullptr;
    if (scene_count_method && scene_name_method &&
        (scene_active_method || scene_at_method)) {
        g_scene_count = reinterpret_cast<scene_count_fn>(
            *reinterpret_cast<uintptr_t*>(scene_count_method));
        g_scene_count_method = reinterpret_cast<uintptr_t>(scene_count_method);
        g_scene_get_name = reinterpret_cast<scene_name_fn>(
            *reinterpret_cast<uintptr_t*>(scene_name_method));
        g_scene_name_method = reinterpret_cast<uintptr_t>(scene_name_method);
        if (scene_active_method) {
            g_scene_active_injected = reinterpret_cast<scene_active_fn>(
                *reinterpret_cast<uintptr_t*>(scene_active_method));
            g_scene_active_method = reinterpret_cast<uintptr_t>(scene_active_method);
        }
        if (scene_at_method) {
            g_scene_at_injected = reinterpret_cast<scene_at_fn>(
                *reinterpret_cast<uintptr_t*>(scene_at_method));
            g_scene_at_method = reinterpret_cast<uintptr_t>(scene_at_method);
        }
        g_api->log("SKULPAD", "SceneManager scene diagnostics armed");
    } else {
        g_api->log("SKULPAD", "SceneManager scene diagnostics unavailable count=%p active=%p at=%p name=%p",
                   scene_count_method, scene_active_method, scene_at_method,
                   scene_name_method);
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

static void on_present(void* userdata);

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
    if (g_api->register_present_callback) {
        g_api->register_present_callback(&on_present, nullptr);
        g_api->log("SKULPAD", "registered present callback");
    }
    return BOGO_PLUGIN_OK;
}

static void on_present(void* /*userdata*/)
{
    if (!g_enabled)
        return;
    poll_scene_operation("egl");
    if (g_platform_loader_done) {
        ++g_post_load_frames;
        if (g_post_load_frames <= 3 ||
            g_post_load_frames == 30 ||
            g_post_load_frames == 120 ||
            (g_post_load_frames % 300) == 0) {
            log_loaded_scenes("egl");
        }
    }
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
