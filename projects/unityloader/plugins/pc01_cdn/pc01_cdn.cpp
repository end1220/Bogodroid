#include "plugin_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// PC01 (Storytaco) BundleManager refuses to load when m_URL is empty
// ("[ BundleManager]::Initialization ] Empty URL"). Release builds often strip
// that Debug.Log, so the game just sits on a black screen. Inject the CloudFront
// base URL before Initialization runs.

namespace {

BogoPluginApi g_api_storage = {};
const BogoPluginApi* g_api = &g_api_storage;
BogoSoModule* g_mod = nullptr;
bool g_enabled = false;
bool g_hooked = false;
std::string g_base_url = "https://d2xfbt8p71ssjd.cloudfront.net";

using p_domain_get = void* (*)();
using p_domain_get_assemblies = void** (*)(void*, size_t*);
using p_assembly_get_image = void* (*)(void*);
using p_image_get_name = const char* (*)(void*);
using p_class_from_name = void* (*)(void*, const char*, const char*);
using p_class_get_parent = void* (*)(void*);
using p_class_get_name = const char* (*)(void*);
using p_get_method = void* (*)(void*, const char*, int);
using p_class_get_methods = void* (*)(void*, void**);
using p_method_get_name = const char* (*)(void*);
using p_method_get_param_count = uint32_t (*)(void*);
using p_class_get_field_from_name = void* (*)(void*, const char*);
using p_field_get_offset = size_t (*)(void*);
using p_string_new = void* (*)(const char*);
using p_string_length = int32_t (*)(void*);
using p_gc_wbarrier_set_field = void (*)(void*, void**, void*);

// ARM64: forward enough slots that MethodInfo / extra args stay in place.
using ForwardFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4);

p_domain_get f_domain_get = nullptr;
p_domain_get_assemblies f_domain_assemblies = nullptr;
p_assembly_get_image f_assembly_image = nullptr;
p_image_get_name f_image_name = nullptr;
p_class_from_name f_class_from_name = nullptr;
p_class_get_parent f_class_get_parent = nullptr;
p_class_get_name f_class_get_name = nullptr;
p_get_method f_get_method = nullptr;
p_class_get_methods f_class_get_methods = nullptr;
p_method_get_name f_method_get_name = nullptr;
p_method_get_param_count f_method_get_param_count = nullptr;
p_class_get_field_from_name f_class_get_field_from_name = nullptr;
p_field_get_offset f_field_get_offset = nullptr;
p_string_new f_string_new = nullptr;
p_string_length f_string_length = nullptr;
p_gc_wbarrier_set_field f_gc_wbarrier_set_field = nullptr;

ForwardFn g_orig_initialization = nullptr;
void* g_url_field = nullptr;
size_t g_url_offset = 0;

// Skip-hooks for optional SDKs (Firebase / AppLovin). Multiple methods share
// one no-op trampoline; we do not call through.
using SkipFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4);
int g_sdk_skip_hooks = 0;
// Direct pointer to Initialization.StartGame (NOT a detour). UniTask /
// struct-returning methods must not be wrapped in void ForwardFn hooks —
// that corrupts the return slot and SEGV after the callee returns.
ForwardFn g_start_game = nullptr;

using p_image_get_class_count = size_t (*)(const void*);
using p_image_get_class = void* (*)(const void*, size_t);
p_image_get_class_count f_image_get_class_count = nullptr;
p_image_get_class f_image_get_class = nullptr;

void* find_image_exact(const char* image_name)
{
    void* domain = f_domain_get();
    if (!domain)
        return nullptr;
    size_t count = 0;
    void** assemblies = f_domain_assemblies(domain, &count);
    if (!assemblies)
        return nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = f_assembly_image(assemblies[i]);
        if (!image)
            continue;
        const char* name = f_image_name(image);
        if (!name)
            continue;
        if (std::strcmp(name, image_name) == 0)
            return image;
        std::string n(name);
        if (n == std::string(image_name) + ".dll")
            return image;
    }
    return nullptr;
}

