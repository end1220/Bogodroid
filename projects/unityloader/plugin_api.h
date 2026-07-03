#ifndef BOGODROID_PLUGIN_API_H
#define BOGODROID_PLUGIN_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOGODROID_PLUGIN_ABI_VERSION 1u

typedef struct so_module BogoSoModule;
typedef void (*BogoIl2cppPostInitCallback)(void* userdata);

typedef struct BogoPluginApi {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* config_path;
    BogoSoModule* il2cpp;

    const char* (*getenv)(const char* name);
    const char* (*config_get_string)(const char* dotted_key, const char* fallback);
    int (*config_get_bool)(const char* dotted_key, int fallback);
    int64_t (*config_get_i64)(const char* dotted_key, int64_t fallback);

    uintptr_t (*so_symbol)(BogoSoModule* mod, const char* name);
    void (*hook_address_detour)(BogoSoModule* mod, uintptr_t addr, uintptr_t dst, uintptr_t* orig_out);
    int (*register_il2cpp_post_init)(BogoIl2cppPostInitCallback cb, void* userdata);

    void (*log)(const char* tag, const char* fmt, ...);
} BogoPluginApi;

typedef int (*BogoPluginInit)(const BogoPluginApi* api);

#ifdef __cplusplus
}
#endif

#endif
