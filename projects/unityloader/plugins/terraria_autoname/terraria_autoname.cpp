#include "plugin_api.h"
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>

namespace terraria_autoname {
    static BogoPluginApi g_api_storage = {};
    static const BogoPluginApi* g_api = nullptr;
    typedef void* (*p_domain_get)();
    typedef void** (*p_domain_get_assemblies)(void*, size_t*);
    typedef void* (*p_assembly_get_image)(void*);
    typedef const char* (*p_image_get_name)(void*);
    typedef void* (*p_class_from_name)(void*, const char*, const char*);
    typedef void* (*p_get_method)(void*, const char*, int);
    typedef void* (*p_class_get_methods)(void*, void**);
    typedef const char* (*p_method_get_name)(void*);
    typedef uint32_t (*p_method_get_param_count)(void*);
    typedef void* (*p_class_get_fields)(void*, void**);
    typedef const char* (*p_field_get_name)(void*);
    typedef void* (*p_field_from_name)(void*, const char*);
    typedef size_t (*p_field_get_offset)(void*);
    typedef void* (*p_field_get_type)(void*);
    typedef char* (*p_type_get_name)(void*);
    typedef void* (*p_string_new)(const char*);

    typedef uintptr_t (*orig8_t)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,
                                 uintptr_t,uintptr_t,uintptr_t,uintptr_t);

    struct ClassFields {
        std::string cls;
        int offsets[8] = {};
        int count = 0;
    };

    struct Hook {
        std::string cls;
        std::string method;
        int argc = 0;
        uintptr_t orig = 0;
    };

    static p_domain_get            f_domain_get;
    static p_domain_get_assemblies f_domain_assemblies;
    static p_assembly_get_image    f_assembly_image;
    static p_image_get_name        f_image_name;
    static p_class_from_name       f_class_from_name;
    static p_get_method            f_get_method;
    static p_class_get_methods     f_class_get_methods;
    static p_method_get_name       f_method_get_name;
    static p_method_get_param_count f_method_get_param_count;
    static p_class_get_fields      f_class_get_fields;
    static p_field_get_name        f_field_get_name;
    static p_field_from_name       f_field_from_name;
    static p_field_get_offset      f_field_get_offset;
    static p_field_get_type        f_field_get_type;
    static p_type_get_name         f_type_get_name;
    static p_string_new            f_string_new;

    static BogoSoModule* g_mod = nullptr;
    static bool g_enabled = false;
    static bool g_debug = false;
    static bool g_auto_create = false;
    static bool g_auto_create_attempted = false;
    static int g_create_menu_draws = 0;
    static std::string g_name;
    static ClassFields g_fields[4];
    static int g_field_class_n = 0;
    static Hook g_hooks[16];
    static int g_hook_n = 0;

    static void* find_image(const char* want) {
        void* dom = f_domain_get(); size_t n = 0;
        void** as = f_domain_assemblies(dom, &n);
        for (size_t i = 0; i < n; i++) {
            void* img = f_assembly_image(as[i]);
            const char* nm = f_image_name(img);
            if (nm && strstr(nm, want)) return img;
        }
        return nullptr;
    }

    static bool env_enabled(const char* name) {
        const char* value = g_api->getenv(name);
        return value && *value && strcmp(value, "0") != 0;
    }

    static bool config_enabled() {
        return g_api->config_get_bool("game_patches.terraria_autoname.enabled",
               g_api->config_get_bool("terraria_autoname.enabled", 0)) != 0;
    }

    static bool config_auto_create(bool fallback) {
        return g_api->config_get_bool("game_patches.terraria_autoname.auto_create",
               g_api->config_get_bool("terraria_autoname.auto_create", fallback ? 1 : 0)) != 0;
    }

    static std::string configured_name() {
        const char* env_name = g_api->getenv("TER_AUTONAME_VALUE");
        if (env_name && *env_name)
            return env_name;
        const char* generic_name = g_api->getenv("BD_SOFT_INPUT_DEFAULT");
        if (generic_name && *generic_name)
            return generic_name;
        const char* patch_name = g_api->config_get_string("game_patches.terraria_autoname.name", "");
        if (patch_name && *patch_name)
            return patch_name;
        return g_api->config_get_string("soft_input.default", "");
    }

    static bool is_string_field(void* field) {
        if (!f_field_get_type || !f_type_get_name) return true;
        void* ty = f_field_get_type(field);
        char* name = ty ? f_type_get_name(ty) : nullptr;
        bool ok = name && strstr(name, "String");
        if (g_debug)
            g_api->log("AUTONAME", "field type %s -> %s", name ? name : "(null)", ok ? "use" : "skip");
        return ok;
    }

    static void add_field(void* klass, ClassFields& cf, const char* field_name) {
        if (cf.count >= 8) return;
        void* f = f_field_from_name ? f_field_from_name(klass, field_name) : nullptr;
        if (!f) return;
        if (!is_string_field(f)) return;
        size_t off = f_field_get_offset ? f_field_get_offset(f) : 0;
        if (off == 0 || off > 0x10000) {
            if (g_debug)
                g_api->log("AUTONAME", "%s.%s has unusable offset %#zx", cf.cls.c_str(), field_name, off);
            return;
        }
        cf.offsets[cf.count++] = (int)off;
        if (g_debug)
            g_api->log("AUTONAME", "%s.%s offset %#zx", cf.cls.c_str(), field_name, off);
    }

    static void enumerate_fields(void* klass, const char* cls) {
        if (!f_class_get_fields || !f_field_get_name) return;
        void* iter = nullptr;
        int seen = 0;
        while (void* f = f_class_get_fields(klass, &iter)) {
            const char* name = f_field_get_name(f);
            void* ty = f_field_get_type ? f_field_get_type(f) : nullptr;
            char* type_name = ty && f_type_get_name ? f_type_get_name(ty) : nullptr;
            size_t off = f_field_get_offset ? f_field_get_offset(f) : 0;
            if (g_debug)
                g_api->log("AUTONAME", "field %s.%s type=%s offset=%#zx",
                       cls, name ? name : "(null)", type_name ? type_name : "(null)", off);
            if (++seen > 120) break;
        }
    }

    static void apply_to(const std::string& cls, uintptr_t self) {
        if (!self || !f_string_new) return;
        void* s = f_string_new(g_name.c_str());
        if (!s) return;
        for (int c = 0; c < g_field_class_n; c++) {
            if (g_fields[c].cls != cls) continue;
            for (int i = 0; i < g_fields[c].count; i++) {
                *(void**)(self + g_fields[c].offsets[i]) = s;
            }
        }
    }

    static bool plausible_object(uintptr_t p) {
        return p > 0x10000 && p < 0x8000000000ULL;
    }

    static void apply_to_player_file_data(uintptr_t file_data) {
        if (!plausible_object(file_data) || !f_string_new) return;
        void* s = f_string_new(g_name.c_str());
        if (!s) return;

        constexpr size_t kFileDataNameOffset = 0x28;
        constexpr size_t kPlayerFileDataPlayerOffset = 0x40;
        constexpr size_t kPlayerNameOffset = 0xe0;

        *(void**)(file_data + kFileDataNameOffset) = s;

        uintptr_t player = *(uintptr_t*)(file_data + kPlayerFileDataPlayerOffset);
        if (plausible_object(player)) {
            *(void**)(player + kPlayerNameOffset) = s;
            g_api->log("AUTONAME", "patched PlayerFileData.Name + Player.name to '%s'", g_name.c_str());
        } else {
            g_api->log("AUTONAME", "patched FileData.Name to '%s' (player ptr %#" PRIxPTR ")",
                   g_name.c_str(), player);
        }
    }

    static bool is_name_editor_open_method(const std::string& method) {
        return method == "EnterName" ||
               method == "OpenNameEdit" ||
               method == "InputCharacterName" ||
               method == "ActivateName";
    }

    static bool is_noisy_draw_method(const std::string& method) {
        return method == "DrawName" ||
               method == "DrawPlayerPreviewText";
    }

    static uintptr_t find_original(const std::string& cls, const std::string& method, int argc) {
        for (int i = 0; i < g_hook_n; i++) {
            if (g_hooks[i].cls == cls && g_hooks[i].method == method && g_hooks[i].argc == argc) {
                return g_hooks[i].orig;
            }
        }
        return 0;
    }

    static void auto_create_now(uintptr_t self, const char* reason) {
        if (!g_auto_create || g_auto_create_attempted || !self) return;
        g_auto_create_attempted = true;
        apply_to("GUIPlayerCreateMenu", self);
        uintptr_t create_and_save = find_original("GUIPlayerCreateMenu", "CreateAndSave", 0);
        if (!create_and_save) {
            g_api->log("AUTONAME", "auto-create skipped: CreateAndSave original missing");
            return;
        }
        g_api->log("AUTONAME", "auto-create player via CreateAndSave (%s), name '%s'",
               reason ? reason : "unknown", g_name.c_str());
        ((orig8_t)create_and_save)(self, 0,0,0,0,0,0,0);
        apply_to("GUIPlayerCreateMenu", self);
    }

    static void maybe_auto_create(uintptr_t self, const std::string& method) {
        if (!g_auto_create || g_auto_create_attempted || !self) return;
        if (method != "DrawName" && method != "DrawPlayerPreviewText") return;
        if (++g_create_menu_draws < 90) return;
        auto_create_now(self, method.c_str());
    }

    static uintptr_t hook_common(int idx, uintptr_t x0,uintptr_t x1,uintptr_t x2,uintptr_t x3,
                                 uintptr_t x4,uintptr_t x5,uintptr_t x6,uintptr_t x7) {
        apply_to(g_hooks[idx].cls, x0);
        if (g_hooks[idx].cls == "GUIPlayerCreateMenu" && is_name_editor_open_method(g_hooks[idx].method)) {
            if (g_auto_create) {
                auto_create_now(x0, g_hooks[idx].method.c_str());
                g_api->log("AUTONAME", "%s.%s(%d) suppressed name editor, forced name '%s'",
                       g_hooks[idx].cls.c_str(), g_hooks[idx].method.c_str(), g_hooks[idx].argc, g_name.c_str());
                return 0;
            }
            g_api->log("AUTONAME", "%s.%s(%d) opening name editor with default '%s'",
                   g_hooks[idx].cls.c_str(), g_hooks[idx].method.c_str(), g_hooks[idx].argc, g_name.c_str());
        }
        uintptr_t ret = ((orig8_t)g_hooks[idx].orig)(x0,x1,x2,x3,x4,x5,x6,x7);
        if (g_hooks[idx].cls == "GUIPlayerCreateMenu" && g_hooks[idx].method == "CreatePlayer") {
            apply_to_player_file_data(ret);
        }
        apply_to(g_hooks[idx].cls, x0);
        if (g_hooks[idx].cls == "GUIPlayerCreateMenu") {
            maybe_auto_create(x0, g_hooks[idx].method);
        }
        if (!is_noisy_draw_method(g_hooks[idx].method)) {
            g_api->log("AUTONAME", "%s.%s(%d) forced name '%s'",
                   g_hooks[idx].cls.c_str(), g_hooks[idx].method.c_str(), g_hooks[idx].argc, g_name.c_str());
        }
        return ret;
    }

    #define TER_AUTONAME_HOOK(K) \
    static uintptr_t hook##K(uintptr_t x0,uintptr_t x1,uintptr_t x2,uintptr_t x3, \
                             uintptr_t x4,uintptr_t x5,uintptr_t x6,uintptr_t x7){ \
        return hook_common(K, x0,x1,x2,x3,x4,x5,x6,x7); \
    }
    TER_AUTONAME_HOOK(0) TER_AUTONAME_HOOK(1) TER_AUTONAME_HOOK(2) TER_AUTONAME_HOOK(3)
    TER_AUTONAME_HOOK(4) TER_AUTONAME_HOOK(5) TER_AUTONAME_HOOK(6) TER_AUTONAME_HOOK(7)
    TER_AUTONAME_HOOK(8) TER_AUTONAME_HOOK(9) TER_AUTONAME_HOOK(10) TER_AUTONAME_HOOK(11)
    TER_AUTONAME_HOOK(12) TER_AUTONAME_HOOK(13) TER_AUTONAME_HOOK(14) TER_AUTONAME_HOOK(15)
    static void* g_hookfn[16] = {
        (void*)hook0, (void*)hook1, (void*)hook2, (void*)hook3,
        (void*)hook4, (void*)hook5, (void*)hook6, (void*)hook7,
        (void*)hook8, (void*)hook9, (void*)hook10, (void*)hook11,
        (void*)hook12, (void*)hook13, (void*)hook14, (void*)hook15
    };

    static void try_hook(void* klass, const char* cls, const char* method) {
        if (g_hook_n >= 16) return;
        for (int argc = 0; argc <= 3; argc++) {
            bool exists = false;
            for (int i = 0; i < g_hook_n; i++) {
                if (g_hooks[i].cls == cls && g_hooks[i].method == method && g_hooks[i].argc == argc) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;
            void* m = f_get_method(klass, method, argc);
            if (!m) continue;
            uintptr_t ptr = *(uintptr_t*)m;
            if (!ptr) continue;
            uintptr_t orig = 0;
            g_api->hook_address_detour(g_mod, ptr, (uintptr_t)g_hookfn[g_hook_n], &orig);
            if (!orig) {
                g_api->log("AUTONAME", "detour failed for %s.%s(%d)", cls, method, argc);
                continue;
            }
            g_hooks[g_hook_n].cls = cls;
            g_hooks[g_hook_n].method = method;
            g_hooks[g_hook_n].argc = argc;
            g_hooks[g_hook_n].orig = orig;
            g_api->log("AUTONAME", "hooked %s.%s(%d) @ %p", cls, method, argc, (void*)ptr);
            g_hook_n++;
            return;
        }
    }

    static bool should_hook_method(const char* name) {
        if (!name) return false;
        const char* exact[] = {
            "OpenNameEdit", "InputCharacterName", "CreatePlayer", "CreateAndSave",
            "CloseNameEdit", "CloseNameEditAndSave", "CloseNameEditAndSaveIf",
            "EnterName", "ActivateName", "DeactivateName", "GenerateName",
            "OnCharacterNamed", "DrawName", "DrawPlayerPreviewText"
        };
        for (const char* e : exact) {
            if (strcmp(name, e) == 0) return true;
        }
        return false;
    }

    static void enumerate_and_hook(void* klass, const char* cls) {
        if (!f_class_get_methods || !f_method_get_name || !f_method_get_param_count) return;
        void* iter = nullptr;
        int seen = 0;
        while (void* m = f_class_get_methods(klass, &iter)) {
            const char* name = f_method_get_name(m);
            uint32_t argc = f_method_get_param_count(m);
            if (name && (strstr(name, "Name") || strstr(name, "Player") || strstr(name, "Create") ||
                         strstr(name, "Edit") || strstr(name, "Enter") || strstr(name, "Input") ||
                         strstr(name, "Save") || strstr(name, "Open") || strstr(name, "Close"))) {
                if (g_debug)
                    g_api->log("AUTONAME", "method %s.%s(%u)", cls, name, argc);
            }
            if (should_hook_method(name)) try_hook(klass, cls, name);
            if (++seen > 200) break;
        }
    }

    static void install() {
        if (!g_enabled) return;
        void* img = find_image("Assembly-CSharp");
        if (!img) { g_api->log("AUTONAME", "Assembly-CSharp image not found"); return; }

        struct LogClass {
            const char* ns;
            const char* cls;
        };
        const LogClass log_classes[] = {
            { "Terraria", "Player" },
            { "Terraria.IO", "PlayerFileData" },
            { "Terraria.IO", "FileData" },
            { "Terraria", "Main" },
            { "", "PlayerFileData" },
            { "", "FileData" },
        };
        for (const auto& lc : log_classes) {
            void* klass = f_class_from_name(img, lc.ns, lc.cls);
            if (!klass) {
                if (g_debug)
                    g_api->log("AUTONAME", "class not found: %s.%s", lc.ns, lc.cls);
                continue;
            }
            if (g_debug)
                g_api->log("AUTONAME", "class found: %s.%s", lc.ns, lc.cls);
            enumerate_fields(klass, lc.cls);
        }

        const char* classes[] = { "GUIPlayerCreateMenu", "GUIPlayerNameMenu", "GUIPlayerCreateController" };
        const char* fields[] = { "_playerName", "editPlayerName", "editNameValue", "playerName", "PlayerName" };
        for (const char* cls : classes) {
            void* klass = f_class_from_name(img, "", cls);
            if (!klass) continue;
            if (g_field_class_n < 4) {
                ClassFields& cf = g_fields[g_field_class_n];
                cf.cls = cls;
                for (const char* f : fields) add_field(klass, cf, f);
                if (cf.count > 0) g_field_class_n++;
            }
            enumerate_fields(klass, cls);
            enumerate_and_hook(klass, cls);
            const char* methods[] = {
                "OpenNameEdit", "InputCharacterName", "CreatePlayer", "CreateAndSave",
                "CloseNameEditAndSave", "CloseNameEditAndSaveIf",
                "EnterName", "ActivateName", "DeactivateName", "GenerateName",
                "DrawName", "DrawPlayerPreviewText"
            };
            for (const char* m : methods) try_hook(klass, cls, m);
        }
        g_api->log("AUTONAME", "installed: name='%s' classes=%d hooks=%d",
               g_name.c_str(), g_field_class_n, g_hook_n);
    }

    static void post_init(void*) { install(); }

    static int init(BogoSoModule* lil2cpp) {
        bool enabled = env_enabled("TER_AUTONAME") || config_enabled();
        if (!enabled) return 0;

        g_auto_create = env_enabled("TER_AUTOCREATE_PLAYER");
        g_auto_create = config_auto_create(g_auto_create);
        g_debug = g_api->config_get_bool("game_patches.terraria_autoname.debug",
                  g_api->config_get_bool("terraria_autoname.debug", 0)) != 0;

        g_name = configured_name();
        if (g_name.empty()) {
            g_api->log("AUTONAME", "enabled but no name configured; set [game_patches.terraria_autoname].name or BD_SOFT_INPUT_DEFAULT");
            return 0;
        }

        g_mod = lil2cpp;
        f_domain_get        = (p_domain_get)g_api->so_symbol(lil2cpp, "il2cpp_domain_get");
        f_domain_assemblies = (p_domain_get_assemblies)g_api->so_symbol(lil2cpp, "il2cpp_domain_get_assemblies");
        f_assembly_image    = (p_assembly_get_image)g_api->so_symbol(lil2cpp, "il2cpp_assembly_get_image");
        f_image_name        = (p_image_get_name)g_api->so_symbol(lil2cpp, "il2cpp_image_get_name");
        f_class_from_name   = (p_class_from_name)g_api->so_symbol(lil2cpp, "il2cpp_class_from_name");
        f_get_method        = (p_get_method)g_api->so_symbol(lil2cpp, "il2cpp_class_get_method_from_name");
        f_class_get_methods = (p_class_get_methods)g_api->so_symbol(lil2cpp, "il2cpp_class_get_methods");
        f_method_get_name   = (p_method_get_name)g_api->so_symbol(lil2cpp, "il2cpp_method_get_name");
        f_method_get_param_count = (p_method_get_param_count)g_api->so_symbol(lil2cpp, "il2cpp_method_get_param_count");
        f_class_get_fields  = (p_class_get_fields)g_api->so_symbol(lil2cpp, "il2cpp_class_get_fields");
        f_field_get_name    = (p_field_get_name)g_api->so_symbol(lil2cpp, "il2cpp_field_get_name");
        f_field_from_name   = (p_field_from_name)g_api->so_symbol(lil2cpp, "il2cpp_class_get_field_from_name");
        f_field_get_offset  = (p_field_get_offset)g_api->so_symbol(lil2cpp, "il2cpp_field_get_offset");
        f_field_get_type    = (p_field_get_type)g_api->so_symbol(lil2cpp, "il2cpp_field_get_type");
        f_type_get_name     = (p_type_get_name)g_api->so_symbol(lil2cpp, "il2cpp_type_get_name");
        f_string_new        = (p_string_new)g_api->so_symbol(lil2cpp, "il2cpp_string_new_wrapper");
        if (!f_string_new) f_string_new = (p_string_new)g_api->so_symbol(lil2cpp, "il2cpp_string_new");
        if (!f_domain_get || !f_class_from_name || !f_get_method || !f_field_from_name ||
            !f_field_get_offset || !f_string_new) {
            g_api->log("AUTONAME", "missing il2cpp exports; disabled");
            return 0;
        }

        g_enabled = true;
        if (!g_api->register_il2cpp_post_init(&post_init, nullptr)) {
            g_api->log("AUTONAME", "failed to register post-init callback");
            return 0;
        }
        g_api->log("AUTONAME", "armed: name='%s' auto_create=%d", g_name.c_str(), g_auto_create ? 1 : 0);
        return 0;
    }
}

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api) {
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(BogoPluginApi))
        return -1;
    if (!api->il2cpp)
        return BOGO_PLUGIN_DEFERRED;
    terraria_autoname::g_api_storage = *api;
    terraria_autoname::g_api = &terraria_autoname::g_api_storage;
    return terraria_autoname::init(api->il2cpp);
}