void* find_bundle_manager_class(void* image)
{
    if (void* k = f_class_from_name(image, "Storytaco", "BundleManager"))
        return k;
    if (void* k = f_class_from_name(image, "", "BundleManager"))
        return k;
    return nullptr;
}

bool ensure_url_field(void* klass)
{
    if (g_url_field)
        return true;
    // Field may live on BundleManager or a parent initializer base.
    for (void* k = klass; k; k = f_class_get_parent ? f_class_get_parent(k) : nullptr) {
        g_url_field = f_class_get_field_from_name(k, "m_URL");
        if (g_url_field)
            break;
    }
    if (!g_url_field) {
        g_api->log("PC01CDN", "BundleManager.m_URL field not found");
        return false;
    }
    g_url_offset = f_field_get_offset(g_url_field);
    g_api->log("PC01CDN", "BundleManager.m_URL offset=%zu", g_url_offset);
    return true;
}

void inject_url(void* self)
{
    if (!self || !g_url_field || !f_string_new)
        return;

    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + g_url_offset);
    void* current = *slot;
    if (current && f_string_length && f_string_length(current) > 0) {
        g_api->log("PC01CDN", "m_URL already set (len=%d), leave alone",
                   f_string_length(current));
        return;
    }

    void* str = f_string_new(g_base_url.c_str());
    if (!str) {
        g_api->log("PC01CDN", "il2cpp_string_new failed");
        return;
    }
    if (f_gc_wbarrier_set_field)
        f_gc_wbarrier_set_field(self, slot, str);
    else
        *slot = str;
    g_api->log("PC01CDN", "injected m_URL=%s", g_base_url.c_str());
}

void hooked_initialization(void* self, void* a1, void* a2, void* a3, void* a4)
{
    g_api->log("PC01CDN", "BundleManager.Initialization ENTER self=%p a1=%p a2=%p a3=%p",
               self, a1, a2, a3);
    // argc=3: often (url/setting,...) — if a1 is an empty Il2CppString, still
    // force m_URL so Empty URL early-out cannot win on the field path.
    inject_url(self);
    if (g_orig_initialization)
        g_orig_initialization(self, a1, a2, a3, a4);
}

void log_methods(void* klass)
{
    if (!f_class_get_methods || !f_method_get_name || !f_method_get_param_count)
        return;
    for (void* k = klass; k; k = f_class_get_parent ? f_class_get_parent(k) : nullptr) {
        const char* cname = f_class_get_name ? f_class_get_name(k) : "?";
        void* it = nullptr;
        int n = 0;
        while (void* m = f_class_get_methods(k, &it)) {
            const char* name = f_method_get_name(m);
            uint32_t argc = f_method_get_param_count(m);
            if (name && (std::strstr(name, "Init") || std::strcmp(name, "Awake") == 0 ||
                         std::strcmp(name, "Start") == 0 || std::strstr(name, "Bundle") ||
                         std::strstr(name, "URL") || std::strstr(name, "Load"))) {
                g_api->log("PC01CDN", "method %s.%s argc=%u ptr=%p",
                           cname ? cname : "?", name, argc,
                           reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(m)));
            }
            if (++n > 256)
                break;
        }
    }
}

void* find_named_method(void* klass, const char* want_name, uint32_t* argc_out)
{
    // Prefer exact get_method_from_name across the inheritance chain.
    for (void* k = klass; k; k = f_class_get_parent ? f_class_get_parent(k) : nullptr) {
        for (int argc = 0; argc <= 8; ++argc) {
            if (void* m = f_get_method(k, want_name, argc)) {
                if (argc_out)
                    *argc_out = static_cast<uint32_t>(argc);
                return m;
            }
        }
    }
    // Enumerate as fallback (some IL2CPP builds mismatch argc).
    if (!f_class_get_methods || !f_method_get_name || !f_method_get_param_count)
        return nullptr;
    for (void* k = klass; k; k = f_class_get_parent ? f_class_get_parent(k) : nullptr) {
        void* it = nullptr;
        while (void* m = f_class_get_methods(k, &it)) {
            const char* name = f_method_get_name(m);
            if (!name || std::strcmp(name, want_name) != 0)
                continue;
            if (argc_out)
                *argc_out = f_method_get_param_count(m);
            return m;
        }
    }
    return nullptr;
}

