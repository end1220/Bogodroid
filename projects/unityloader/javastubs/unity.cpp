#include "unity.h"
#include "../globals.h"
#include "baron/baron.h"
#include "logging.h"
#include "jnibridge.h"
#include "toml++/toml.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern toml::table config;

namespace {

// BD_CTOR_FALLBACK_NULL=1 restores the pre-fallback behaviour where an
// unbound <init> made ReflectionHelper::getConstructorID() hand back an empty
// wrapper (construction -> null). Only for A/B debugging.
bool bd_ctor_fallback_null()
{
    const char* v = ::getenv("BD_CTOR_FALLBACK_NULL");
    return v && *v && ::strcmp(v, "0") != 0;
}

bool pad_enabled()
{
    return config["play_asset_delivery"]["enabled"].value_or(false);
}

std::string pad_default_pack()
{
    return config["play_asset_delivery"]["default_pack"].value_or<std::string>("");
}

std::string pad_version()
{
    return config["play_asset_delivery"]["pack_version"].value_or<std::string>("");
}

// Template uses {pack} and {ver}; relative to android_files (conf).
// Example: "assetpacks/{pack}/{ver}/{ver}/assets"
std::string pad_path_template()
{
    return config["play_asset_delivery"]["pack_path_template"].value_or<std::string>(
        "assetpacks/{pack}/{ver}/{ver}/assets");
}

std::string expand_pad_template(const std::string& tmpl,
                                const std::string& pack,
                                const std::string& ver)
{
    std::string out = tmpl;
    auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(out, "{pack}", pack);
    replace_all(out, "{ver}", ver);
    return out;
}

std::shared_ptr<FakeJni::JString> pad_name_or_default(
    std::shared_ptr<FakeJni::JString> name)
{
    if (name)
        return name;
    const std::string def = pad_default_pack();
    return std::make_shared<FakeJni::JString>(def.c_str());
}

} // namespace

///// UnityPlayer
std::shared_ptr<jnivm::com::unity3d::player::UnityPlayerActivity> jnivm::com::unity3d::player::UnityPlayer::currentActivity = nullptr;

static bool g_soft_input_active = false;
static std::string g_soft_input_text;
static bool is_soft_input_confirm_key(int code);
static bool is_soft_input_cancel_key(int code);
static void submit_soft_input(const char* source, std::shared_ptr<FakeJni::JString> requested);
static void cancel_soft_input(const char* source);

bool jnivm::com::unity3d::player::UnityPlayerActivity::injectEvent(std::shared_ptr<android::view::InputEvent> event)
{
    // This is the C++ equivalent of the `mUnityPlayer.injectEvent(event)` call,
    // which in turn calls the native function.

    verbose("UnityPlayerActivity", "Injecting input event into native engine.");

    FakeJni::LocalFrame frame(vm);

    auto unityPlayerClass = vm.findClass("com/unity3d/player/UnityPlayer").get();
    if (!unityPlayerClass) {
        verbose("UnityPlayerActivity", "Could not find class com/unity3d/player/UnityPlayer");
        return false;
    }

    // Unity 2021: nativeInjectEvent(InputEvent)Z
    // Unity 2022.3: nativeInjectEvent(InputEvent, int)Z  (extra deviceId; loaders pass 0)
    static int inject_nargs = -1;
    static jnivm::MethodProxy inject_method(nullptr, nullptr, nullptr);
    if (inject_nargs < 0) {
        auto m2022 = unityPlayerClass->getMethod("(Landroid/view/InputEvent;I)Z", "nativeInjectEvent");
        if (m2022) {
            inject_method = m2022;
            inject_nargs = 2;
            BD_LOG("INPUT", "nativeInjectEvent (Landroid/view/InputEvent;I)Z");
        } else {
            auto m2021 = unityPlayerClass->getMethod("(Landroid/view/InputEvent;)Z", "nativeInjectEvent");
            if (m2021) {
                inject_method = m2021;
                inject_nargs = 1;
                BD_LOG("INPUT", "nativeInjectEvent (Landroid/view/InputEvent;)Z");
            } else {
                inject_nargs = 0;
                verbose("UnityPlayerActivity", "Could not find native method nativeInjectEvent");
            }
        }
    }
    if (inject_nargs <= 0)
        return false;

    auto key = std::dynamic_pointer_cast<jnivm::android::view::KeyEvent>(event);
    if (g_soft_input_active && key) {
        int code = key->getKeyCode();
        bool confirm_soft_input = is_soft_input_confirm_key(code);
        bool cancel = is_soft_input_cancel_key(code);
        if (key->getAction() == jnivm::android::view::KeyEvent::ACTION_UP && confirm_soft_input) {
            submit_soft_input("key-confirm", nullptr);
            return true;
        }
        if (key->getAction() == jnivm::android::view::KeyEvent::ACTION_UP && cancel) {
            cancel_soft_input("key-cancel");
            return true;
        }
        verbose("UnityPlayerActivity", "soft input swallowed key code=%d action=%d", code, key->getAction());
        return true;
    }

    jboolean result = JNI_FALSE;
    if (inject_nargs == 2)
        result = inject_method.invoke(frame.getJniEnv(), unityPlayerClass, event, (FakeJni::JInt)0).z;
    else
        result = inject_method.invoke(frame.getJniEnv(), unityPlayerClass, event).z;
    if (key) {
        BD_LOG("INPUT", "nativeInjectEvent keycode=%d action=%d device=%d result=%d",
               key->getKeyCode(), key->getAction(),
               key->getDeviceId(), result ? 1 : 0);
    }
    verbose("UnityPlayerActivity", "Result: %d", result);
    return result == JNI_TRUE;
}

///// PlayAssetDeliveryUnityWrapper

void jnivm::com::unity3d::player::IAssetPackManagerStatusQueryCallback::onStatusResult(
    FakeJni::JLong, std::shared_ptr<FakeJni::JArray<FakeJni::JString>>,
    std::shared_ptr<FakeJni::JIntArray>, std::shared_ptr<FakeJni::JIntArray>)
{
}

void jnivm::com::unity3d::player::IAssetPackManagerDownloadStatusCallback::onStatusUpdate(
    std::shared_ptr<FakeJni::JString>, FakeJni::JInt, FakeJni::JLong,
    FakeJni::JLong, FakeJni::JInt, FakeJni::JInt)
{
}

void jnivm::com::unity3d::player::IAssetPackManagerMobileDataConfirmationCallback::onMobileDataConfirmationResult(
    FakeJni::JBoolean)
{
}

