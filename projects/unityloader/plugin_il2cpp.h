#ifndef BOGODROID_PLUGIN_IL2CPP_H
#define BOGODROID_PLUGIN_IL2CPP_H

#include "plugin_api.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bogodroid {

// Shared plugin utilities without title names, offsets or input policies.
// Call bind after IL2CPP init. Managed lookups/calls belong on the Unity thread.
// Uses the existing C plugin ABI; no C++ state crosses the loader/plugin DSO.
class Il2cppRuntime {
    const BogoPluginApi* api_ = nullptr;
    const char* tag_ = "IL2CPP";

public:
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

    p_domain_get f_domain_get = nullptr;
    p_domain_get_assemblies f_domain_assemblies = nullptr;
    p_assembly_get_image f_assembly_image = nullptr;
    p_image_get_name f_image_name = nullptr;
    p_class_from_name f_class_from_name = nullptr;
    p_get_method f_get_method = nullptr;
    p_class_get_methods f_class_get_methods = nullptr;
    p_method_get_name f_method_get_name = nullptr;
    p_method_get_param_count f_method_get_param_count = nullptr;
    p_class_get_type f_class_get_type = nullptr;
    p_type_get_object f_type_get_object = nullptr;
    p_class_get_fields f_class_get_fields = nullptr;
    p_field_get_name f_field_get_name = nullptr;
    p_field_get_type f_field_get_type = nullptr;
    p_field_get_offset f_field_get_offset = nullptr;
    p_type_get_name f_type_get_name = nullptr;
    void (*f_free)(void*) = nullptr;

    bool bind(const BogoPluginApi* api, const char* tag)
    {
        api_ = api;
        tag_ = tag ? tag : "IL2CPP";
        if (!api_ || !api_->il2cpp || !api_->so_symbol || !api_->log)
            return false;
        BogoSoModule* il2cpp = api_->il2cpp;
        f_domain_get = reinterpret_cast<p_domain_get>(
            api_->so_symbol(il2cpp, "il2cpp_domain_get"));
        f_domain_assemblies = reinterpret_cast<p_domain_get_assemblies>(
            api_->so_symbol(il2cpp, "il2cpp_domain_get_assemblies"));
        f_assembly_image = reinterpret_cast<p_assembly_get_image>(
            api_->so_symbol(il2cpp, "il2cpp_assembly_get_image"));
        f_image_name = reinterpret_cast<p_image_get_name>(
            api_->so_symbol(il2cpp, "il2cpp_image_get_name"));
        f_class_from_name = reinterpret_cast<p_class_from_name>(
            api_->so_symbol(il2cpp, "il2cpp_class_from_name"));
        f_get_method = reinterpret_cast<p_get_method>(
            api_->so_symbol(il2cpp, "il2cpp_class_get_method_from_name"));
        f_class_get_methods = reinterpret_cast<p_class_get_methods>(
            api_->so_symbol(il2cpp, "il2cpp_class_get_methods"));
        f_method_get_name = reinterpret_cast<p_method_get_name>(
            api_->so_symbol(il2cpp, "il2cpp_method_get_name"));
        f_method_get_param_count = reinterpret_cast<p_method_get_param_count>(
            api_->so_symbol(il2cpp, "il2cpp_method_get_param_count"));
        f_class_get_type = reinterpret_cast<p_class_get_type>(
            api_->so_symbol(il2cpp, "il2cpp_class_get_type"));
        f_type_get_object = reinterpret_cast<p_type_get_object>(
            api_->so_symbol(il2cpp, "il2cpp_type_get_object"));
        f_class_get_fields = reinterpret_cast<p_class_get_fields>(
            api_->so_symbol(il2cpp, "il2cpp_class_get_fields"));
        f_field_get_name = reinterpret_cast<p_field_get_name>(
            api_->so_symbol(il2cpp, "il2cpp_field_get_name"));
        f_field_get_type = reinterpret_cast<p_field_get_type>(
            api_->so_symbol(il2cpp, "il2cpp_field_get_type"));
        f_field_get_offset = reinterpret_cast<p_field_get_offset>(
            api_->so_symbol(il2cpp, "il2cpp_field_get_offset"));
        f_type_get_name = reinterpret_cast<p_type_get_name>(
            api_->so_symbol(il2cpp, "il2cpp_type_get_name"));
        f_free = reinterpret_cast<void (*)(void*)>(
            api_->so_symbol(il2cpp, "il2cpp_free"));
        return f_domain_get && f_domain_assemblies && f_assembly_image &&
            f_image_name && f_class_from_name && f_get_method;
    }