bool hook_method_ptr(void* method, const char* label, uint32_t argc)
{
    if (!method)
        return false;
    uintptr_t address = *reinterpret_cast<uintptr_t*>(method);
    if (!address) {
        g_api->log("PC01CDN", "%s has null method pointer", label);
        return false;
    }
    uintptr_t original = 0;
    g_api->hook_address_detour(g_mod, address,
                               reinterpret_cast<uintptr_t>(&hooked_initialization),
                               &original);
    if (!original) {
        g_api->log("PC01CDN", "detour %s failed", label);
        return false;
    }
    g_orig_initialization = reinterpret_cast<ForwardFn>(original);
    g_api->log("PC01CDN", "hooked %s argc=%u @ %p", label, argc,
               reinterpret_cast<void*>(address));
    return true;
}

void hooked_sdk_skip(void* self, void* a1, void* a2, void* a3, void* a4)
{
    (void)self;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    // Intentionally empty — optional analytics/ads/auth must not block Boot.
}

void hooked_init_firebase_to_start(void* self, void* a1, void* a2, void* a3, void* a4)
{
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    // Boot: InitDefaultManager → InitAfterManager → InitFirebase → StartGame
    // → InitStartGame (async, called from StartGame). Do NOT detour or
    // manually invoke InitStartGame: void ForwardFn hooks smash UniTask
    // returns (crash: StartGame ENTER → InitStartGame LEAVE → SEGV, no splash).
    // StartGame(bool isReStart): pass false (0), not InitFirebase's junk regs.
    g_api->log("PC01CDN",
               "InitFirebase BYPASS -> StartGame(isReStart=false) self=%p fn=%p",
               self, reinterpret_cast<void*>(g_start_game));
    if (g_start_game) {
        g_api->log("PC01CDN", "StartGame ENTER self=%p", self);
        g_start_game(self, nullptr, nullptr, nullptr, nullptr);
        g_api->log("PC01CDN", "StartGame LEAVE self=%p", self);
    } else {
        g_api->log("PC01CDN", "StartGame missing — cannot continue Boot");
    }
}

bool hook_method_skip(void* method, const char* label, uint32_t argc)
{
    if (!method)
        return false;
    uintptr_t address = *reinterpret_cast<uintptr_t*>(method);
    if (!address)
        return false;
    uintptr_t original = 0;
    g_api->hook_address_detour(g_mod, address,
                               reinterpret_cast<uintptr_t>(&hooked_sdk_skip),
                               &original);
    if (!original) {
        g_api->log("PC01CDN", "skip-detour %s failed", label);
        return false;
    }
    ++g_sdk_skip_hooks;
    g_api->log("PC01CDN", "skip %s argc=%u @ %p", label, argc,
               reinterpret_cast<void*>(address));
    return true;
}

bool hook_named_on_class(void* klass, const char* method_name)
{
    if (!klass)
        return false;
    uint32_t argc = 0;
    void* method = find_named_method(klass, method_name, &argc);
    if (!method)
        return false;
    char label[96];
    const char* cname = f_class_get_name ? f_class_get_name(klass) : "?";
    std::snprintf(label, sizeof(label), "%s.%s", cname ? cname : "?", method_name);
    return hook_method_skip(method, label, argc);
}