void jnivm::com::unity3d::player::UnityCoreAssetPacksStatusCallbacks::report(
    std::shared_ptr<FakeJni::JString> name, FakeJni::JInt status, FakeJni::JInt error)
{
    BD_LOG("PAD", "status result name=%s status=%d error=%d",
           name ? name->c_str() : "(null)", (int)status, (int)error);
    auto callbacks = vm.findClass("com/unity3d/player/UnityCoreAssetPacksStatusCallbacks");
    if (!callbacks) {
        BD_LOG("PAD", "status result dropped: callback class not found");
        return;
    }
    auto method = callbacks->getMethod("(Ljava/lang/String;II)V", "nativeStatusQueryResult");
    if (!method) {
        BD_LOG("PAD", "nativeStatusQueryResult method is not registered");
        return;
    }
    FakeJni::LocalFrame frame(vm);
    BD_LOG("PAD", "calling nativeStatusQueryResult");
    method.invoke(frame.getJniEnv(), this, name, status, error);
}

void jnivm::com::unity3d::player::UnityCoreAssetPacksStatusCallbacks::onStatusResult(
    FakeJni::JLong sequence, std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<FakeJni::JIntArray> statuses, std::shared_ptr<FakeJni::JIntArray> errors)
{
    (void)sequence;
    BD_LOG("PAD", "onStatusResult count=%d", names ? names->getSize() : 0);
    if (!names || !statuses || !errors)
        return;
    const int count = std::min({ names->getSize(), statuses->getSize(), errors->getSize() });
    for (int i = 0; i < count; ++i)
        report((*names)[i], (*statuses)[i], (*errors)[i]);
}

void jnivm::com::unity3d::player::UnityCoreAssetPacksStatusCallbacks::onStatusUpdate(
    std::shared_ptr<FakeJni::JString> name, FakeJni::JInt status,
    FakeJni::JLong transferred, FakeJni::JLong total, FakeJni::JInt error,
    FakeJni::JInt errorCode)
{
    BD_LOG("PAD", "onStatusUpdate name=%s status=%d transferred=%lld total=%lld error=%d errorCode=%d",
           name ? name->c_str() : "(null)", (int)status,
           (long long)transferred, (long long)total, (int)error, (int)errorCode);
    report(name, status, errorCode ? errorCode : error);
}

std::shared_ptr<jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper>
    jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::instance = nullptr;

std::shared_ptr<jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper> jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::init(std::shared_ptr<jnivm::android::content::Context> context)
{
    if (!instance)
        instance = std::make_shared<jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper>();
    verbose("PlayAssetDeliveryUnityWrapper", "init() -> %p", instance.get());
    return instance;
}

std::shared_ptr<jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper>
jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::getInstance()
{
    if (!instance)
        instance = std::make_shared<jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper>();
    verbose("PlayAssetDeliveryUnityWrapper", "getInstance() -> %p", instance.get());
    return instance;
}