    void* find_image(const char* want)
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

    void* find_class_image(const char* namespaze, const char* klass)
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

    bool resolve_call(void* image, const char* namespaze, const char* klass,
                      const char* method, int argc, void** fn_out,
                      void** mi_out)
    {
        if (!image || !f_class_from_name || !f_get_method || !fn_out || !mi_out)
            return false;
        *fn_out = nullptr;
        *mi_out = nullptr;
        void* k = f_class_from_name(image, namespaze, klass);
        if (!k) {
            api_->log(tag_, "class %s.%s not found", namespaze, klass);
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
            api_->log(tag_, "method %s.%s(%d) not found", klass, method,
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
                        api_->log(tag_, "  candidate %s.%s(%d)", klass,
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

    bool hook_method(void* image, const char* namespaze, const char* klass,
                     const char* method, int argc, uintptr_t hook,
                     uintptr_t* orig_out)
    {
        if (!api_ || !api_->hook_address_detour || !orig_out)
            return false;
        void* fn = nullptr;
        void* mi = nullptr;
        if (!resolve_call(image, namespaze, klass, method, argc, &fn, &mi))
            return false;
        uintptr_t orig = 0;
        api_->hook_address_detour(api_->il2cpp, reinterpret_cast<uintptr_t>(fn),
                                   hook, &orig);
        if (!orig) {
            api_->log(tag_, "detour %s.%s FAILED", klass, method);
            return false;
        }
        *orig_out = orig;
        api_->log(tag_, "hooked %s.%s(%d) @ %p", klass, method, argc,
                   fn);
        return true;
    }

    void* resolve_type_object(void* image, const char* namespaze,
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

    // Returns zero when metadata is unavailable; callers validate expected layout.
    int field_offset_for(void* image, const char* klass_name,
                         const char* field_name, const char* namespaze = "")
    {
        if (!image || !klass_name || !field_name || !f_class_from_name ||
            !f_class_get_fields || !f_field_get_name || !f_field_get_offset)
            return 0;
        void* klass = f_class_from_name(image, namespaze, klass_name);
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

    void log_class(void* image, const char* namespaze, const char* klass_name)
    {
        if (!image || !f_class_from_name || !api_)
            return;
        void* klass = f_class_from_name(image, namespaze, klass_name);
        if (!klass)
            return;
        api_->log(tag_, "class %s found", klass_name);
        if (f_class_get_fields && f_field_get_name) {
            void* iter = nullptr;
            int logged = 0;
            while (void* field = f_class_get_fields(klass, &iter)) {
                const char* field_name = f_field_get_name(field);
                const char* type_name = "?";
                char* resolved = nullptr;
                if (f_field_get_type && f_type_get_name) {
                    void* type = f_field_get_type(field);
                    resolved = type ? f_type_get_name(type) : nullptr;
                    if (resolved)
                        type_name = resolved;
                }
                const size_t offset = f_field_get_offset
                    ? f_field_get_offset(field)
                    : 0;
                api_->log(tag_, "  field %s.%s type=%s offset=0x%zx",
                           klass_name, field_name ? field_name : "?",
                           type_name, offset);
                if (resolved && f_free)
                    f_free(resolved);
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
                api_->log(tag_, "  method %s.%s(%d)", klass_name,
                           method_name, f_method_get_param_count(method));
                if (++logged >= 120)
                    break;
            }
        }
    }
};

} // namespace bogodroid

#endif