void skip_optional_sdks(void* image)
{
    if (void* fm = f_class_from_name(image, "Storytaco", "FirebaseManager")) {
        log_methods(fm);
        static const char* kFirebaseSkip[] = {
            "Initialization",
            "SetAnalyticsConsent",
            "SetPushable",
            "SignInGuest",
            "SignInWithGoogle",
            "SignInWithPlayGames",
            "SignInWithFacebook",
            "SignInWithGameCenter",
        };
        for (const char* name : kFirebaseSkip)
            hook_named_on_class(fm, name);
    }

    if (void* am = f_class_from_name(image, "Storytaco", "AppLovinManager")) {
        log_methods(am);
        static const char* kMaxSkip[] = {
            "Initialization",
            "Initialize",
            "Init",
        };
        for (const char* name : kMaxSkip)
            hook_named_on_class(am, name);
    }

    // Title class "Initialization": bypass InitFirebase → call StartGame only.
    // InitStartGame is async (argc=1, <InitStartGame>d__8); StartGame invokes it.
    if (f_image_get_class_count && f_image_get_class) {
        const size_t n = f_image_get_class_count(image);
        for (size_t i = 0; i < n; ++i) {
            void* klass = f_image_get_class(image, i);
            if (!klass)
                continue;
            uint32_t argc_fb = 0;
            void* m_fb = find_named_method(klass, "InitFirebase", &argc_fb);
            if (!m_fb)
                continue;
            const char* cname = f_class_get_name ? f_class_get_name(klass) : "?";
            log_methods(klass);

            uint32_t argc_sg = 0, argc_isg = 0;
            void* m_sg = find_named_method(klass, "StartGame", &argc_sg);
            void* m_isg = find_named_method(klass, "InitStartGame", &argc_isg);
            g_start_game = nullptr;
            if (m_sg) {
                uintptr_t sg = *reinterpret_cast<uintptr_t*>(m_sg);
                if (sg)
                    g_start_game = reinterpret_cast<ForwardFn>(sg);
            }
            g_api->log("PC01CDN",
                       "%s boot: StartGame argc=%u ptr=%p InitStartGame argc=%u "
                       "ptr=%p (no detour on either — UniTask-safe)",
                       cname ? cname : "?", argc_sg,
                       reinterpret_cast<void*>(g_start_game), argc_isg,
                       m_isg ? reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(m_isg))
                             : nullptr);

            uintptr_t fb = *reinterpret_cast<uintptr_t*>(m_fb);
            if (!fb)
                continue;
            uintptr_t original = 0;
            g_api->hook_address_detour(
                g_mod, fb,
                reinterpret_cast<uintptr_t>(&hooked_init_firebase_to_start),
                &original);
            if (original) {
                ++g_sdk_skip_hooks;
                g_api->log("PC01CDN",
                           "bypass %s.InitFirebase argc=%u -> StartGame=%p",
                           cname ? cname : "?", argc_fb,
                           reinterpret_cast<void*>(g_start_game));
            } else {
                g_api->log("PC01CDN", "bypass %s.InitFirebase detour failed",
                           cname ? cname : "?");
            }
        }
    }

    g_api->log("PC01CDN", "sdk-skip hooks installed: %d", g_sdk_skip_hooks);
}

bool hook_initialization(void* image)
{
    void* klass = find_bundle_manager_class(image);
    if (!klass) {
        g_api->log("PC01CDN", "BundleManager class not found");
        return false;
    }
    if (!ensure_url_field(klass))
        return false;

    log_methods(klass);

    static const char* kCandidates[] = {
        "Initialization",
        "LoadDefaultBundles",
        "Awake",
        "Start",
        "Init",
    };
    for (const char* name : kCandidates) {
        uint32_t argc = 0;
        void* method = find_named_method(klass, name, &argc);
        if (!method)
            continue;
        char label[96];
        std::snprintf(label, sizeof(label), "BundleManager.%s", name);
        if (hook_method_ptr(method, label, argc))
            return true;
    }

    g_api->log("PC01CDN", "no hookable Initialization/Awake/Start on BundleManager");
    return false;
}

