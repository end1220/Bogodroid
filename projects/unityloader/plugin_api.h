#ifndef BOGODROID_PLUGIN_API_H
#define BOGODROID_PLUGIN_API_H

#include <stdint.h>
#include "input_observer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOGODROID_PLUGIN_ABI_VERSION 8u

enum BogoPluginInitResult {
    BOGO_PLUGIN_OK = 0,
    BOGO_PLUGIN_DEFERRED = 1
};

typedef struct so_module BogoSoModule;
typedef void (*BogoIl2cppPostInitCallback)(void* userdata);
typedef void (*BogoJniInitCallback)(void* jvm, void* userdata);
typedef void (*BogoPresentCallback)(void* userdata);
typedef void (*BogoAssetSourceCallback)(const char* path, void* userdata);
typedef void (*BogoModuleLoadedCallback)(const char* name,
                                         BogoSoModule* module,
                                         void* userdata);
typedef union BogoJniValue {
    uint8_t z;
    int8_t b;
    uint16_t c;
    int16_t s;
    int32_t i;
    int64_t j;
    float f;
    double d;
    void* l;
} BogoJniValue;

typedef BogoJniValue (*BogoJniMethodCallback)(
    void* env, void* receiver, const BogoJniValue* args,
    uint32_t arg_count, void* userdata);

enum BogoJniMethodFlags {
    BOGO_JNI_METHOD_STATIC = 1u << 0
};

typedef struct BogoJniMethod {
    const char* name;
    const char* signature;
    uint32_t flags;
    BogoJniMethodCallback callback;
    void* userdata;
} BogoJniMethod;

typedef struct BogoPluginApi {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* config_path;
    BogoSoModule* il2cpp;
    void* jvm;

    const char* (*getenv)(const char* name);
    const char* (*config_get_string)(const char* dotted_key, const char* fallback);
    int (*config_get_bool)(const char* dotted_key, int fallback);
    int64_t (*config_get_i64)(const char* dotted_key, int64_t fallback);
    double (*config_get_f64)(const char* dotted_key, double fallback);

    uintptr_t (*so_symbol)(BogoSoModule* mod, const char* name);
    uintptr_t (*so_base)(BogoSoModule* mod);
    BogoSoModule* (*find_module)(const char* name);
    void (*hook_address_detour)(BogoSoModule* mod, uintptr_t addr, uintptr_t dst, uintptr_t* orig_out);
    int (*register_il2cpp_post_init)(BogoIl2cppPostInitCallback cb, void* userdata);
    int (*register_jni_init)(BogoJniInitCallback cb, void* userdata);
    int (*register_module_loaded)(const char* name,
                                  BogoModuleLoadedCallback cb,
                                  void* userdata);
    // Called from the render/present thread (eglSwapBuffers). Keep work small.
    int (*register_present_callback)(BogoPresentCallback cb, void* userdata);
    // SDL controller events and render-frame boundaries. Callbacks run on the
    // input/render thread; they must be bounded and must not throw.
    int (*register_input_observer)(const BogoInputObserver* observer);
    int (*inject_touch)(int pointer_id, float x, float y,
                        float design_width, float design_height,
                        int down, const char* source);
    int (*set_present_viewport)(int enabled, double render_scale,
                                const char* anchor, int offset_y);
    int (*register_asset_source_callback)(BogoAssetSourceCallback cb,
                                          void* userdata);
    int (*register_jni_class)(const char* class_name,
                              const BogoJniMethod* methods,
                              uint32_t method_count);
    int (*jni_string_utf8)(void* env, void* string_ref,
                           char* output, uint32_t output_size);
    void* (*jni_new_string_utf8)(void* env, const char* value);
    void* (*jni_new_object)(void* env, const char* class_name, void* userdata);
    void* (*jni_object_userdata)(void* object);
    void* (*jni_new_byte_array)(void* env, uint32_t length);
    int (*jni_byte_array_write)(void* env, void* array, const int8_t* data,
                                uint32_t length);
    void* (*jni_new_string_array)(void* env, uint32_t length);
    int (*jni_string_array_set)(void* env, void* array, uint32_t index,
                                void* string_ref);

    void (*log)(const char* tag, const char* fmt, ...);
} BogoPluginApi;

typedef int (*BogoPluginInit)(const BogoPluginApi* api);

#ifdef __cplusplus
}
#endif

#endif
