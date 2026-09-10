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
#include <sstream>
#include <string>

extern toml::table config;

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
    BD_LOG("SKULPAD", "status result name=%s status=%d error=%d",
           name ? name->c_str() : "(null)", (int)status, (int)error);
    auto callbacks = vm.findClass("com/unity3d/player/UnityCoreAssetPacksStatusCallbacks");
    if (!callbacks) {
        BD_LOG("SKULPAD", "status result dropped: callback class not found");
        return;
    }
    auto method = callbacks->getMethod("(Ljava/lang/String;II)V", "nativeStatusQueryResult");
    if (!method) {
        BD_LOG("SKULPAD", "nativeStatusQueryResult method is not registered");
        return;
    }
    FakeJni::LocalFrame frame(vm);
    BD_LOG("SKULPAD", "calling nativeStatusQueryResult");
    method.invoke(frame.getJniEnv(), this, name, status, error);
}

void jnivm::com::unity3d::player::UnityCoreAssetPacksStatusCallbacks::onStatusResult(
    FakeJni::JLong sequence, std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<FakeJni::JIntArray> statuses, std::shared_ptr<FakeJni::JIntArray> errors)
{
    (void)sequence;
    BD_LOG("SKULPAD", "onStatusResult count=%d", names ? names->getSize() : 0);
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
    BD_LOG("SKULPAD", "onStatusUpdate name=%s status=%d transferred=%lld total=%lld error=%d errorCode=%d",
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
    // The Linux loader supplies the PAD result through the IL2CPP hook in
    // skul_pad.so. Treat the Java Play Core dependency as available so the
    // managed AndroidAssetPacks path does not abort before reaching it.
    verbose("PlayAssetDeliveryUnityWrapper", "playCoreApiMissing() -> false (native PAD shim)");
    return false;
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::cancelAssetPackDownload(
    std::shared_ptr<FakeJni::JString> name)
{
    auto names = std::make_shared<FakeJni::JArray<FakeJni::JString>>(1);
    (*names)[0] = name ? name : std::make_shared<FakeJni::JString>("CustomFastFollow");
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
    (*names)[0] = name ? name : std::make_shared<FakeJni::JString>("CustomFastFollow");
    downloadAssetPacks(names, callback);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::downloadAssetPacks(
    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback)
{
    verbose("PlayAssetDeliveryUnityWrapper", "downloadAssetPacks(%d) -> completed", names ? names->getSize() : 0);
    if (!callback)
        return;
    // An empty list is a normal success path when every custom pack is already
    // installed. AssetPackDownloader has called NotifyCustomPackDownloaded for
    // those packs before reaching Java; after this method returns it observes
    // the empty list and sets customAssetPacksDownloaded itself. Synthesizing a
    // status event here would report a pack that is no longer pending and make
    // the managed callback dereference a missing list/dictionary entry.
    if (!names || names->getSize() == 0) {
        BD_LOG("SKULPAD", "empty download list; all custom packs already installed");
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
    if (pack_name == "UnityDataAssetPack" ||
        pack_name == "UnityStreamingAssetsPack") {
        // The Android asset-pack root contains the "assets/" directory.
        // Returning game_root/assets would make Unity look for
        // <root>/assets/assets/bin/Data and leaves the core data pack
        // partially mounted.
        path = game_root;
    } else if (pack_name.size() == 32 &&
               std::all_of(pack_name.begin(), pack_name.end(), [](unsigned char c) {
                   return std::isxdigit(c) != 0;
               })) {
        // Addressables passes the bundle hash through GetAssetPackPath.
        path = custom_root / "assetpacks" / "CustomFastFollow" / "48" / "48" /
               "assets" / (pack_name + ".bundle");
    } else {
        path = custom_root / "assetpacks" / "CustomFastFollow" / "48" / "48";
    }

    const std::string path_string = path.lexically_normal().string();
    BD_LOG("SKULPAD", "getAssetPackPath(%s) -> %s",
           pack_name.empty() ? "(null)" : pack_name.c_str(), path_string.c_str());
    return std::make_shared<FakeJni::JString>(path_string);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::getAssetPackState(
    std::shared_ptr<FakeJni::JString> name,
    std::shared_ptr<IAssetPackManagerStatusQueryCallback> callback)
{
    auto names = std::make_shared<FakeJni::JArray<FakeJni::JString>>(1);
    (*names)[0] = name ? name : std::make_shared<FakeJni::JString>("CustomFastFollow");
    getAssetPackStates(names, callback);
}

void jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper::getAssetPackStates(
    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<IAssetPackManagerStatusQueryCallback> callback)
{
    verbose("PlayAssetDeliveryUnityWrapper", "getAssetPackStates(%d) -> completed", names ? names->getSize() : 0);
    if (!callback || !names)
        return;
    auto statuses = std::make_shared<FakeJni::JIntArray>(names->getSize());
    auto errors = std::make_shared<FakeJni::JIntArray>(names->getSize());
    for (int i = 0; i < names->getSize(); ++i) {
        BD_LOG("SKULPAD", "getAssetPackStates[%d] name=%s status=4 error=0",
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
    verbose("UnityReflection", "getConstructorID(%s, %s) = 0x%p \n",
            clazz->getName().c_str(), signature ? signature->c_str() : "(null)", ctor.get());
    return ctor;
}

std::shared_ptr<jnivm::java::lang::reflect::Method> jnivm::com::unity3d::player::ReflectionHelper::getMethodID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> methodName, std::shared_ptr<FakeJni::JString> signature, bool isStatic)
{
    if(clazz == nullptr)
        return nullptr;

    const char* name = methodName.get()->c_str();
    const char* sig;

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
        verbose("UnityReflection", "getMethodID(type 1, %s, %s, %s, %d) = 0x%p \n", clazz->getName().c_str(), name, foundMethod->signature.c_str(), isStatic, method.get());
        return method;
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
    verbose("UnityReflection", "getMethodID(type 2, %s, %s, %s, %d) = 0x%p \n", clazz->getName().c_str(), name, sig, isStatic, method.get());
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
            verbose("UnityReflection", "getFieldID(%s, %s, %s, %d) = %p \n", clazz->getName().c_str(), fieldName.get()->c_str(), sig, isStatic, field);
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
    { FakeJni::Function<&ReflectionHelper::setNativeExceptionOnProxy> {}, "setNativeExceptionOnProxy", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&ReflectionHelper::createInvocationError> {}, "createInvocationError", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::unity3d::player::ReflectionHelper::InvocationError) { FakeJni::Constructor<InvocationError, long, bool> {} },
    END_NATIVE_DESCRIPTOR
