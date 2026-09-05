#include "plugin_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace samurai2_offline {
static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = &g_api_storage;
static char g_callback_object[128] = "GooglePlayGames";

static BogoJniValue empty_value()
{
    BogoJniValue value = {};
    return value;
}

static const char* string_argument(void* env, const BogoJniValue* args,
                                   uint32_t index, uint32_t count,
                                   char* output, uint32_t output_size)
{
    if (!args || index >= count || !args[index].l || !output || !output_size)
        return "<null>";
    if (!g_api->jni_string_utf8(env, args[index].l, output, output_size))
        return "";
    return output;
}

static void send_unity_callback(const char* method, const char* value)
{
    using UnitySendMessageFn = void (*)(const char*, const char*, const char*);
    auto send = reinterpret_cast<UnitySendMessageFn>(
        g_api->so_symbol(nullptr, "UnitySendMessage"));
    if (!send) {
        g_api->log("SAMURAI2", "%s callback not sent: UnitySendMessage unavailable", method);
        return;
    }
    g_api->log("SAMURAI2", "UnitySendMessage('%s', '%s', '%s')",
               g_callback_object, method, value);
    send(g_callback_object, method, value);
}

static BogoJniValue api_connect(void*, void*, const BogoJniValue*, uint32_t, void*)
{
    g_api->log("SAMURAI2", "API.connect: offline, leaving player disconnected");
    send_unity_callback("_OnConnect", "false");
    return empty_value();
}

static BogoJniValue api_disconnect(void*, void*, const BogoJniValue*, uint32_t, void*)
{
    g_api->log("SAMURAI2", "API.disconnect");
    return empty_value();
}

static BogoJniValue return_false(void*, void*, const BogoJniValue*, uint32_t, void* userdata)
{
    g_api->log("SAMURAI2", "%s: false", static_cast<const char*>(userdata));
    BogoJniValue value = {};
    value.z = 0;
    return value;
}

static BogoJniValue resolve_availability(void*, void*, const BogoJniValue*, uint32_t, void*)
{
    g_api->log("SAMURAI2", "API.resolvePlayServicesAvailabilityError: unavailable offline");
    send_unity_callback("_OnResolvePlayServicesAvailability", "false");
    return empty_value();
}

static BogoJniValue callbacks_init(void* env, void*, const BogoJniValue* args,
                                   uint32_t count, void*)
{
    char value[sizeof(g_callback_object)] = {};
    const char* object = string_argument(env, args, 0, count, value, sizeof(value));
    if (object[0] && std::strcmp(object, "<null>") != 0) {
        std::strncpy(g_callback_object, object, sizeof(g_callback_object) - 1);
        g_callback_object[sizeof(g_callback_object) - 1] = '\0';
    }
    g_api->log("SAMURAI2", "UnityCallbacks.init: game object='%s'", object);
    return empty_value();
}

static BogoJniValue activity_result(void*, void*, const BogoJniValue* args,
                                    uint32_t count, void*)
{
    const int request = args && count > 0 ? args[0].i : 0;
    const int result = args && count > 1 ? args[1].i : 0;
    g_api->log("SAMURAI2", "UnityPlayerActivityProxy.onActivityResult: request=%d result=%d",
               request, result);
    return empty_value();
}

static BogoJniValue ignored(void* env, void*, const BogoJniValue* args,
                            uint32_t count, void* userdata)
{
    char value[160] = {};
    const char* method = static_cast<const char*>(userdata);
    if (count && args && args[0].l) {
        const char* id = string_argument(env, args, 0, count, value, sizeof(value));
        g_api->log("SAMURAI2", "%s('%s') ignored while offline", method, id);
    } else {
        g_api->log("SAMURAI2", "%s ignored while offline", method);
    }
    return empty_value();
}

static BogoJniValue ignored_string_int(void* env, void*, const BogoJniValue* args,
                                       uint32_t count, void* userdata)
{
    char value[160] = {};
    const char* method = static_cast<const char*>(userdata);
    const char* id = string_argument(env, args, 0, count, value, sizeof(value));
    const int amount = args && count > 1 ? args[1].i : 0;
    g_api->log("SAMURAI2", "%s('%s', %d) ignored while offline", method, id, amount);
    return empty_value();
}

static BogoJniValue ignored_string_long(void* env, void*, const BogoJniValue* args,
                                        uint32_t count, void* userdata)
{
    char value[160] = {};
    const char* method = static_cast<const char*>(userdata);
    const char* id = string_argument(env, args, 0, count, value, sizeof(value));
    const long long score = args && count > 1 ? static_cast<long long>(args[1].j) : 0;
    g_api->log("SAMURAI2", "%s('%s', %lld) ignored while offline", method, id, score);
    return empty_value();
}

static BogoJniValue return_empty_string(void* env, void*, const BogoJniValue*,
                                        uint32_t, void*)
{
    BogoJniValue value = {};
    value.l = g_api->jni_new_string_utf8(env, "");
    return value;
}

#define STATIC_METHOD(name, signature, callback, userdata) \
    {name, signature, BOGO_JNI_METHOD_STATIC, callback, userdata}