bool jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::playCoreApiMissing()
{
    const bool missing = !pad_enabled();
    verbose("PlayAssetDeliveryUnityWrapper", "playCoreApiMissing() -> %s",
            missing ? "true" : "false (PAD shim)");
    return missing;
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::cancelAssetPackDownload(
    std::shared_ptr<FakeJni::JString> name)
{
    auto names = std::make_shared<FakeJni::JArray<FakeJni::JString>>(1);
    (*names)[0] = pad_name_or_default(name);
    cancelAssetPackDownloads(names);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::cancelAssetPackDownloads(
    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names)
{
    verbose("PlayAssetDeliveryUnityWrapper", "cancelAssetPackDownloads(%d)", names ? names->getSize() : 0);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::downloadAssetPack(
    std::shared_ptr<FakeJni::JString> name,
    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback)
{
    auto names = std::make_shared<FakeJni::JArray<FakeJni::JString>>(1);
    (*names)[0] = pad_name_or_default(name);
    downloadAssetPacks(names, callback);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::downloadAssetPacks(
    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback)
{
    verbose("PlayAssetDeliveryUnityWrapper", "downloadAssetPacks(%d) -> completed", names ? names->getSize() : 0);
    if (!callback || !pad_enabled())
        return;
    // An empty list is a normal success path when every custom pack is already
    // installed. AssetPackDownloader has called NotifyCustomPackDownloaded for
    // those packs before reaching Java; after this method returns it observes
    // the empty list and sets customAssetPacksDownloaded itself. Synthesizing a
    // status event here would report a pack that is no longer pending and make
    // the managed callback dereference a missing list/dictionary entry.
    if (!names || names->getSize() == 0) {
        BD_LOG("PAD", "empty download list; all custom packs already installed");
        return;
    }
    for (int i = 0; i < names->getSize(); ++i)
        callback->onStatusUpdate((*names)[i], 4, 1, 1, 0, 0);
}

std::shared_ptr<FakeJni::JString>
jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::getAssetPackPath(
    std::shared_ptr<FakeJni::JString> name)
{
    const std::string pack_name = name ? name->c_str() : std::string();
    verbose("PlayAssetDeliveryUnityWrapper", "getAssetPackPath(%s)",
            pack_name.empty() ? "(null)" : pack_name.c_str());

    // init_config changes cwd to paths.game_files (the port's gamedata
    // directory). Unity asks this Java method for both the core data-pack
    // directory and the custom PAD bundle locations, which are different
    // directories in the Linux port layout.
    const std::filesystem::path game_root = std::filesystem::current_path();
    std::string android_root = config["paths"]["android_files"].value_or<std::string>("../conf");
    std::filesystem::path custom_root = android_root.empty()
        ? game_root.parent_path() / "conf"
        : std::filesystem::path(android_root);
    if (custom_root.is_relative())
        custom_root = game_root / custom_root;
    custom_root = custom_root.lexically_normal();

    std::filesystem::path path;
    if (pack_name == "UnityDataAssetPack") {
        // Unity's MountDataArchive only accepts paths that contain
        // ".apk/", ".obb/", ".jar/", or ".zip/" (ZipCentralDirectory). A plain
        // directory like game_root fails with "Path ... was not parsed" and
        // never opens datapack.unity3d. Return an APK-style assets path whose
        // zip payload is staged next to gamedata as UnityDataAssetPack.apk.
        path = game_root / "UnityDataAssetPack.apk" / "assets";
    } else if (pack_name == "UnityStreamingAssetsPack") {
        // StreamingAssets live under assets/ in this port layout. Addressables
        // loads CustomAssetPacksData.json from
        // Application.streamingAssetsPath/CustomAssetPacksData.json, which is
        // derived from this pack path.
        path = game_root / "assets";
    } else if (pad_enabled()) {
        const std::string def_pack = pad_default_pack();
        const std::string ver = pad_version();
        const std::string pack = !def_pack.empty() ? def_pack : pack_name;
        std::string rel = expand_pad_template(pad_path_template(), pack, ver);
        if (pack_name.size() == 32 &&
            std::all_of(pack_name.begin(), pack_name.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            })) {
            path = custom_root / rel / (pack_name + ".bundle");
        } else {
            path = custom_root / rel;
        }
    } else {
        path = custom_root;
    }

    const std::string path_string = path.lexically_normal().string();
    BD_LOG("PAD", "getAssetPackPath(%s) -> %s",
           pack_name.empty() ? "(null)" : pack_name.c_str(), path_string.c_str());
    return std::make_shared<FakeJni::JString>(path_string);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::getAssetPackState(
    std::shared_ptr<FakeJni::JString> name,
    std::shared_ptr<IAssetPackManagerStatusQueryCallback> callback)
{
    auto names = std::make_shared<FakeJni::JArray<FakeJni::JString>>(1);
    (*names)[0] = pad_name_or_default(name);
    getAssetPackStates(names, callback);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::getAssetPackStates(
    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<IAssetPackManagerStatusQueryCallback> callback)
{
    verbose("PlayAssetDeliveryUnityWrapper", "getAssetPackStates(%d) -> completed", names ? names->getSize() : 0);
    if (!callback || !names || !pad_enabled())
        return;
    auto statuses = std::make_shared<FakeJni::JIntArray>(names->getSize());
    auto errors = std::make_shared<FakeJni::JIntArray>(names->getSize());
    for (int i = 0; i < names->getSize(); ++i) {
        BD_LOG("PAD", "getAssetPackStates[%d] name=%s status=4 error=0",
               i, (*names)[i] ? (*names)[i]->c_str() : "(null)");
        (*statuses)[i] = 4;
        (*errors)[i] = 0;
    }
    callback->onStatusResult(0, names, statuses, errors);
}

std::shared_ptr<jnivm::Object>
jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::registerDownloadStatusListener(
    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback)
{
    verbose("PlayAssetDeliveryUnityWrapper", "registerDownloadStatusListener(%p)", callback.get());
    return callback;
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::removeAssetPack(
    std::shared_ptr<FakeJni::JString> name)
{
    verbose("PlayAssetDeliveryUnityWrapper", "removeAssetPack(%s)", name ? name->c_str() : "(null)");
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::requestToUseMobileData(
    std::shared_ptr<jnivm::android::app::Activity>,
    std::shared_ptr<IAssetPackManagerMobileDataConfirmationCallback> callback)
{
    if (callback)
        callback->onMobileDataConfirmationResult(JNI_TRUE);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::unregisterDownloadStatusListener(
    std::shared_ptr<jnivm::Object> token)
{
    verbose("PlayAssetDeliveryUnityWrapper", "unregisterDownloadStatusListener(%p)", token.get());
}

///// UnityPlayer

bool jnivm::com::unity3d::player::UnityPlayer::initializeGoogleAr()
{
    return false; 
}

std::shared_ptr<FakeJni::JString> jnivm::com::unity3d::player::UnityPlayer::getLaunchURL()
{
    return std::make_shared<FakeJni::JString>("");
}

static bool env_enabled(const char* name)
{
    const char* value = getenv(name);
    return value && *value && strcmp(value, "0") != 0;
}

static std::string upper_ascii(std::string value)
{
    for (char& c : value) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    }
    return value;
}

static bool key_name_to_code(const std::string& raw, int* out)
{
    std::string name = upper_ascii(raw);
    if (name.rfind("KEYCODE_", 0) == 0)
        name = name.substr(8);

    if (name == "ENTER") *out = jnivm::android::view::KeyEvent::KEYCODE_ENTER;
    else if (name == "NUMPAD_ENTER") *out = jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_ENTER;
    else if (name == "DPAD_CENTER") *out = jnivm::android::view::KeyEvent::KEYCODE_DPAD_CENTER;
    else if (name == "BUTTON_A" || name == "A") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_A;
    else if (name == "BUTTON_B" || name == "B") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_B;
    else if (name == "BUTTON_X" || name == "X") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_X;
    else if (name == "BUTTON_Y" || name == "Y") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_Y;
    else if (name == "BUTTON_START" || name == "START") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_START;
    else if (name == "BUTTON_SELECT" || name == "SELECT") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_SELECT;
    else if (name == "BUTTON_R1" || name == "R1" || name == "RB") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_R1;
    else if (name == "BUTTON_L1" || name == "L1" || name == "LB") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_L1;
    else if (name == "BUTTON_THUMBR" || name == "THUMBR" || name == "R3") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_THUMBR;
    else if (name == "BUTTON_THUMBL" || name == "THUMBL" || name == "L3") *out = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_THUMBL;
    else if (name == "BACK") *out = jnivm::android::view::KeyEvent::KEYCODE_BACK;
    else if (name == "ESCAPE" || name == "ESC") *out = jnivm::android::view::KeyEvent::KEYCODE_ESCAPE;
    else return false;
    return true;
}

static bool key_list_contains(const std::string& list, int code)
{
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(" \t\r\n");
        size_t end = item.find_last_not_of(" \t\r\n");
        if (start == std::string::npos)
            continue;
        int parsed = jnivm::android::view::KeyEvent::KEYCODE_UNKNOWN;
        if (key_name_to_code(item.substr(start, end - start + 1), &parsed) && parsed == code)
            return true;
    }
    return false;
}

static std::string soft_input_key_list(const char* env_name, const char* toml_key, const char* fallback)
{
    const char* env = getenv(env_name);
    if (env && *env)
        return env;
    auto t = config["soft_input"].as_table();
    if (t)
        return (*t)[toml_key].value<std::string>().value_or(fallback);
    return fallback;
}

static bool is_soft_input_confirm_key(int code)
{
    return key_list_contains(
        soft_input_key_list("BD_SOFT_INPUT_CONFIRM_KEYS", "confirm_keys",
                            "ENTER,NUMPAD_ENTER,DPAD_CENTER,BUTTON_A,BUTTON_START,BUTTON_R1,BUTTON_THUMBR"),
        code);
}

static bool is_soft_input_cancel_key(int code)
{
    return key_list_contains(
        soft_input_key_list("BD_SOFT_INPUT_CANCEL_KEYS", "cancel_keys",
                            "BACK,ESCAPE,BUTTON_B,BUTTON_SELECT"),
        code);
}

static std::string soft_input_default_value()
{
    const char* value = getenv("BD_SOFT_INPUT_DEFAULT");
    if (value && *value)
        return value;
    auto t = config["soft_input"].as_table();
    if (t)
        return (*t)["default"].value<std::string>().value_or("");
    return "";
}

static bool soft_input_force_default()
{
    if (env_enabled("BD_SOFT_INPUT_FORCE_DEFAULT"))
        return true;
    auto t = config["soft_input"].as_table();
    return t && (*t)["force_default"].value<bool>().value_or(false);
}

static std::string soft_input_value(std::shared_ptr<FakeJni::JString> requested)
{
    std::string fallback = soft_input_default_value();
    if (soft_input_force_default() && !fallback.empty())
        return fallback;
    if (requested && requested->c_str() && requested->c_str()[0])
        return requested->c_str();
    if (!fallback.empty())
        return fallback;
    return "";
}

static bool call_unity_native_void(const char* name, const char* sig)
{
    try {
        FakeJni::LocalFrame frame(vm);
        auto unityPlayerClass = vm.findClass("com/unity3d/player/UnityPlayer").get();
        if (!unityPlayerClass)
            return false;
        auto method = unityPlayerClass->getMethod(sig, name);
        if (!method)
            return false;
        method.invoke(frame.getJniEnv(), unityPlayerClass);
        return true;
    } catch (const std::exception& e) {
        verbose("UnityPlayer", "%s%s failed: %s", name, sig, e.what());
        return false;
    } catch (...) {
        verbose("UnityPlayer", "%s%s failed", name, sig);
        return false;
    }
}

static bool call_unity_native_bool(const char* name, const char* sig, FakeJni::JBoolean value)
{
    try {
        FakeJni::LocalFrame frame(vm);
        auto unityPlayerClass = vm.findClass("com/unity3d/player/UnityPlayer").get();
        if (!unityPlayerClass)
            return false;
        auto method = unityPlayerClass->getMethod(sig, name);
        if (!method)
            return false;
        method.invoke(frame.getJniEnv(), unityPlayerClass, value);
        return true;
    } catch (const std::exception& e) {
        verbose("UnityPlayer", "%s%s failed: %s", name, sig, e.what());
        return false;
    } catch (...) {
        verbose("UnityPlayer", "%s%s failed", name, sig);
        return false;
    }
}

static bool call_unity_native_string(const char* name, const char* sig, const std::string& value)
{
    try {
        FakeJni::LocalFrame frame(vm);
        auto unityPlayerClass = vm.findClass("com/unity3d/player/UnityPlayer").get();
        if (!unityPlayerClass)
            return false;
        auto method = unityPlayerClass->getMethod(sig, name);
        if (!method)
            return false;
        auto text = std::make_shared<FakeJni::JString>(value);
        method.invoke(frame.getJniEnv(), unityPlayerClass, text);
        return true;
    } catch (const std::exception& e) {
        verbose("UnityPlayer", "%s%s failed: %s", name, sig, e.what());
        return false;
    } catch (...) {
        verbose("UnityPlayer", "%s%s failed", name, sig);
        return false;
    }
}

static bool call_unity_native_int2(const char* name, const char* sig, FakeJni::JInt a, FakeJni::JInt b)
{
    try {
        FakeJni::LocalFrame frame(vm);
        auto unityPlayerClass = vm.findClass("com/unity3d/player/UnityPlayer").get();
        if (!unityPlayerClass)
            return false;
        auto method = unityPlayerClass->getMethod(sig, name);
        if (!method)
            return false;
        method.invoke(frame.getJniEnv(), unityPlayerClass, a, b);
        return true;
    } catch (const std::exception& e) {
        verbose("UnityPlayer", "%s%s failed: %s", name, sig, e.what());
        return false;
    } catch (...) {
        verbose("UnityPlayer", "%s%s failed", name, sig);
        return false;
    }
}

static bool apply_soft_input_text(const std::string& value)
{
    bool set_ok = call_unity_native_string("nativeSetInputString", "(Ljava/lang/String;)V", value);
    bool selection_ok = call_unity_native_int2("nativeSetInputSelection", "(II)V",
                                               (FakeJni::JInt)value.size(), (FakeJni::JInt)value.size());
    verbose("UnityPlayer", "soft input text='%s' set=%d selection=%d",
            value.c_str(), set_ok ? 1 : 0, selection_ok ? 1 : 0);
    return set_ok;
}

static void submit_soft_input(const char* source, std::shared_ptr<FakeJni::JString> requested)
{
    std::string value = requested ? soft_input_value(requested) : g_soft_input_text;
    if (value.empty())
        value = soft_input_default_value();
    bool set_ok = apply_soft_input_text(value);
    bool visible_ok = call_unity_native_bool("nativeSetKeyboardIsVisible", "(Z)V", JNI_FALSE);
    bool close_ok = call_unity_native_void("nativeSoftInputClosed", "()V");
    apply_soft_input_text(value);
    g_soft_input_text = value;
    g_soft_input_active = false;
    verbose("UnityPlayer", "%s mocked soft input text='%s' set=%d visible=%d closed=%d",
            source, value.c_str(), set_ok ? 1 : 0, visible_ok ? 1 : 0, close_ok ? 1 : 0);
}

static void cancel_soft_input(const char* source)
{
    bool visible_ok = call_unity_native_bool("nativeSetKeyboardIsVisible", "(Z)V", JNI_FALSE);
    bool cancel_ok = call_unity_native_void("nativeSoftInputCanceled", "()V");
    if (!cancel_ok)
        cancel_ok = call_unity_native_void("nativeSoftInputClosed", "()V");
    g_soft_input_active = false;
    g_soft_input_text.clear();
    verbose("UnityPlayer", "%s mocked soft input cancel visible=%d canceled=%d",
            source, visible_ok ? 1 : 0, cancel_ok ? 1 : 0);
}

void jnivm::com::unity3d::player::UnityPlayer::hideSoftInput()
{
    verbose("UnityPlayer", "hideSoftInput()");
    g_soft_input_active = false;
    g_soft_input_text.clear();
    call_unity_native_bool("nativeSetKeyboardIsVisible", "(Z)V", JNI_FALSE);
}

void jnivm::com::unity3d::player::UnityPlayer::showSoftInput(
    std::shared_ptr<FakeJni::JString> text, FakeJni::JInt keyboardType,
    FakeJni::JBoolean autocorrection, FakeJni::JBoolean multiline,
    FakeJni::JBoolean secure, FakeJni::JBoolean alert,
    std::shared_ptr<FakeJni::JString> placeholder, FakeJni::JInt characterLimit,
    FakeJni::JBoolean reuseKeyboard, FakeJni::JBoolean hideInput)
{
    verbose("UnityPlayer", "showSoftInput(text='%s', type=%d, limit=%d, placeholder='%s')",
            text ? text->c_str() : "", keyboardType, characterLimit,
            placeholder ? placeholder->c_str() : "");
    g_soft_input_active = true;
    std::string value = soft_input_value(text);
    g_soft_input_text = value;
    bool set_ok = apply_soft_input_text(value);
    bool visible_ok = call_unity_native_bool("nativeSetKeyboardIsVisible", "(Z)V", JNI_TRUE);
    verbose("UnityPlayer", "showSoftInput prefilled text='%s' set=%d visible=%d",
            value.c_str(), set_ok ? 1 : 0, visible_ok ? 1 : 0);
}

void jnivm::com::unity3d::player::UnityPlayer::setSoftInputStr(std::shared_ptr<FakeJni::JString> text)
{
    g_soft_input_active = true;
    std::string value = soft_input_value(text);
    g_soft_input_text = value;
    bool set_ok = apply_soft_input_text(value);
    verbose("UnityPlayer", "setSoftInputStr prefilled text='%s' set=%d",
            value.c_str(), set_ok ? 1 : 0);
}

void jnivm::com::unity3d::player::UnityPlayer::setSoftInputStrWithAction(std::shared_ptr<FakeJni::JString> text, FakeJni::JInt action)
{
    verbose("UnityPlayer", "setSoftInputStr(action=%d)", action);
    submit_soft_input("setSoftInputStr(action)", text);
}

FakeJni::JInt jnivm::com::unity3d::player::UnityPlayer::getKeyboardLayout()
{
    verbose("UnityPlayer", "getKeyboardLayout()");
    return 0;
}

void jnivm::com::unity3d::player::UnityPlayer::startActivityIndicator(FakeJni::JInt unused)
{
    (void)unused;
}

void jnivm::com::unity3d::player::UnityPlayer::stopActivityIndicator()
{
}



///// ReflectionHelper

std::shared_ptr<jnivm::java::lang::reflect::Constructor> jnivm::com::unity3d::player::ReflectionHelper::getConstructorID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> signature)
{
    if (clazz == nullptr)
        return nullptr;
    auto ctor = std::make_shared<jnivm::java::lang::reflect::Constructor>(clazz, signature);

    // Keep the Constructor alive for the lifetime of the VM.
    //
    // Unlike getMethodID()/getFieldID(), which hand back non-owning shared_ptrs
    // into the Class's own methods/fields vectors, this one is freshly allocated
    // and its only owner would be the caller's local frame. jnivm clears that
    // frame on PopLocalFrame, but Unity keeps the pointer we return and uses it
    // as a jmethodID long afterwards.
    //
    // Note this is *not* about Constructor.newInstance(): an entry log confirmed
    // Unity never calls it (ODDMAR.md §4.4). Unity instead invokes the pointer
    // directly through the NewObject* slot, which reads name/signature/
    // nativehandle straight off this object -- so a destroyed object means a
    // dangling jmethodID, not just a failed lookup. An earlier version of this
    // comment blamed newInstance; the symptom that prompted it (a STUB-MISS whose
    // `Sig=` was heap garbage while `Member=<init>` still read back correctly)
    // is just what a partly-recycled std::string looks like -- "<init>" fits in
    // the small-string buffer and survives, the real signature lives on the heap
    // and does not.
    static std::mutex bd_ctor_mutex;
    static std::vector<std::shared_ptr<jnivm::java::lang::reflect::Constructor>> bd_ctor_keepalive;
    {
        std::lock_guard<std::mutex> lock(bd_ctor_mutex);
        bd_ctor_keepalive.push_back(ctor);
    }

    // [BD] Temporary (remove once the AssetLocator path is confirmed on
    // screen -- ODDMAR.md §4.2): dump what this class actually registered, so the
    // "bound ... -> ..." line below can be read against a known table.
    {
        std::lock_guard<std::mutex> lock(clazz->mtx);
        BD_LOG("JavaReflect", "getConstructorID: %s prefix='%s' methods=%zu",
               clazz->nativeprefix.c_str(), clazz->nativeprefix.c_str(),
               clazz->methods.size());
        for (auto& m : clazz->methods) {
            BD_LOG("JavaReflect", "   reg name='%s' sig='%s' static=%d native=%p handle=%d",
                   m->name.c_str(), m->signature.c_str(), (int)m->_static, m->native,
                   (int)(bool)m->nativehandle);
        }
    }

    // [BD] Give the wrapper an execution body.
    //
    // Unity takes the pointer returned here and uses it as a jmethodID for the
    // NewObject* slot, and jnivm dispatches that slot as a *static* call
    // (MDispatch<jobject, jclass>::CallMethod, see vm.cpp). That path can only
    // run a method through mid->dynamic or mid->nativehandle -- and a freshly
    // built reflect::Constructor has neither, because javac.cpp's constructor
    // fills in name and signature and nothing else. So every construction Unity
    // performed through this id fell into the "Unknown Static" branch and came
    // back null. That is why com.mobge.assetlocator.AssetLocator never came into
    // existence and ListAssets was never reached
    // (docs/ODDMAR.md P0; the mid printed there *is* this object, so
    // GetMethodID's <init> rewriting was never the missing piece).
    //
    // The class does carry the body: FakeJni registers every constructor as a
    // *static* Method whose signature ends in its own class -- "(args)L<class>;"
    // (ODDMAR.md §3.2). Resolve that entry and copy the execution body across.
    //
    // Bound here rather than at call time so the wrapper is self-contained:
    // nothing has to still be alive when Unity eventually invokes it.
    {
        std::lock_guard<std::mutex> lock(clazz->mtx);

        // Compare argument lists only. The lookup arrives as "(args)V" while the
        // registered entry carries the class as its return type, so the complete
        // signatures are never equal.
        const auto wantClose = ctor->signature.find(')');
        const std::string wantArgs = wantClose == std::string::npos
            ? ctor->signature
            : ctor->signature.substr(0, wantClose + 1);

        std::shared_ptr<Method> body;
        for (auto& m : clazz->methods) {
            if (m->name != "<init>" || (!m->nativehandle && !m->dynamic))
                continue;
            const auto close = m->signature.find(')');
            const std::string args = close == std::string::npos
                ? m->signature
                : m->signature.substr(0, close + 1);
            if (args == wantArgs) {
                body = m;
                break;
            }
        }

        // Nothing matched the argument list. Fall back to the first registered
        // constructor that actually has a body: Unity's signature strings are
        // routinely imprecise, and a constructor reached with the wrong static
        // argument type still beats no constructor at all (all entries funnel
        // into the same C++ constructor -- see javastubs/bd_assetlocator.cpp).
        if (!body) {
            for (auto& m : clazz->methods) {
                if (m->name == "<init>" && (m->nativehandle || m->dynamic)) {
                    body = m;
                    break;
                }
            }
        }

        if (body) {
            ctor->nativehandle = body->nativehandle;
            ctor->native = body->native;
            ctor->_static = true;
            BD_LOG("JavaReflect", "getConstructorID: bound %s%s -> %s%s",
                   clazz->nativeprefix.c_str(), ctor->signature.c_str(),
                   clazz->nativeprefix.c_str(), body->signature.c_str());
        } else if (bd_ctor_fallback_null()) {
            // Escape hatch for A/B runs: the old behaviour, kept so the
            // regression pair can still tell "the fallback caused it" from
            // "something else did".
            BD_LOG("JavaReflect",
                   "getConstructorID: no bound <init> on %s for %s"
                   " -- constructions through this id will return null"
                   " (BD_CTOR_FALLBACK_NULL)",
                   clazz->nativeprefix.c_str(), ctor->signature.c_str());
        } else {
            // A missing stub must not become a *null Java object*.
            //
            // Unity reaches this id through
            // _AndroidJNIHelper.GetConstructorID() and then runs it in the
            // NewObject* slot. With no bound <init> the construction came back
            // null, and managed code forwarded that null as an *argument* to
            // the next AndroidJavaObject. Unity then computes the signature of
            // each argument with obj.GetType() and dies with a
            // NullReferenceException inside
            // UnityEngine._AndroidJNIHelper.GetSignature -- which is exactly
            // what aborted
            // MobGe.ICloud.AndroidGooglePlayServiceCloudPlatform.get_androidClient()
            // / CheckAccountStatus() on Oddmar, because
            // com.mobge.unitygameintegration.SocialImpl is a class we never
            // stubbed (docs/ODDMAR.md).
            //
            // Java's `new` never yields null, so a class we never implemented
            // should still hand back a live, inert object of that class: every
            // call on it misses and returns a default, which the offline path
            // tolerates, while Unity's own reflection code keeps working.
            //
            // Cloned on a per-call basis rather than cached: the object is
            // pushed onto the caller's local frame by ToJNIType(), and sharing
            // one instance across frames would let an earlier pop collect it.
            ctor->_static = true;
            ctor->dynamic = [clazz](JNIEnv* env, jobject, jclass, const jvalue*) -> jvalue {
                auto obj = std::make_shared<jnivm::Object>();
                obj->clazz = clazz;
                jvalue value{};
                value.l = jnivm::JNITypes<std::shared_ptr<jnivm::Object>>::ToJNIType(
                    jnivm::ENV::FromJNIEnv(env), obj);
                return value;
            };
            BD_LOG("JavaReflect",
                   "getConstructorID: no bound <init> on %s for %s"
                   " -- synthesising an inert object",
                   clazz->nativeprefix.c_str(), ctor->signature.c_str());
        }
    }

    verbose("UnityReflection", "getConstructorID(%s, %s) = %p \n",
            clazz->getName().c_str(), signature ? signature->c_str() : "(null)", ctor.get());
    return ctor;
}

std::shared_ptr<jnivm::java::lang::reflect::Method> jnivm::com::unity3d::player::ReflectionHelper::getMethodID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> methodName, std::shared_ptr<FakeJni::JString> signature, bool isStatic)
{
    if(clazz == nullptr)
        return nullptr;

    const char* name = methodName.get()->c_str();
    const char* sig;

    // Method 0: [BD] exact signature match.
    //
    // Run this before the name-only search below. That search ignores the
    // caller's signature entirely, so a lookup for forName(String) would be
    // answered with whatever single method named "forName" is registered --
    // and if that method takes a different number of parameters, the caller
    // builds a shorter jvalue array than the hook reads, and the varargs
    // dispatch walks off the end of it. Matching the requested signature when
    // we actually have such a method keeps arity consistent.
    //
    // This is a preference, not a requirement: Unity's own signature strings
    // are often inaccurate (see the hardcoded table below), so a miss just
    // falls through to the old heuristics.
    if (signature != nullptr) {
        const char* wanted = signature.get()->c_str();
        if (wanted != nullptr && wanted[0] == '(') {
            auto exact = std::shared_ptr<Method>(
                (Method*)clazz->getMethod(wanted, name),
                [](Method*) { } // No-op deleter
            );
            if (exact != nullptr) {
                verbose("UnityReflection", "getMethodID(type 0/exact, %s, %s, %s, %d) = %p \n",
                        clazz->getName().c_str(), name, wanted, isStatic, exact.get());
                return exact;
            }
        }
    }

    // Method 1: Search for matching methods in class by name only (bail out if there is ambiguity due to duplicates)

    std::shared_ptr<Method> foundMethod=nullptr;
    bool duplicate=false;
    for(std::shared_ptr<Method> method : clazz.get()->methods)
    {
        if(strcmp(method->name.c_str(), name) == 0)
        {
            if(foundMethod != nullptr)
                duplicate=true;
            
            foundMethod=method;
        }
    }

    if(foundMethod != nullptr && !duplicate)
    {
        auto method = std::shared_ptr<Method>(
        (Method*)clazz->getMethod(foundMethod->signature.c_str(), name),
        [](Method*) { } // No-op deleter
        );
        // Log both the requested and the found signature: the found one is the
        // stub's, and comparing the two is the only way to tell whether we are
        // about to hand the caller a method with the wrong arity.
        verbose("UnityReflection", "getMethodID(type 1, %s, %s, requested=%s found=%s, %d) = %p \n",
                clazz->getName().c_str(), name,
                signature != nullptr ? signature.get()->c_str() : "(null)",
                foundMethod->signature.c_str(), isStatic, method.get());
        return method;
    }

    // Method 1b: same name-only search, but across the class *and its base
    // classes*.
    //
    // Method 1 only walks clazz->methods, i.e. what was registered directly on
    // the class. Unity's reflected lookups routinely name an inherited member
    // with an imprecise signature: getIntent() lives on android/app/Activity,
    // getPackageManager()/getAssets()/getObbDir() on android/content/Context,
    // while the lookup is done on com/unity3d/player/UnityPlayerActivity and
    // asks for them as `()Ljava/lang/Object;`. All three earlier strategies then
    // miss -- the exact pass wants the concrete return type, the own-class scan
    // sees no such name, and the hardcoded table below has no entry -- so
    // getMethodID returned null and the managed side failed with
    // "JNI: Init'd AndroidJavaObject with null ptr!" or a
    // NullReferenceException. Reusing the *registered* signature of the unique
    // name match is the same trick Method 1 uses, so argument arity stays
    // consistent with what the stub actually reads.
    if (foundMethod == nullptr || duplicate) {
        std::shared_ptr<Method> inherited = nullptr;
        bool inheritedDuplicate = false;
        if (jnivm::ENV* env = jnivm::ENV::FromJNIEnv(FakeJni::JniEnv::getCurrentEnv())) {
            std::vector<Class*> work{clazz.get()};
            for (size_t i = 0; i < work.size() && i < 64; ++i) {
                Class* current = work[i];
                if (current == nullptr)
                    continue;
                for (std::shared_ptr<Method>& method : current->methods) {
                    if (method != nullptr && strcmp(method->name.c_str(), name) == 0) {
                        if (inherited != nullptr && inherited != method)
                            inheritedDuplicate = true;
                        inherited = method;
                    }
                }
                if (current->baseclasses) {
                    for (std::shared_ptr<Class>& base : current->baseclasses(env)) {
                        if (base)
                            work.push_back(base.get());
                    }
                }
            }
        }
        if (inherited != nullptr && !inheritedDuplicate) {
            auto method = std::shared_ptr<Method>(
                (Method*)clazz->getMethod(inherited->signature.c_str(), name),
                [](Method*) { } // No-op deleter
            );
            if (method != nullptr) {
                verbose("UnityReflection", "getMethodID(type 1b/inherited, %s, %s, requested=%s found=%s, %d) = %p \n",
                        clazz->getName().c_str(), name,
                        signature != nullptr ? signature.get()->c_str() : "(null)",
                        inherited->signature.c_str(), isStatic, method.get());
                return method;
            }
        }
    }

    // Method 2: Hardcoded fixes for the signature inaccuracies, then use getMethod

    if (strcmp("initialize", name) == 0 && strcmp(clazz->getName().c_str(), "com/google/android/gms/games/PlayGamesSdk") == 0)
        sig = "(Landroid/content/Context;)V";
    else if (strcmp("getGamesSignInClient", name) == 0)
        sig = "(Landroid/app/Activity;)Lcom/google/android/gms/games/GamesSignInClient;";
    else if (strcmp("create", name) == 0 && strcmp(clazz->getName().c_str(), "com/google/android/play/core/review/ReviewManagerFactory") == 0)
        sig = "(Landroid/content/Context;)Lcom/google/android/play/core/review/ReviewManager;";
    else if ((strcmp("getFilesDir", name) == 0 || strcmp("getCacheDir", name) == 0 ||
              strcmp("getDataDir", name) == 0 || strcmp("getExternalCacheDir", name) == 0) &&
             strcmp(clazz->getName().c_str(), "com/unity3d/player/UnityPlayerActivity") == 0)
        sig = "()Ljava/io/File;";
    else if (strcmp("getExternalFilesDir", name) == 0 &&
             strcmp(clazz->getName().c_str(), "com/unity3d/player/UnityPlayerActivity") == 0)
        sig = "(Ljava/lang/String;)Ljava/io/File;";
    else if (strcmp("getClass", name) == 0)
        sig = "()Ljava/lang/Class;";
    else
        sig = signature.get()->c_str();

    auto method = std::shared_ptr<Method>(
        (Method*)clazz->getMethod(sig, name),
        [](Method*) { } // No-op deleter
    );
    verbose("UnityReflection", "getMethodID(type 2, %s, %s, %s, %d) = %p \n", clazz->getName().c_str(), name, sig, isStatic, method.get());
    return method;
}
std::shared_ptr<jnivm::java::lang::reflect::Field> jnivm::com::unity3d::player::ReflectionHelper::getFieldID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> fieldName, std::shared_ptr<FakeJni::JString> signature, bool isStatic)
{
    const char* name = fieldName.get()->c_str();
    const char* sig;

    if (strcmp("currentActivity", name) == 0 && strcmp(clazz->getName().c_str(), "com/unity3d/player/UnityPlayer") == 0)
        sig = "Lcom/unity3d/player/UnityPlayerActivity;";
    else if (strcmp("mUnityPlayer", name) == 0 &&
             strcmp(clazz->getName().c_str(), "com/unity3d/player/UnityPlayerActivity") == 0)
        // Unity's AndroidJNIHelper asks for Object, while the actual field is
        // declared as UnityPlayer in the Android UnityPlayerActivity class.
        sig = "Lcom/unity3d/player/UnityPlayer;";
    else if (strcmp("PressedStates", name) == 0 && strcmp(clazz->getName().c_str(), "com/unity3d/player/UnityPlayerActivity") == 0)
        sig = "[Z";
    else
        sig = signature.get()->c_str();

    for (auto field : clazz->fields) {
        if (field->name == name && field->type == sig) {
            verbose("UnityReflection", "getFieldID(%s, %s, %s, %d) = %p \n", clazz->getName().c_str(), fieldName.get()->c_str(), sig, isStatic, field.get());
            return field;
        }
    }

    verbose("UnityReflection", "getFieldID(%s, %s, %s, %d) = null \n", clazz->getName().c_str(), fieldName.get()->c_str(), sig, isStatic);
    return nullptr;
}

std::shared_ptr<FakeJni::JString> jnivm::com::unity3d::player::ReflectionHelper::getFieldSignature(std::shared_ptr<jnivm::java::lang::reflect::Field> field)
{
    if(field == nullptr)
        return nullptr;
    return std::make_shared<FakeJni::JString>(field->type);
}

std::shared_ptr<jnivm::Object> jnivm::com::unity3d::player::ReflectionHelper::newProxyInstance(std::shared_ptr<UnityPlayer> player, long nativeHandle, std::shared_ptr<jnivm::Class> interface)
{
    if (!interface) {
        BD_LOG("UnityReflection", "newProxyInstance(%p, %ld, null)", player.get(), nativeHandle);
        return nullptr;
    }

    const std::string interfaceName = interface->getName();
    verbose("UnityReflection", "newProxyInstance(%p, %ld, %s) \n", player.get(), nativeHandle, interfaceName.c_str());
    return std::make_shared<jnivm::bitter::jnibridge::JNIBridgeProxy>(
        nativeHandle, std::set<std::string>{interfaceName},
        jnivm::bitter::jnibridge::JNIBridgeProxy::InvocationMode::ManagedGCHandle);
}

// Two-argument form, see the declaration in unity.h.
//
// Unity's `_AndroidJNIHelper.CreateJNIArgArray()` builds the jvalue array for
// `new AndroidJavaObject(...)` by calling AndroidJavaProxy.GetProxy(), and that
// ends up here with (int proxyId, Class interfaceClass). With no such overload
// registered the lookup missed, GetProxy() returned IntPtr.Zero,
// AndroidJavaObjectDeleteLocalRef(0) was called on it, and the resulting
// exception aborted the caller -- on Oddmar that is
// MobGe.ICloud.AndroidGooglePlayServiceCloudPlatform.CheckAccountStatus(),
// reached from MGLOProgressData.construct() in MRLevelContext.Awake(), i.e. the
// save/progress system never came up.
//
// proxyId carries the same native handle the three-argument overload takes, so
// forward to it; the `player` argument is only used for a log line there.
std::shared_ptr<jnivm::Object> jnivm::com::unity3d::player::ReflectionHelper::newProxyInstanceById(
    jint proxyId, std::shared_ptr<jnivm::Class> interface)
{
    verbose("UnityReflection", "newProxyInstanceById(%d, %s) \n",
            (int)proxyId, interface ? interface->getName().c_str() : "(null)");
    return newProxyInstance(nullptr, (long)proxyId, interface);
}

std::shared_ptr<jnivm::Object> jnivm::com::unity3d::player::ReflectionHelper::createInvocationError(long nativeHandle, bool toggle)
{
    verbose("UnityReflection", "createInvocationError(%ld, %d)\n", nativeHandle, toggle);
    return std::make_shared<jnivm::com::unity3d::player::ReflectionHelper::InvocationError>(nativeHandle, toggle);
}

void jnivm::com::unity3d::player::ReflectionHelper::setNativeExceptionOnProxy(
    std::shared_ptr<jnivm::Object> proxy, long nativeHandle, bool hasException)
{
    // Unity uses this Java helper to associate native exception state with
    // its InvocationHandler. jnivm invokes the native callback synchronously,
    // so there is no Java-side handler state to update.
    verbose("UnityReflection", "setNativeExceptionOnProxy(%p, %ld, %d)\n",
            proxy.get(), nativeHandle, hasException);
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::IAssetPackManagerStatusQueryCallback) { FakeJni::Constructor<IAssetPackManagerStatusQueryCallback> {} },
    { FakeJni::Function<&IAssetPackManagerStatusQueryCallback::onStatusResult> {}, "onStatusResult", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::IAssetPackManagerDownloadStatusCallback) { FakeJni::Constructor<IAssetPackManagerDownloadStatusCallback> {} },
    { FakeJni::Function<&IAssetPackManagerDownloadStatusCallback::onStatusUpdate> {}, "onStatusUpdate", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::IAssetPackManagerMobileDataConfirmationCallback) { FakeJni::Constructor<IAssetPackManagerMobileDataConfirmationCallback> {} },
    { FakeJni::Function<&IAssetPackManagerMobileDataConfirmationCallback::onMobileDataConfirmationResult> {}, "onMobileDataConfirmationResult", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::UnityCoreAssetPacksStatusCallbacks) { FakeJni::Constructor<UnityCoreAssetPacksStatusCallbacks> {} },
    { FakeJni::Function<&UnityCoreAssetPacksStatusCallbacks::onStatusResult> {}, "onStatusResult", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityCoreAssetPacksStatusCallbacks::onStatusUpdate> {}, "onStatusUpdate", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper) { FakeJni::Constructor<PlayAssetDeliveryUnityWrapper> {} },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::init> {}, "init", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::getInstance> {}, "getInstance", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::playCoreApiMissing> {}, "playCoreApiMissing", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::cancelAssetPackDownload> {}, "cancelAssetPackDownload", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::cancelAssetPackDownloads> {}, "cancelAssetPackDownloads", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::downloadAssetPack> {}, "downloadAssetPack", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::downloadAssetPacks> {}, "downloadAssetPacks", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::getAssetPackPath> {}, "getAssetPackPath", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::getAssetPackState> {}, "getAssetPackState", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::getAssetPackStates> {}, "getAssetPackStates", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::registerDownloadStatusListener> {}, "registerDownloadStatusListener", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::removeAssetPack> {}, "removeAssetPack", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::requestToUseMobileData> {}, "requestToUseMobileData", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PlayAssetDeliveryUnityWrapper::unregisterDownloadStatusListener> {}, "unregisterDownloadStatusListener", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::UnityPlayerActivity) { FakeJni::Constructor<UnityPlayerActivity> {} },
    { FakeJni::Field<&UnityPlayerActivity::mUnityPlayer> {}, "mUnityPlayer", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&UnityPlayerActivity::MouseMode> {}, "MouseMode", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&UnityPlayerActivity::MouseInside> {}, "MouseInside", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&UnityPlayerActivity::PressedStates> {}, "PressedStates", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Function<&jnivm::android::app::Activity::getWindowManager> {}, "getWindowManager", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::UnityPlayer) { FakeJni::Constructor<UnityPlayer> {} },
    { FakeJni::Field<&UnityPlayer::currentActivity> {}, "currentActivity", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&UnityPlayer::initializeGoogleAr> {}, "initializeGoogleAr", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityPlayer::getLaunchURL> {}, "getLaunchURL", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityPlayer::hideSoftInput> {}, "hideSoftInput", FakeJni::JMethodID::PUBLIC },
    // Unity 2021 soft input: showSoftInput(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V
    { FakeJni::Function<&UnityPlayer::showSoftInput> {}, "showSoftInput", FakeJni::JMethodID::PUBLIC },
    // Unity 2021 soft input: setSoftInputStr(Ljava/lang/String;)V and setSoftInputStr(Ljava/lang/String;I)V
    { FakeJni::Function<&UnityPlayer::setSoftInputStr> {}, "setSoftInputStr", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityPlayer::setSoftInputStrWithAction> {}, "setSoftInputStr", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityPlayer::getKeyboardLayout> {}, "getKeyboardLayout", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityPlayer::startActivityIndicator> {}, "startActivityIndicator", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&UnityPlayer::stopActivityIndicator> {}, "stopActivityIndicator", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::ReflectionHelper) { FakeJni::Constructor<ReflectionHelper> {} },
    { FakeJni::Function<&ReflectionHelper::getConstructorID> {}, "getConstructorID", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::getMethodID> {}, "getMethodID", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::getFieldID> {}, "getFieldID", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::getFieldSignature> {}, "getFieldSignature", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::newProxyInstance> {}, "newProxyInstance", FakeJni::JMethodID::STATIC },
    // (int proxyId, Class) -- the overload Unity's AndroidJavaProxy.GetProxy()
    // actually resolves. Same JNI name, different signature; FakeJni keys its
    // registrations by name+signature, so both coexist.
    { FakeJni::Function<&ReflectionHelper::newProxyInstanceById> {}, "newProxyInstance", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::setNativeExceptionOnProxy> {}, "setNativeExceptionOnProxy", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::createInvocationError> {}, "createInvocationError", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::ReflectionHelper::InvocationError) { FakeJni::Constructor<InvocationError, long, bool> {} },
    END_NATIVE_DESCRIPTOR
