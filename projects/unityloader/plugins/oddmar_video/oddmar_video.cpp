#include "plugin_api.h"
#include "logging.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace oddmar_video {

static const BogoPluginApi* g_api = nullptr;
static uintptr_t g_orig = 0;
static std::string g_root;
static std::string g_fallback;
static std::string g_last_source;
static std::map<std::string, std::string> g_url_cache;
static std::map<std::string, long long> g_size_cache;

static long long size_of(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return -1;
    const std::streamoff size = file.tellg();
    return size > 0 ? static_cast<long long>(size) : -1;
}

static void init_paths()
{
    if (!g_root.empty()) return;
    std::filesystem::path config = g_api->config_path ? g_api->config_path : "";
    // Test runners may copy the TOML to /tmp before launching the loader. The
    // loader's working directory remains the actual game root, so prefer it
    // whenever it contains gamedata and only then fall back to the TOML path.
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path cwd_data = cwd / "gamedata";
    const std::filesystem::path config_data = config.parent_path() / "gamedata";
    if (std::filesystem::exists(cwd / "assets"))
        g_root = cwd.lexically_normal().string();
    else if (std::filesystem::exists(cwd_data / "assets"))
        g_root = cwd_data.lexically_normal().string();
    else
        g_root = config_data.lexically_normal().string();
    g_fallback = g_root + "/assets/Videos/mobge_and_senri_splash_video.mp4";
}

static void note_source(const char* path, void*)
{
    if (!path || !*path) return;
    const std::string value(path);
    if (value.size() < 4) return;
    const std::string ext = value.substr(value.size() - 4);
    if (ext != ".m4v" && ext != ".mp4") return;
    init_paths();
    std::string absolute = value;
    if (absolute.front() != '/') absolute = g_root + "/" + absolute;
    if (size_of(absolute) > 0) {
        g_last_source = absolute;
        g_api->log("MEDIA", "oddmar_video source=%s", absolute.c_str());
    }
}

static bool relative_path(const char* url, std::string& out)
{
    if (!url) return false;
    std::string value(url);
    if (value.size() > 1024) return false;
    const size_t query = value.find_first_of("?#");
    if (query != std::string::npos) value.resize(query);
    const size_t assets = value.find("assets/");
    if (assets == std::string::npos) return false;
    out = value.substr(assets + 7);
    return !out.empty() && out.find('/') != std::string::npos;
}

static uintptr_t translate(uintptr_t, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                           uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    uintptr_t url_ptr = 0;
#if defined(__aarch64__)
    __asm__ volatile("mov %0, x23" : "=r"(url_ptr));
#endif
    const char* url = reinterpret_cast<const char*>(url_ptr);
    init_paths();
    const std::string key = url ? std::string(url).substr(0, 512) : std::string();
    const bool temporary = url && std::strstr(url, "temporary_video.mp4");
    if (temporary && !g_last_source.empty() && size_of(g_last_source) > 0)
        g_url_cache[key] = g_last_source;

    auto inserted = g_url_cache.emplace(key, std::string());
    std::string& path = inserted.first->second;
    if (inserted.second && !temporary) {
        std::string relative;
        if (relative_path(url, relative)) {
            const std::string candidate = g_root + "/assets/" + relative;
            if (size_of(candidate) > 0) {
                path = candidate;
                g_last_source = path;
            }
        }
    }
    bool fallback = false;
    if (path.empty()) {
        path = g_fallback;
        fallback = true;
    }
    auto size = g_size_cache.find(path);
    if (size == g_size_cache.end())
        size = g_size_cache.emplace(path, size_of(path) > 0 ? size_of(path) : 0).first;

    if (a1) *reinterpret_cast<const char**>(a1) = path.c_str();
    if (a2) *reinterpret_cast<void**>(a2) = nullptr;
    if (a3) *reinterpret_cast<size_t*>(a3) = static_cast<size_t>(size->second);
    g_api->log("MEDIA", "oddmar_video xlat=%s url=%s path=%s size=%lld",
               fallback ? "fallback" : "ok", url ? url : "(null)",
               path.c_str(), size->second);
    return 1;
}

static void on_unity(const char*, BogoSoModule* module, void*)
{
    if (!g_api || !module || g_orig) return;
    const uintptr_t base = g_api->so_base(module);
    if (!base) return;
    g_api->hook_address_detour(module, base + 0x4c124c,
                               reinterpret_cast<uintptr_t>(&translate), &g_orig);
    g_api->log("ODDMAR_VIDEO", "installed rva=0x4c124c orig=%p",
               reinterpret_cast<void*>(g_orig));
}

static int init(const BogoPluginApi* api)
{
    const char* package = api->config_get_string("package.packageName", "");
    if (!package || std::strcmp(package, "com.mobge.Oddmar") != 0)
        return BOGO_PLUGIN_OK;
    if (!api->getenv || !api->getenv("BD_BYPASS_VIDEO_TRANSLATE")) {
        api->log("ODDMAR_VIDEO", "disabled: BD_BYPASS_VIDEO_TRANSLATE is unset");
        return BOGO_PLUGIN_OK;
    }
    g_api = api;
    api->register_asset_source_callback(note_source, nullptr);
    if (!api->register_module_loaded("libunity.so", on_unity, nullptr))
        return -1;
    return BOGO_PLUGIN_OK;
}

} // namespace oddmar_video

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(BogoPluginApi) || !api->log ||
        !api->register_module_loaded || !api->register_asset_source_callback)
        return -1;
    oddmar_video::g_api = api;
    return oddmar_video::init(api);
}