static const BogoJniMethod api_methods[] = {
    STATIC_METHOD("connect", "()V", &api_connect, nullptr),
    STATIC_METHOD("disconnect", "()V", &api_disconnect, nullptr),
    STATIC_METHOD("isConnected", "()Z", &return_false, (void*)"API.isConnected"),
    STATIC_METHOD("isPlayServicesAvailable", "()Z", &return_false, (void*)"API.isPlayServicesAvailable"),
    STATIC_METHOD("resolvePlayServicesAvailabilityError", "()V", &resolve_availability, nullptr),
};

static const BogoJniMethod callback_methods[] = {
    STATIC_METHOD("init", "(Ljava/lang/String;)V", &callbacks_init, nullptr),
};

static const BogoJniMethod activity_methods[] = {
    STATIC_METHOD("onActivityResult", "(IILandroid/content/Intent;)V", &activity_result, nullptr),
};

static const BogoJniMethod achievement_methods[] = {
    STATIC_METHOD("load", "()V", &ignored, (void*)"Achievements.load"),
    STATIC_METHOD("reveal", "(Ljava/lang/String;)V", &ignored, (void*)"Achievements.reveal"),
    STATIC_METHOD("unlock", "(Ljava/lang/String;)V", &ignored, (void*)"Achievements.unlock"),
    STATIC_METHOD("increment", "(Ljava/lang/String;I)V", &ignored_string_int, (void*)"Achievements.increment"),
    STATIC_METHOD("set", "(Ljava/lang/String;I)V", &ignored_string_int, (void*)"Achievements.set"),
    STATIC_METHOD("showUI", "()V", &ignored, (void*)"Achievements.showUI"),
};

static const BogoJniMethod leaderboard_methods[] = {
    STATIC_METHOD("showLeaderboardsIntent", "()V", &ignored, (void*)"Leaderboards.showLeaderboardsIntent"),
    STATIC_METHOD("showLeaderboardsIntent", "(Ljava/lang/String;)V", &ignored, (void*)"Leaderboards.showLeaderboardsIntent"),
    STATIC_METHOD("submitScore", "(Ljava/lang/String;J)V", &ignored_string_long, (void*)"Leaderboards.submitScore"),
};

static const BogoJniMethod player_methods[] = {
    STATIC_METHOD("getDisplayName", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getHiResImgUri", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getIconUri", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getName", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getPlayerId", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getTitle", "()Ljava/lang/String;", &return_empty_string, nullptr),
};

static const BogoJniMethod google_methods[] = {
    STATIC_METHOD("getAccessToken", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getAccountId", "()Ljava/lang/String;", &return_empty_string, nullptr),
    STATIC_METHOD("getServerAuthCode", "()Ljava/lang/String;", &return_empty_string, nullptr),
};

static const BogoJniMethod recording_methods[] = {
    STATIC_METHOD("isCaptureSupported", "()Z", &return_false, (void*)"Recording.isCaptureSupported"),
    STATIC_METHOD("isCapturing", "()Z", &return_false, (void*)"Recording.isCapturing"),
    STATIC_METHOD("isOverlayVisible", "()Z", &return_false, (void*)"Recording.isOverlayVisible"),
    STATIC_METHOD("showRecordingIntent", "()V", &ignored, (void*)"Recording.showRecordingIntent"),
};

#undef STATIC_METHOD

template<size_t N>
static bool register_class(const char* name, const BogoJniMethod (&methods)[N])
{
    return g_api->register_jni_class(name, methods, static_cast<uint32_t>(N)) != 0;
}

static void jni_init(void*, void*)
{
    bool ok = true;
    ok &= register_class("com/madfingergames/googleplaygames/API", api_methods);
    ok &= register_class("com/madfingergames/googleplaygames/UnityCallbacks", callback_methods);
    ok &= register_class("com/madfingergames/googleplaygames/UnityPlayerActivityProxy", activity_methods);
    ok &= register_class("com/madfingergames/googleplaygames/Achievements", achievement_methods);
    ok &= register_class("com/madfingergames/googleplaygames/Leaderboards", leaderboard_methods);
    ok &= register_class("com/madfingergames/googleplaygames/Player", player_methods);
    ok &= register_class("com/madfingergames/googleplaygames/Google", google_methods);
    ok &= register_class("com/madfingergames/googleplaygames/Recording", recording_methods);
    g_api->log("SAMURAI2", "Madfinger JNI classes registered: %s", ok ? "ok" : "failed");
}

static int init(const BogoPluginApi* api)
{
    g_api_storage = *api;
    const bool offline = g_api->config_get_bool("google_play.offline", 0) != 0;
    if (!offline) {
        g_api->log("SAMURAI2", "offline Google Play compatibility disabled");
        return 0;
    }
    if (!g_api->register_jni_init(&jni_init, nullptr)) {
        g_api->log("SAMURAI2", "failed to register JNI init callback");
        return -1;
    }
    g_api->log("SAMURAI2", "plugin armed: offline_google_play=1");
    return 0;
}
}

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(BogoPluginApi) || !api->jvm || api->il2cpp ||
        !api->register_jni_init || !api->register_jni_class ||
        !api->jni_string_utf8 || !api->jni_new_string_utf8 || !api->so_symbol ||
        !api->config_get_bool || !api->log)
        return -1;
    return samurai2_offline::init(api);
}
