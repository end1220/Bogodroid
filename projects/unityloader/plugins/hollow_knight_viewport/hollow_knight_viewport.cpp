#include "plugin_api.h"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace hollow_knight_viewport {
    static BogoPluginApi g_api_storage = {};
    static const BogoPluginApi* g_api = nullptr;

    typedef void* (*p_domain_get)();
    typedef void** (*p_domain_get_assemblies)(void* domain, size_t* size);
    typedef void* (*p_assembly_get_image)(void* assembly);
    typedef const char* (*p_image_get_name)(void* image);
    typedef void* (*p_class_from_name)(void* image, const char* ns, const char* name);
    typedef void* (*p_get_method)(void* klass, const char* name, int argc);
    typedef void* (*p_field_from_name)(void* klass, const char* name);
    typedef size_t (*p_field_get_offset)(void* field);

    static bool g_enabled = false;
    static bool g_debug = false;
    static bool g_force_full_viewport = false;
    static bool g_tk2d_width_safe = false;
    static bool g_ui_orthographic_width_safe = false;
    static BogoSoModule* g_mod = nullptr;
    static p_domain_get f_domain_get = nullptr;
    static p_domain_get_assemblies f_domain_assemblies = nullptr;
    static p_assembly_get_image f_assembly_image = nullptr;
    static p_image_get_name f_image_name = nullptr;
    static p_class_from_name f_class_from_name = nullptr;
    static p_get_method f_get_method = nullptr;
    static p_field_from_name f_field_from_name = nullptr;
    static p_field_get_offset f_field_get_offset = nullptr;

    static uintptr_t g_force_aspect_set_overscan_orig = 0;
    static uintptr_t g_force_aspect_auto_scale_orig = 0;
    static uintptr_t g_game_cameras_set_overscan_orig = 0;
    static uintptr_t g_gs_load_overscan_orig = 0;
    static uintptr_t g_gs_save_overscan_orig = 0;
    static uintptr_t g_camera_set_rect_injected_orig = 0;
    static uintptr_t g_camera_set_orthographic_size_orig = 0;
    static uintptr_t g_tk2d_update_camera_matrix_orig = 0;
    static int g_gs_overscan_adjustment_off = -1;
    static int g_gs_overscan_adjusted_off = -1;

    typedef void (*orig_float_void_t)(void*, float, void*);
    typedef float (*orig_float_ret_t)(void*, void*);
    typedef void (*orig_void_t)(void*, void*);
    struct UnityEngine_Rect_o {
        float x;
        float y;
        float width;
        float height;
    };
    struct UnityEngine_Vector2_o {
        float x;
        float y;
    };
    struct Tk2dCameraOffsets {
        int cameraSettings = 0x20;
        int nativeResolutionWidth = 0x38;
        int nativeResolutionHeight = 0x3c;
        int unityCamera = 0x40;
        int targetResolution = 0x5c;
        int zoomFactor = 0x64;
        int screenExtents = 0x74;
        int nativeScreenExtents = 0x84;
    };
    struct Tk2dCameraSettingsOffsets {
        int orthographicSize = 0x14;
        int orthographicPixelsPerMeter = 0x18;
        int orthographicType = 0x20;
        int rect = 0x2c;
    };
    typedef void (*camera_set_rect_injected_t)(void*, UnityEngine_Rect_o*, void*);
    typedef float (*camera_get_float_t)(void*, void*);
    typedef int (*screen_int_t)(void*);
    static Tk2dCameraOffsets g_tk2d_cam_off = {};
    static Tk2dCameraSettingsOffsets g_tk2d_settings_off = {};
    static camera_set_rect_injected_t g_camera_set_rect_injected = nullptr;
    static void* g_mi_camera_set_rect_injected = nullptr;
    static camera_get_float_t g_camera_get_orthographic_size = nullptr;
    static void* g_mi_camera_get_orthographic_size = nullptr;
    static void* g_mi_camera_set_orthographic_size = nullptr;
    static screen_int_t g_screen_width = nullptr;
    static screen_int_t g_screen_height = nullptr;
    static void* g_mi_screen_width = nullptr;
    static void* g_mi_screen_height = nullptr;

    static bool cfg_bool(const char* name, int fallback = 0) {
        char game_patch_key[128];
        std::snprintf(game_patch_key, sizeof(game_patch_key),
                      "game_patches.hollow_knight_viewport.%s", name);
        char compat_key[64];
        std::snprintf(compat_key, sizeof(compat_key), "hk_viewport.%s", name);
        return g_api->config_get_bool(game_patch_key,
               g_api->config_get_bool(compat_key, fallback)) != 0;
    }

    static void* find_image(const char* want) {
        void* dom = f_domain_get();
        size_t n = 0;
        void** as = f_domain_assemblies(dom, &n);
        for (size_t i = 0; i < n; i++) {
            void* img = f_assembly_image(as[i]);
            const char* nm = f_image_name(img);
            if (nm && std::strstr(nm, want)) return img;
        }
        return nullptr;
    }

    static bool resolve_method(void* img, const char* ns, const char* cls, const char* method,
                               int argc, uintptr_t hook, uintptr_t* orig_out) {
        void* k = f_class_from_name(img, ns, cls);
        if (!k) { g_api->log("HKVIEW", "class %s.%s not found", ns, cls); return false; }
        void* m = f_get_method(k, method, argc);
        if (!m) { g_api->log("HKVIEW", "method %s.%s(%d) not found", cls, method, argc); return false; }
        uintptr_t ptr = *(uintptr_t*)m;
        uintptr_t orig = 0;
        g_api->hook_address_detour(g_mod, ptr, hook, &orig);
        if (!orig) { g_api->log("HKVIEW", "detour %s.%s FAILED", cls, method); return false; }
        *orig_out = orig;
        g_api->log("HKVIEW", "hooked %s.%s(%d) @ %p", cls, method, argc, (void*)ptr);
        return true;
    }

    static bool resolve_call(void* img, const char* ns, const char* cls, const char* method,
                             int argc, void** fn_out, void** mi_out) {
        void* k = f_class_from_name(img, ns, cls);
        if (!k) return false;
        void* m = f_get_method(k, method, argc);
        if (!m) return false;
        *fn_out = (void*)*(uintptr_t*)m;
        *mi_out = m;
        return true;
    }

    static int screen_w() {
        return g_screen_width ? g_screen_width(g_mi_screen_width) : -1;
    }

    static int screen_h() {
        return g_screen_height ? g_screen_height(g_mi_screen_height) : -1;
    }

    static float width_safe_scale();
    static void apply_tk2d_width_safe(void* tk2d, const char* tag);
    static void apply_ui_width_safe_ortho(void* camera, const char* tag);
    static void log_tk2d_camera_state(const char* tag, void* tk2d);

    static void clamp_rect_to_full(UnityEngine_Rect_o* rect) {
        if (!rect) return;
        rect->x = 0.0f;
        rect->y = 0.0f;
        rect->width = 1.0f;
        rect->height = 1.0f;
    }

    static void apply_full_camera_rect(void* camera, const char* tag) {
        if (!g_force_full_viewport || !camera) return;
        camera_set_rect_injected_t set_rect =
            g_camera_set_rect_injected_orig
                ? (camera_set_rect_injected_t)g_camera_set_rect_injected_orig
                : g_camera_set_rect_injected;
        if (!set_rect) return;
        UnityEngine_Rect_o rect = {0.0f, 0.0f, 1.0f, 1.0f};
        set_rect(camera, &rect, g_mi_camera_set_rect_injected);
        static int count = 0;
        if (count++ < 32) {
            g_api->log("HKVIEW", "%s full camera rect applied camera=%p screen=%dx%d",
                       tag, camera, screen_w(), screen_h());
        }
    }

    struct OrthoBase {
        void* camera = nullptr;
        float base = 0.0f;
        float patched = 0.0f;
    };
    static OrthoBase g_ortho_base[16] = {};

    static float remember_ortho_base(void* camera, float current, float scale) {
        if (!camera || current <= 0.0f) return current;
        for (auto& slot : g_ortho_base) {
            if (slot.camera == camera) {
                if (slot.patched > 0.0f && current > 0.0f) {
                    float delta = current - slot.patched;
                    if (delta < 0.0f) delta = -delta;
                    if (delta < 0.01f) return slot.base;
                }
                slot.base = current;
                slot.patched = current * scale;
                return slot.base;
            }
        }
        for (auto& slot : g_ortho_base) {
            if (!slot.camera) {
                slot.camera = camera;
                slot.base = current;
                slot.patched = current * scale;
                return slot.base;
            }
        }
        return current;
    }

    struct Tk2dBase {
        void* camera = nullptr;
        void* settings = nullptr;
        float ortho_base = 0.0f;
        float ortho_patched = 0.0f;
        float ppm_base = 0.0f;
        float ppm_patched = 0.0f;
        float zoom_base = 0.0f;
        float zoom_patched = 0.0f;
    };
    static Tk2dBase g_tk2d_base[16] = {};

    static float absf_local(float v) {
        return v < 0.0f ? -v : v;
    }

    static bool looks_like_current_patch(float current, float patched) {
        return patched > 0.0f && current > 0.0f && absf_local(current - patched) < 0.01f;
    }

    static void apply_ui_width_safe_ortho(void* camera, const char* tag) {
        if (!g_ui_orthographic_width_safe || !camera || !g_camera_get_orthographic_size ||
            !g_camera_set_orthographic_size_orig) return;
        float scale = width_safe_scale();
        if (scale <= 1.0f) return;
        float current = g_camera_get_orthographic_size(camera, g_mi_camera_get_orthographic_size);
        float base = remember_ortho_base(camera, current, scale);
        float patched = base * scale;
        ((orig_float_void_t)g_camera_set_orthographic_size_orig)(
            camera, patched, g_mi_camera_set_orthographic_size);
        static int count = 0;
        if (count++ < 64) {
            g_api->log("HKVIEW", "%s UI ortho width-safe camera=%p %.4f(base %.4f) -> %.4f scale=%.4f screen=%dx%d",
                       tag, camera, (double)current, (double)base, (double)patched,
                       (double)scale, screen_w(), screen_h());
        }
    }

    static Tk2dBase* remember_tk2d_base(void* camera, void* settings,
                                        float ortho, float ppm, float zoom, float scale) {
        if (!camera) return nullptr;
        for (auto& slot : g_tk2d_base) {
            if (slot.camera == camera) {
                if (slot.settings != settings) {
                    slot.settings = settings;
                    slot.ortho_base = ortho;
                    slot.ppm_base = ppm;
                    slot.zoom_base = zoom;
                } else {
                    if (ortho > 0.0f && !looks_like_current_patch(ortho, slot.ortho_patched))
                        slot.ortho_base = ortho;
                    if (ppm > 0.0f && !looks_like_current_patch(ppm, slot.ppm_patched))
                        slot.ppm_base = ppm;
                    if (zoom > 0.0f && !looks_like_current_patch(zoom, slot.zoom_patched))
                        slot.zoom_base = zoom;
                }
                slot.ortho_patched = slot.ortho_base > 0.0f ? slot.ortho_base * scale : 0.0f;
                slot.ppm_patched = slot.ppm_base > 0.0f ? slot.ppm_base / scale : 0.0f;
                slot.zoom_patched = slot.zoom_base > 0.0f ? slot.zoom_base / scale : 0.0f;
                return &slot;
            }
        }
        for (auto& slot : g_tk2d_base) {
            if (!slot.camera) {
                slot.camera = camera;
                slot.settings = settings;
                slot.ortho_base = ortho;
                slot.ppm_base = ppm;
                slot.zoom_base = zoom;
                slot.ortho_patched = ortho > 0.0f ? ortho * scale : 0.0f;
                slot.ppm_patched = ppm > 0.0f ? ppm / scale : 0.0f;
                slot.zoom_patched = zoom > 0.0f ? zoom / scale : 0.0f;
                return &slot;
            }
        }
        return nullptr;
    }

    static void log_tk2d_camera_state(const char* tag, void* tk2d) {
        if (!tk2d) return;
        void* settings = *(void**)((uintptr_t)tk2d + g_tk2d_cam_off.cameraSettings);
        int native_w = *(int*)((uintptr_t)tk2d + g_tk2d_cam_off.nativeResolutionWidth);
        int native_h = *(int*)((uintptr_t)tk2d + g_tk2d_cam_off.nativeResolutionHeight);
        UnityEngine_Vector2_o target =
            *(UnityEngine_Vector2_o*)((uintptr_t)tk2d + g_tk2d_cam_off.targetResolution);
        float zoom = *(float*)((uintptr_t)tk2d + g_tk2d_cam_off.zoomFactor);
        UnityEngine_Rect_o ext =
            *(UnityEngine_Rect_o*)((uintptr_t)tk2d + g_tk2d_cam_off.screenExtents);
        UnityEngine_Rect_o native_ext =
            *(UnityEngine_Rect_o*)((uintptr_t)tk2d + g_tk2d_cam_off.nativeScreenExtents);
        float ortho = 0.0f;
        float ppm = 0.0f;
        int ortho_type = -1;
        UnityEngine_Rect_o rect = {0.0f, 0.0f, 0.0f, 0.0f};
        if (settings) {
            ortho = *(float*)((uintptr_t)settings + g_tk2d_settings_off.orthographicSize);
            ppm = *(float*)((uintptr_t)settings + g_tk2d_settings_off.orthographicPixelsPerMeter);
            ortho_type = *(int*)((uintptr_t)settings + g_tk2d_settings_off.orthographicType);
            rect = *(UnityEngine_Rect_o*)((uintptr_t)settings + g_tk2d_settings_off.rect);
        }
        g_api->log("HKVIEW", "%s tk2d=%p settings=%p native=%dx%d target=%.1fx%.1f zoom=%.4f ortho=%.4f ppm=%.4f type=%d rect=%.3f,%.3f %.3fx%.3f ext=%.2f,%.2f %.2fx%.2f nativeExt=%.2f,%.2f %.2fx%.2f screen=%dx%d",
                   tag, tk2d, settings, native_w, native_h,
                   (double)target.x, (double)target.y, (double)zoom,
                   (double)ortho, (double)ppm, ortho_type,
                   (double)rect.x, (double)rect.y, (double)rect.width, (double)rect.height,
                   (double)ext.x, (double)ext.y, (double)ext.width, (double)ext.height,
                   (double)native_ext.x, (double)native_ext.y,
                   (double)native_ext.width, (double)native_ext.height,
                   screen_w(), screen_h());
    }

    static void apply_tk2d_width_safe(void* tk2d, const char* tag) {
        if (!g_tk2d_width_safe || !tk2d) return;
        float scale = width_safe_scale();
        if (scale <= 1.0f) return;
        void* settings = *(void**)((uintptr_t)tk2d + g_tk2d_cam_off.cameraSettings);
        if (!settings) return;

        float ortho = *(float*)((uintptr_t)settings + g_tk2d_settings_off.orthographicSize);
        float ppm = *(float*)((uintptr_t)settings + g_tk2d_settings_off.orthographicPixelsPerMeter);
        float zoom = *(float*)((uintptr_t)tk2d + g_tk2d_cam_off.zoomFactor);
        Tk2dBase* base = remember_tk2d_base(tk2d, settings, ortho, ppm, zoom, scale);
        if (!base) return;

        if (base->ortho_patched > 0.0f)
            *(float*)((uintptr_t)settings + g_tk2d_settings_off.orthographicSize) = base->ortho_patched;
        if (base->ppm_patched > 0.0f)
            *(float*)((uintptr_t)settings + g_tk2d_settings_off.orthographicPixelsPerMeter) = base->ppm_patched;
        if (base->zoom_patched > 0.0f)
            *(float*)((uintptr_t)tk2d + g_tk2d_cam_off.zoomFactor) = base->zoom_patched;

        static int count = 0;
        if (count++ < 96) {
            UnityEngine_Vector2_o target =
                *(UnityEngine_Vector2_o*)((uintptr_t)tk2d + g_tk2d_cam_off.targetResolution);
            g_api->log("HKVIEW", "%s tk2d width-safe tk2d=%p target=%.1fx%.1f ortho %.4f(base %.4f)->%.4f ppm %.4f(base %.4f)->%.4f zoom %.4f(base %.4f)->%.4f scale=%.4f screen=%dx%d",
                       tag, tk2d, (double)target.x, (double)target.y,
                       (double)ortho, (double)base->ortho_base, (double)base->ortho_patched,
                       (double)ppm, (double)base->ppm_base, (double)base->ppm_patched,
                       (double)zoom, (double)base->zoom_base, (double)base->zoom_patched,
                       (double)scale, screen_w(), screen_h());
        }
    }

    static void log_game_settings(const char* tag, void* gs) {
        float overscan = 0.0f;
        int adjusted = -1;
        if (gs && g_gs_overscan_adjustment_off >= 0)
            overscan = *(float*)((uintptr_t)gs + g_gs_overscan_adjustment_off);
        if (gs && g_gs_overscan_adjusted_off >= 0)
            adjusted = *(int*)((uintptr_t)gs + g_gs_overscan_adjusted_off);
        g_api->log("HKVIEW", "%s gs=%p overScanAdjustment=%.4f overscanAdjusted=%d screen=%dx%d",
                   tag, gs, (double)overscan, adjusted, screen_w(), screen_h());
    }

    static void game_settings_load_overscan_hook(void* self, void* method) {
        ((orig_void_t)g_gs_load_overscan_orig)(self, method);
        log_game_settings("GameSettings.LoadOverscanSettings", self);
    }

    static void game_settings_save_overscan_hook(void* self, void* method) {
        ((orig_void_t)g_gs_save_overscan_orig)(self, method);
        log_game_settings("GameSettings.SaveOverscanSettings", self);
    }

    static void force_aspect_set_overscan_hook(void* self, float adjustment, void* method) {
        static int count = 0;
        if (count++ < 32) {
            g_api->log("HKVIEW", "ForceCameraAspect.SetOverscanViewport adjustment=%.4f self=%p screen=%dx%d",
                       (double)adjustment, self, screen_w(), screen_h());
        }
        ((orig_float_void_t)g_force_aspect_set_overscan_orig)(self, adjustment, method);
        if (self) {
            void* tk2d = *(void**)((uintptr_t)self + 0x18);
            void* hud = *(void**)((uintptr_t)self + 0x20);
            apply_tk2d_width_safe(tk2d, "ForceCameraAspect.tk2dCam");
            apply_full_camera_rect(hud, "ForceCameraAspect.hudCam");
            apply_ui_width_safe_ortho(hud, "ForceCameraAspect.hudCam");
        }
    }

    static float force_aspect_auto_scale_hook(void* self, void* method) {
        float ret = ((orig_float_ret_t)g_force_aspect_auto_scale_orig)(self, method);
        if (self) {
            void* tk2d = *(void**)((uintptr_t)self + 0x18);
            void* hud = *(void**)((uintptr_t)self + 0x20);
            apply_tk2d_width_safe(tk2d, "ForceCameraAspect.AutoScale.tk2dCam");
            apply_full_camera_rect(hud, "ForceCameraAspect.AutoScale");
            apply_ui_width_safe_ortho(hud, "ForceCameraAspect.AutoScale");
        }
        static int count = 0;
        if (count++ < 32) {
            g_api->log("HKVIEW", "ForceCameraAspect.AutoScaleViewport -> %.4f self=%p screen=%dx%d",
                       (double)ret, self, screen_w(), screen_h());
        }
        if (g_force_full_viewport) return 1.0f;
        return ret;
    }

    static void game_cameras_set_overscan_hook(void* self, float value, void* method) {
        static int count = 0;
        if (count++ < 32) {
            g_api->log("HKVIEW", "GameCameras.SetOverscan value=%.4f self=%p screen=%dx%d",
                       (double)value, self, screen_w(), screen_h());
        }
        ((orig_float_void_t)g_game_cameras_set_overscan_orig)(self, value, method);
        if (self) {
            void* hud = *(void**)((uintptr_t)self + 0x18);
            void* main = *(void**)((uintptr_t)self + 0x20);
            void* tk2d = *(void**)((uintptr_t)self + 0x88);
            apply_tk2d_width_safe(tk2d, "GameCameras.tk2dCam");
            apply_full_camera_rect(hud, "GameCameras.hudCamera");
            apply_full_camera_rect(main, "GameCameras.mainCamera");
            apply_ui_width_safe_ortho(hud, "GameCameras.hudCamera");
            apply_ui_width_safe_ortho(main, "GameCameras.mainCamera");
        }
    }

    static float width_safe_scale() {
        int w = screen_w();
        int h = screen_h();
        if (w <= 0 || h <= 0) return 1.0f;
        float aspect = (float)w / (float)h;
        const float target = 16.0f / 9.0f;
        if (aspect <= 0.0f || aspect >= target) return 1.0f;
        return target / aspect;
    }

    static void camera_set_rect_injected_hook(void* self, UnityEngine_Rect_o* rect, void* method) {
        UnityEngine_Rect_o before = rect ? *rect : UnityEngine_Rect_o{0.0f, 0.0f, 0.0f, 0.0f};
        if (g_force_full_viewport) clamp_rect_to_full(rect);
        static int count = 0;
        if (count++ < 96) {
            g_api->log("HKVIEW", "Camera.set_rect_Injected camera=%p rect %.4f,%.4f %.4fx%.4f -> %.4f,%.4f %.4fx%.4f screen=%dx%d",
                       self,
                       (double)before.x, (double)before.y, (double)before.width, (double)before.height,
                       rect ? (double)rect->x : 0.0, rect ? (double)rect->y : 0.0,
                       rect ? (double)rect->width : 0.0, rect ? (double)rect->height : 0.0,
                       screen_w(), screen_h());
        }
        ((camera_set_rect_injected_t)g_camera_set_rect_injected_orig)(self, rect, method);
    }

    static void camera_set_orthographic_size_hook(void* self, float value, void* method) {
        float scale = g_ui_orthographic_width_safe ? width_safe_scale() : 1.0f;
        float patched = value * scale;
        static int count = 0;
        if (count++ < 96) {
            g_api->log("HKVIEW", "Camera.set_orthographicSize camera=%p %.4f -> %.4f scale=%.4f screen=%dx%d",
                       self, (double)value, (double)patched, (double)scale, screen_w(), screen_h());
        }
        ((orig_float_void_t)g_camera_set_orthographic_size_orig)(self, patched, method);
    }

    static void tk2d_update_camera_matrix_hook(void* self, void* method) {
        static int before_count = 0;
        if (g_debug && before_count++ < 64)
            log_tk2d_camera_state("tk2dCamera.UpdateCameraMatrix.before", self);
        apply_tk2d_width_safe(self, "tk2dCamera.UpdateCameraMatrix");
        ((orig_void_t)g_tk2d_update_camera_matrix_orig)(self, method);
        static int after_count = 0;
        if (g_debug && after_count++ < 64)
            log_tk2d_camera_state("tk2dCamera.UpdateCameraMatrix.after", self);
    }

    static void resolve_field_offset(void* klass, const char* field, int* out) {
        if (!klass || !field || !out || !f_field_from_name || !f_field_get_offset) return;
        void* f = f_field_from_name(klass, field);
        if (f) *out = (int)f_field_get_offset(f);
    }

    static void install() {
        if (!g_enabled) return;
        void* asmImg = find_image("Assembly-CSharp");
        void* coreImg = find_image("UnityEngine.CoreModule");
        if (!asmImg || !coreImg) {
            g_api->log("HKVIEW", "image missing asm=%p core=%p", asmImg, coreImg);
            return;
        }

        resolve_call(coreImg, "UnityEngine", "Screen", "get_width", 0,
                     (void**)&g_screen_width, &g_mi_screen_width);
        resolve_call(coreImg, "UnityEngine", "Screen", "get_height", 0,
                     (void**)&g_screen_height, &g_mi_screen_height);
        resolve_call(coreImg, "UnityEngine", "Camera", "set_rect_Injected", 1,
                     (void**)&g_camera_set_rect_injected, &g_mi_camera_set_rect_injected);
        resolve_call(coreImg, "UnityEngine", "Camera", "get_orthographicSize", 0,
                     (void**)&g_camera_get_orthographic_size, &g_mi_camera_get_orthographic_size);
        void* set_ortho_ptr = nullptr;
        resolve_call(coreImg, "UnityEngine", "Camera", "set_orthographicSize", 1,
                     &set_ortho_ptr, &g_mi_camera_set_orthographic_size);
        g_api->log("HKVIEW", "force_full_viewport=%d camera.set_rect_Injected=%p",
                   g_force_full_viewport ? 1 : 0, (void*)g_camera_set_rect_injected);
        if (g_force_full_viewport) {
            resolve_method(coreImg, "UnityEngine", "Camera", "set_rect_Injected", 1,
                           (uintptr_t)&camera_set_rect_injected_hook,
                           &g_camera_set_rect_injected_orig);
        }
        if (g_ui_orthographic_width_safe) {
            resolve_method(coreImg, "UnityEngine", "Camera", "set_orthographicSize", 1,
                           (uintptr_t)&camera_set_orthographic_size_hook,
                           &g_camera_set_orthographic_size_orig);
        }

        void* tk2d = f_class_from_name(asmImg, "", "tk2dCamera");
        if (tk2d) {
            resolve_field_offset(tk2d, "cameraSettings", &g_tk2d_cam_off.cameraSettings);
            resolve_field_offset(tk2d, "nativeResolutionWidth", &g_tk2d_cam_off.nativeResolutionWidth);
            resolve_field_offset(tk2d, "nativeResolutionHeight", &g_tk2d_cam_off.nativeResolutionHeight);
            resolve_field_offset(tk2d, "_unityCamera", &g_tk2d_cam_off.unityCamera);
            resolve_field_offset(tk2d, "_targetResolution", &g_tk2d_cam_off.targetResolution);
            resolve_field_offset(tk2d, "zoomFactor", &g_tk2d_cam_off.zoomFactor);
            resolve_field_offset(tk2d, "_screenExtents", &g_tk2d_cam_off.screenExtents);
            resolve_field_offset(tk2d, "_nativeScreenExtents", &g_tk2d_cam_off.nativeScreenExtents);
            g_api->log("HKVIEW", "tk2dCamera fields settings=0x%x native=%x/%x unity=0x%x target=0x%x zoom=0x%x ext=0x%x nativeExt=0x%x",
                       g_tk2d_cam_off.cameraSettings,
                       g_tk2d_cam_off.nativeResolutionWidth,
                       g_tk2d_cam_off.nativeResolutionHeight,
                       g_tk2d_cam_off.unityCamera,
                       g_tk2d_cam_off.targetResolution,
                       g_tk2d_cam_off.zoomFactor,
                       g_tk2d_cam_off.screenExtents,
                       g_tk2d_cam_off.nativeScreenExtents);
        }
        void* tk2d_settings = f_class_from_name(asmImg, "", "tk2dCameraSettings");
        if (tk2d_settings) {
            resolve_field_offset(tk2d_settings, "orthographicSize", &g_tk2d_settings_off.orthographicSize);
            resolve_field_offset(tk2d_settings, "orthographicPixelsPerMeter", &g_tk2d_settings_off.orthographicPixelsPerMeter);
            resolve_field_offset(tk2d_settings, "orthographicType", &g_tk2d_settings_off.orthographicType);
            resolve_field_offset(tk2d_settings, "rect", &g_tk2d_settings_off.rect);
            g_api->log("HKVIEW", "tk2dCameraSettings fields ortho=0x%x ppm=0x%x type=0x%x rect=0x%x",
                       g_tk2d_settings_off.orthographicSize,
                       g_tk2d_settings_off.orthographicPixelsPerMeter,
                       g_tk2d_settings_off.orthographicType,
                       g_tk2d_settings_off.rect);
        }
        if (tk2d && (g_tk2d_width_safe || g_debug)) {
            resolve_method(asmImg, "", "tk2dCamera", "UpdateCameraMatrix", 0,
                           (uintptr_t)&tk2d_update_camera_matrix_hook,
                           &g_tk2d_update_camera_matrix_orig);
        }

        void* gs = f_class_from_name(asmImg, "", "GameSettings");
        if (gs && f_field_from_name && f_field_get_offset) {
            void* f = f_field_from_name(gs, "overScanAdjustment");
            if (f) g_gs_overscan_adjustment_off = (int)f_field_get_offset(f);
            f = f_field_from_name(gs, "overscanAdjusted");
            if (f) g_gs_overscan_adjusted_off = (int)f_field_get_offset(f);
            g_api->log("HKVIEW", "GameSettings fields overScanAdjustment=0x%x overscanAdjusted=0x%x",
                       g_gs_overscan_adjustment_off, g_gs_overscan_adjusted_off);
        }

        resolve_method(asmImg, "", "ForceCameraAspect", "SetOverscanViewport", 1,
                       (uintptr_t)&force_aspect_set_overscan_hook,
                       &g_force_aspect_set_overscan_orig);
        resolve_method(asmImg, "", "ForceCameraAspect", "AutoScaleViewport", 0,
                       (uintptr_t)&force_aspect_auto_scale_hook,
                       &g_force_aspect_auto_scale_orig);
        resolve_method(asmImg, "", "GameCameras", "SetOverscan", 1,
                       (uintptr_t)&game_cameras_set_overscan_hook,
                       &g_game_cameras_set_overscan_orig);
        resolve_method(asmImg, "", "GameSettings", "LoadOverscanSettings", 0,
                       (uintptr_t)&game_settings_load_overscan_hook,
                       &g_gs_load_overscan_orig);
        resolve_method(asmImg, "", "GameSettings", "SaveOverscanSettings", 0,
                       (uintptr_t)&game_settings_save_overscan_hook,
                       &g_gs_save_overscan_orig);
    }

    static void post_init(void*) {
        install();
    }

    static int init(BogoSoModule* lil2cpp) {
        g_mod = lil2cpp;
        g_debug = cfg_bool("debug");
        g_enabled = cfg_bool("enabled", 1);
        g_force_full_viewport = cfg_bool("force_full_viewport", 1);
        g_tk2d_width_safe = cfg_bool("tk2d_width_safe", 1);
        g_ui_orthographic_width_safe = cfg_bool("ui_orthographic_width_safe", 1);
        if (!g_enabled) return 0;

        f_domain_get = (p_domain_get)g_api->so_symbol(lil2cpp, "il2cpp_domain_get");
        f_domain_assemblies = (p_domain_get_assemblies)g_api->so_symbol(lil2cpp, "il2cpp_domain_get_assemblies");
        f_assembly_image = (p_assembly_get_image)g_api->so_symbol(lil2cpp, "il2cpp_assembly_get_image");
        f_image_name = (p_image_get_name)g_api->so_symbol(lil2cpp, "il2cpp_image_get_name");
        f_class_from_name = (p_class_from_name)g_api->so_symbol(lil2cpp, "il2cpp_class_from_name");
        f_get_method = (p_get_method)g_api->so_symbol(lil2cpp, "il2cpp_class_get_method_from_name");
        f_field_from_name = (p_field_from_name)g_api->so_symbol(lil2cpp, "il2cpp_class_get_field_from_name");
        f_field_get_offset = (p_field_get_offset)g_api->so_symbol(lil2cpp, "il2cpp_field_get_offset");
        if (!f_domain_get || !f_domain_assemblies || !f_assembly_image || !f_image_name ||
            !f_class_from_name || !f_get_method) {
            g_api->log("HKVIEW", "missing il2cpp exports; disabled");
            g_enabled = false;
            return 0;
        }
        if (!g_api->register_il2cpp_post_init(&post_init, nullptr)) {
            g_api->log("HKVIEW", "failed to register post-init callback");
            g_enabled = false;
            return 0;
        }
        g_api->log("HKVIEW", "plugin armed: full_viewport=%d tk2d_width_safe=%d debug=%d",
                   g_force_full_viewport ? 1 : 0,
                   g_tk2d_width_safe ? 1 : 0,
                   g_debug ? 1 : 0);
        return 0;
    }
}

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api) {
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION || !api->il2cpp)
        return -1;
    hollow_knight_viewport::g_api_storage = *api;
    hollow_knight_viewport::g_api = &hollow_knight_viewport::g_api_storage;
    return hollow_knight_viewport::init(api->il2cpp);
}