void install(void*)
{
    if (!g_enabled || g_hooked)
        return;
    void* image = find_image_exact("Assembly-CSharp");
    if (!image) {
        g_api->log("PC01CDN", "Assembly-CSharp not ready yet");
        return;
    }
    skip_optional_sdks(image);
    if (hook_initialization(image))
        g_hooked = true;
}

bool enabled()
{
    return g_api->config_get_bool("game_patches.pc01_cdn.enabled", 1) != 0;
}

int init_il2cpp(BogoSoModule* lil2cpp)
{
    if (!enabled())
        return BOGO_PLUGIN_OK;

    g_mod = lil2cpp;
    g_enabled = true;
    const char* url = g_api->config_get_string("game_patches.pc01_cdn.base_url", nullptr);
    if (url && url[0])
        g_base_url = url;

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
    f_class_get_parent = reinterpret_cast<p_class_get_parent>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_parent"));
    f_class_get_name = reinterpret_cast<p_class_get_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_name"));
    f_get_method = reinterpret_cast<p_get_method>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_method_from_name"));
    f_class_get_methods = reinterpret_cast<p_class_get_methods>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_methods"));
    f_method_get_name = reinterpret_cast<p_method_get_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_method_get_name"));
    f_method_get_param_count = reinterpret_cast<p_method_get_param_count>(
        g_api->so_symbol(lil2cpp, "il2cpp_method_get_param_count"));
    f_class_get_field_from_name = reinterpret_cast<p_class_get_field_from_name>(
        g_api->so_symbol(lil2cpp, "il2cpp_class_get_field_from_name"));
    f_field_get_offset = reinterpret_cast<p_field_get_offset>(
        g_api->so_symbol(lil2cpp, "il2cpp_field_get_offset"));
    f_string_new = reinterpret_cast<p_string_new>(
        g_api->so_symbol(lil2cpp, "il2cpp_string_new_wrapper"));
    if (!f_string_new)
        f_string_new = reinterpret_cast<p_string_new>(
            g_api->so_symbol(lil2cpp, "il2cpp_string_new"));
    f_string_length = reinterpret_cast<p_string_length>(
        g_api->so_symbol(lil2cpp, "il2cpp_string_length"));
    f_gc_wbarrier_set_field = reinterpret_cast<p_gc_wbarrier_set_field>(
        g_api->so_symbol(lil2cpp, "il2cpp_gc_wbarrier_set_field"));
    f_image_get_class_count = reinterpret_cast<p_image_get_class_count>(
        g_api->so_symbol(lil2cpp, "il2cpp_image_get_class_count"));
    f_image_get_class = reinterpret_cast<p_image_get_class>(
        g_api->so_symbol(lil2cpp, "il2cpp_image_get_class"));

    if (!f_domain_get || !f_domain_assemblies || !f_assembly_image ||
        !f_image_name || !f_class_from_name || !f_get_method ||
        !f_class_get_field_from_name || !f_field_get_offset || !f_string_new) {
        g_api->log("PC01CDN", "required IL2CPP exports missing; disabled");
        g_enabled = false;
        return BOGO_PLUGIN_OK;
    }

    // Must NOT call IL2CPP domain/class APIs here: plugin load runs before
    // il2cpp_init. Wait for post-init.
    if (!g_api->register_il2cpp_post_init(&install, nullptr)) {
        g_api->log("PC01CDN", "register_il2cpp_post_init failed");
        return -1;
    }
    g_api->log("PC01CDN", "armed (base_url=%s); hook on il2cpp_init", g_base_url.c_str());
    return BOGO_PLUGIN_OK;
}

} // namespace

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION)
        return -1;
    g_api_storage = *api;
    g_api = &g_api_storage;

    if (!api->il2cpp)
        return BOGO_PLUGIN_DEFERRED;
    return init_il2cpp(api->il2cpp);
}
