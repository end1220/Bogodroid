// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "javac.h"
#include "logging.h"
#include <fstream>
#include <inttypes.h>
#include <pthread.h>
#include <filesystem>
#include <chrono>
#include <atomic>
#include "json.hpp"
using bd_json = nlohmann::json;

///// PackageManager
// getPackageInfo migrated to registerFactory in android_descriptors.cpp.
// getInstallerPackageName stays here: registerFactory<JString> would catch
// every missing String-returning method, not just this one.

std::shared_ptr<FakeJni::JString>
jnivm::android::content::pm::PackageManager::getInstallerPackageName(std::shared_ptr<FakeJni::JString> packageName)
{
    return std::make_shared<FakeJni::JString>("");
}

bool jnivm::android::content::pm::PackageManager::hasSystemFeature(std::shared_ptr<FakeJni::JString> feature)
{
    verbose("JBRIDGE", "App asks about availability of feature: %s", feature.get()->c_str());
    return false; // We don't claim support of anything right now
}

///// AssetManager

std::shared_ptr<jnivm::java::io::InputStream>
jnivm::android::content::res::AssetManager::open(std::shared_ptr<FakeJni::JString> filename)
{
    verbose("JBRIDGE", "AssetManager opening file %s", filename.get()->c_str());
    return std::make_shared<jnivm::java::io::InputStream>(std::make_shared<FakeJni::JString>(std::string("assets/").append(filename.get()->c_str())));
}

std::shared_ptr<jnivm::Array<FakeJni::JString>>
jnivm::android::content::res::AssetManager::list(std::shared_ptr<FakeJni::JString> path)
{
    const std::string relPath = path ? path->c_str() : "";
    verbose("JBRIDGE", "AssetManager listing files in '%s'", relPath.c_str());

    // Construct the actual filesystem path
    std::filesystem::path p = std::filesystem::path("assets") / relPath;

    try {
        if (!std::filesystem::exists(p) || !std::filesystem::is_directory(p)) {
            return std::make_shared<jnivm::Array<jnivm::java::lang::String>>(0);
        }

        std::vector<std::string> entries;

        for (const auto& entry : std::filesystem::directory_iterator(p)) {
            // ANDROID SPEC: Only return the child name; no prefix
            std::string name = entry.path().filename().string();
            entries.push_back(name);

            verbose("JBRIDGE", "AssetManager found entry '%s'", name.c_str());
        }

        // Convert vector<string> to JNI-style Array<JString>
        auto result = std::make_shared<jnivm::Array<jnivm::java::lang::String>>(entries.size());

        for (size_t i = 0; i < entries.size(); i++) {
            (*result)[i] = std::make_shared<jnivm::java::lang::String>(entries[i]);
        }

        return result;
    }
    catch (const std::filesystem::filesystem_error& e) {
        verbose("JBRIDGE", "Error listing files in '%s': %s", relPath.c_str(), e.what());
        std::make_shared<jnivm::Array<jnivm::java::lang::String>>(0);
    }
}


///// Resources

int jnivm::android::content::res::Resources::getIdentifier(std::shared_ptr<FakeJni::JString> name, std::shared_ptr<FakeJni::JString> defType, std::shared_ptr<FakeJni::JString> defPackage)
{
    // Args may be null — game passes them during init.
    BD_DEBUG("RES", "getIdentifier(%s, %s, %s)",
            name      ? name->c_str()      : "(null)",
            defType   ? defType->c_str()   : "(null)",
            defPackage? defPackage->c_str(): "(null)");
    return 1337420;
}

///// SharedPreferences (persisted as JSON to <android_files>/shared_prefs/<name>.json)
// Unity PlayerPrefs sits on top of this — without persistence, saves drop silently.

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace {
std::string bd_prefs_file_path(const std::string& name)
{
    std::string dir = config["paths"]["android_files"].value_or<std::string>("./");
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    dir += "shared_prefs";
    mkdir(dir.c_str(), 0755);
    return dir + "/" + name + ".json";
}
} // anon

void jnivm::android::content::SharedPreferences::load(const std::string& fromName)
{
    name = fromName;
    int_vals.clear(); long_vals.clear(); float_vals.clear();
    bool_vals.clear(); string_vals.clear();

    std::ifstream f(bd_prefs_file_path(name));
    if (!f.is_open()) return;

    bd_json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        BD_LOG("PREFS", "'%s' parse error (%s), starting empty",
                name.c_str(), e.what());
        return;
    }
    if (!doc.is_object()) {
        BD_LOG("PREFS", "'%s' top-level not an object, starting empty", name.c_str());
        return;
    }
    int ver = doc.value("version", 0);
    if (ver != 1) {
        BD_LOG("PREFS", "'%s' unknown version %d, starting empty",
                name.c_str(), ver);
        return;
    }
    auto load_section = [&](const char* key, auto& target) {
        auto it = doc.find(key);
        if (it == doc.end() || !it->is_object()) return;
        for (auto& [k, v] : it->items()) {
            try { target[k] = v.template get<typename std::remove_reference_t<decltype(target)>::mapped_type>(); }
            catch (...) {}
        }
    };
    load_section("ints",    int_vals);
    load_section("longs",   long_vals);
    load_section("floats",  float_vals);
    load_section("bools",   bool_vals);
    load_section("strings", string_vals);

    BD_LOG("PREFS", "loaded '%s' i=%zu l=%zu f=%zu b=%zu s=%zu",
            name.c_str(), int_vals.size(), long_vals.size(),
            float_vals.size(), bool_vals.size(), string_vals.size());
}

void jnivm::android::content::SharedPreferences::save() const
{
    std::string path = bd_prefs_file_path(name);
    std::string tmp  = path + ".tmp";

    bd_json doc;
    doc["version"] = 1;
    doc["ints"]    = int_vals;
    doc["longs"]   = long_vals;
    doc["floats"]  = float_vals;
    doc["bools"]   = bool_vals;
    doc["strings"] = string_vals;

    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open()) {
        BD_LOG("PREFS", "save FAIL: cannot open %s", tmp.c_str());
        return;
    }
    f << doc.dump(2);  // pretty-printed for human readability
    f.flush();
    f.close();
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        BD_LOG("PREFS", "save FAIL: rename %s -> %s", tmp.c_str(), path.c_str());
        return;
    }
    BD_LOG("PREFS", "saved '%s' (%zu records)",
            name.c_str(),
            int_vals.size() + long_vals.size() + float_vals.size()
            + bool_vals.size() + string_vals.size());
}

// Truncate at a UTF-8 codepoint boundary (back up off continuation bytes).
namespace {
size_t bd_safe_utf8_cut(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s.size();
    size_t cut = maxBytes;
    while (cut > 0 && (((unsigned char)s[cut] & 0xC0) == 0x80)) {
        --cut;
    }
    return cut;
}
} // anon

namespace {
void bd_log_get(const char* api, const std::string& key, bool hit, const std::string& valSummary)
{
    static std::atomic<int> n{0};
    int cur = n.fetch_add(1);
    if (cur < 200) {
        BD_DEBUG("PREFS-GET", "%s(%s) %s = %s",
                api, key.c_str(), hit ? "HIT" : "miss", valSummary.c_str());
    } else if (cur == 200) {
        BD_DEBUG("PREFS-GET", "(further get* calls suppressed)");
    }
}
} // anon

bool jnivm::android::content::SharedPreferences::contains(std::shared_ptr<FakeJni::JString> key)
{
    if (!key) return false;
    std::string k = key->asStdString();
    bool hit = int_vals.count(k) || long_vals.count(k) || float_vals.count(k)
        || bool_vals.count(k) || string_vals.count(k);
    bd_log_get("contains", k, hit, hit ? "true" : "false");
    return hit;
}

int jnivm::android::content::SharedPreferences::getInt(std::shared_ptr<FakeJni::JString> key, int def)
{
    if (!key) return def;
    std::string k = key->asStdString();
    auto it = int_vals.find(k);
    bool hit = it != int_vals.end();
    int v = hit ? it->second : def;
    bd_log_get("getInt", k, hit, std::to_string(v));
    return v;
}

jlong jnivm::android::content::SharedPreferences::getLong(std::shared_ptr<FakeJni::JString> key, jlong def)
{
    if (!key) return def;
    std::string k = key->asStdString();
    auto it = long_vals.find(k);
    bool hit = it != long_vals.end();
    jlong v = hit ? (jlong)it->second : def;
    bd_log_get("getLong", k, hit, std::to_string(v));
    return v;
}

float jnivm::android::content::SharedPreferences::getFloat(std::shared_ptr<FakeJni::JString> key, float def)
{
    if (!key) return def;
    std::string k = key->asStdString();
    auto it = float_vals.find(k);
    bool hit = it != float_vals.end();
    float v = hit ? it->second : def;
    bd_log_get("getFloat", k, hit, std::to_string(v));
    return v;
}

bool jnivm::android::content::SharedPreferences::getBoolean(std::shared_ptr<FakeJni::JString> key, bool def)
{
    if (!key) return def;
    std::string k = key->asStdString();
    auto it = bool_vals.find(k);
    bool hit = it != bool_vals.end();
    bool v = hit ? it->second : def;
    bd_log_get("getBoolean", k, hit, v ? "true" : "false");
    return v;
}

std::shared_ptr<FakeJni::JString> jnivm::android::content::SharedPreferences::getString(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> def)
{
    if (!key) return def;
    std::string k = key->asStdString();
    auto it = string_vals.find(k);
    bool hit = it != string_vals.end();
    if (hit) {
        bd_log_get("getString", k, true, "\"" + it->second + "\" (" + std::to_string(it->second.size()) + "B)");
        return std::make_shared<FakeJni::JString>(it->second.c_str());
    }
    bd_log_get("getString", k, false, def ? "<default>" : "<null>");
    return def;
}

// getAll -> registerFactory (java/util/Map).

std::shared_ptr<jnivm::android::content::SharedPreferencesEditor> jnivm::android::content::SharedPreferences::edit()
{
    BD_DEBUG("PREFS-EDIT", "edit() called on '%s'", name.c_str());
    auto e = std::make_shared<SharedPreferencesEditor>();
    e->owner = std::dynamic_pointer_cast<SharedPreferences>(shared_from_this());
    return e;
}

namespace {
std::mutex& bd_prefs_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<jnivm::android::content::SharedPreferences>>& bd_prefs_cache() {
    static std::map<std::string, std::shared_ptr<jnivm::android::content::SharedPreferences>> c;
    return c;
}
} // anon

void jnivm::android::content::SharedPreferences::flush_all()
{
    std::lock_guard<std::mutex> g(bd_prefs_mutex());
    int dirty_count = 0;
    for (auto& [name, p] : bd_prefs_cache()) {
        if (p && p->dirty) {
            p->save();
            p->dirty = false;
            dirty_count++;
        }
    }
    if (dirty_count > 0) {
        BD_LOG("PREFS", "flush_all: wrote %d dirty file(s)", dirty_count);
    }
}

// C-ABI shim for libc TU (exit_impl) — avoids pulling jnivm headers in.
extern "C" void bd_flush_prefs_impl()
{
    jnivm::android::content::SharedPreferences::flush_all();
}

///// SharedPreferencesEditor

static std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
bd_editor_self(jnivm::android::content::SharedPreferencesEditor* self)
{
    // Aliasing shared_ptr (no-op deleter) — the real owner is `edit()`'s ptr.
    return std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>(
        self, [](jnivm::android::content::SharedPreferencesEditor*){});
}

namespace {
void bd_log_put(const char* op, const std::string& key, const std::string& val)
{
    BD_DEBUG("PREFS-PUT", "%s(%s) = %s", op, key.c_str(), val.c_str());
}
} // anon

std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::putInt(std::shared_ptr<FakeJni::JString> key, int val)
{
    if (key) {
        std::string k = key->asStdString();
        pending_int[k] = val;
        bd_log_put("putInt", k, std::to_string(val));
    }
    return bd_editor_self(this);
}
std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::putLong(std::shared_ptr<FakeJni::JString> key, jlong val)
{
    if (key) {
        std::string k = key->asStdString();
        pending_long[k] = val;
        bd_log_put("putLong", k, std::to_string(val));
    }
    return bd_editor_self(this);
}
std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::putFloat(std::shared_ptr<FakeJni::JString> key, float val)
{
    if (key) {
        std::string k = key->asStdString();
        pending_float[k] = val;
        bd_log_put("putFloat", k, std::to_string(val));
    }
    return bd_editor_self(this);
}
std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::putBoolean(std::shared_ptr<FakeJni::JString> key, bool val)
{
    if (key) {
        std::string k = key->asStdString();
        pending_bool[k] = val;
        bd_log_put("putBoolean", k, val ? "true" : "false");
    }
    return bd_editor_self(this);
}
std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::putString(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> val)
{
    if (key) {
        std::string k = key->asStdString();
        if (val) {
            std::string v = val->asStdString();
            pending_string[k] = v;
            bd_log_put("putString", k, "\"" + v + "\" (" + std::to_string(v.size()) + "B)");
        } else {
            pending_remove.insert(k);
            bd_log_put("putString-null/remove", k, "(null)");
        }
    }
    return bd_editor_self(this);
}
std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::remove(std::shared_ptr<FakeJni::JString> key)
{
    if (key) {
        std::string k = key->asStdString();
        pending_remove.insert(k);
        bd_log_put("remove", k, "");
    }
    return bd_editor_self(this);
}
std::shared_ptr<jnivm::android::content::SharedPreferencesEditor>
jnivm::android::content::SharedPreferencesEditor::clear()
{
    pending_clear = true;
    BD_DEBUG("PREFS-PUT", "clear() — staged full wipe");
    return bd_editor_self(this);
}

// Merge pending changes into the owner's in-memory map; mark dirty for flush.
static void bd_apply_to_owner(jnivm::android::content::SharedPreferencesEditor* e,
                              jnivm::android::content::SharedPreferences* p)
{
    if (e->pending_clear) {
        p->int_vals.clear(); p->long_vals.clear(); p->float_vals.clear();
        p->bool_vals.clear(); p->string_vals.clear();
        e->pending_clear = false;
    }
    for (auto& [k, v] : e->pending_int)    p->int_vals[k] = v;
    for (auto& [k, v] : e->pending_long)   p->long_vals[k] = v;
    for (auto& [k, v] : e->pending_float)  p->float_vals[k] = v;
    for (auto& [k, v] : e->pending_bool)   p->bool_vals[k] = v;
    for (auto& [k, v] : e->pending_string) p->string_vals[k] = v;
    for (auto& k : e->pending_remove) {
        p->int_vals.erase(k);    p->long_vals.erase(k);
        p->float_vals.erase(k);  p->bool_vals.erase(k);
        p->string_vals.erase(k);
    }
    e->pending_int.clear(); e->pending_long.clear(); e->pending_float.clear();
    e->pending_bool.clear(); e->pending_string.clear(); e->pending_remove.clear();
    p->dirty = true;
}

void jnivm::android::content::SharedPreferencesEditor::apply()
{
    auto p = owner.lock();
    if (!p) return;
    BD_DEBUG("PREFS-EDIT", "apply() pending: int=%zu long=%zu float=%zu bool=%zu string=%zu remove=%zu clear=%d",
            pending_int.size(), pending_long.size(), pending_float.size(),
            pending_bool.size(), pending_string.size(), pending_remove.size(),
            (int)pending_clear);
    bd_apply_to_owner(this, p.get());  // apply() = in-memory only; disk waits for flush_all/commit.
}

bool jnivm::android::content::SharedPreferencesEditor::commit()
{
    auto p = owner.lock();
    if (!p) return false;
    BD_DEBUG("PREFS-EDIT", "commit() pending: int=%zu long=%zu float=%zu bool=%zu string=%zu remove=%zu clear=%d",
            pending_int.size(), pending_long.size(), pending_float.size(),
            pending_bool.size(), pending_string.size(), pending_remove.size(),
            (int)pending_clear);
    bd_apply_to_owner(this, p.get());

    // Rate-limit disk writes — flush_all() at exit catches deferred saves.
    constexpr long long BD_PREFS_MIN_SAVE_S = 2;
    long long now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last  = p->last_save_epoch_s.load();
    if (now_s - last < BD_PREFS_MIN_SAVE_S) {
        return true; // dirty stays true; will flush at exit or next eligible commit
    }
    p->save();
    p->dirty = false;
    p->last_save_epoch_s.store(now_s);
    return true;
}

///// Context
//
// getAssets / getResources / getWindow / getContentResolver /
// getPackageManager are now registerFactory entries (android_descriptors.cpp).
// Anything that reads config[] or has real logic stays.

std::shared_ptr<jnivm::android::content::pm::ApplicationInfo>
jnivm::android::content::Context::getApplicationInfo()
{
    auto info = std::make_shared<jnivm::android::content::pm::ApplicationInfo>();

    // Populate dataDir = absolute path of config["paths"]["android_data"].
    // This is what Unity reads (via reflection) to figure out where to
    // write its PlayerPrefs xml: <dataDir>/shared_prefs/<bundle>.v2.playerprefs.xml
    std::string dd = config["paths"]["android_data"].value_or<std::string>("../");
    if (char* abs = realpath(dd.c_str(), nullptr)) {
        info->dataDir = std::make_shared<FakeJni::JString>(abs);
        free(abs);
    } else {
        info->dataDir = std::make_shared<FakeJni::JString>(dd.c_str());
    }
    BD_LOG("DATADIR", "ApplicationInfo.dataDir = %s",
            info->dataDir->asStdString().c_str());

    // Unity prefixes prefs name with applicationInfo.packageName, so we MUST
    // populate this — otherwise PlayerPrefs file ends up named
    // ".v2.playerprefs" (no bundle prefix) which doesn't match Android.
    std::string pkg = config["package"]["packageName"].value_or<std::string>("");
    info->packageName = std::make_shared<FakeJni::JString>(pkg.c_str());
    BD_LOG("DATADIR", "ApplicationInfo.packageName = %s", pkg.c_str());

    return info;
}

std::shared_ptr<FakeJni::JObject>
jnivm::android::content::Context::getSystemService(std::shared_ptr<FakeJni::JString> service)
{
    if (service == nullptr)
        return nullptr;

    if (*service == LOCATION_SERVICE)
        return nullptr;

    if (*service == AUDIO_SERVICE)
        return std::make_shared<jnivm::android::media::AudioManager>();

    if (*service == DISPLAY_SERVICE)
        return std::make_shared<jnivm::android::hardware::display::DisplayManager>();

    if (*service == POWER_SERVICE)
        return std::make_shared<jnivm::android::os::PowerManager>();

    if (*service == MEDIA_ROUTER_SERVICE)
        return std::make_shared<jnivm::android::media::MediaRouter>();

    if (*service == INPUT_SERVICE)
        return std::make_shared<jnivm::android::hardware::input::InputManager>();

    verbose("JBRIDGE", "App requesting unknown system service %s", service.get()->c_str());

    return nullptr;
}

std::shared_ptr<FakeJni::JString>
jnivm::android::content::Context::getPackageName()
{
    return std::make_shared<FakeJni::JString>(config["package"]["packageName"].value_or<std::string>("package.name.not.defined"));
}

std::shared_ptr<jnivm::android::content::SharedPreferences>
jnivm::android::content::Context::getSharedPreferences(std::shared_ptr<FakeJni::JString> str, int num)
{
    std::string name = str ? str->asStdString() : std::string("default");
    std::lock_guard<std::mutex> g(bd_prefs_mutex());
    auto& cache = bd_prefs_cache();
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    auto p = std::make_shared<SharedPreferences>();
    p->load(name);
    cache[name] = p;
    return p;
}

std::shared_ptr<FakeJni::JString>
jnivm::android::content::Context::getPackageCodePath()
{
    return std::make_shared<FakeJni::JString>(config["paths"]["android_package_code"].value_or<std::string>("./path_not_defined_code"));
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getExternalFilesDir(std::shared_ptr<FakeJni::JString> path)
{
    return getExternalFilesDirInternal();
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getExternalFilesDirInternal()
{
    char* resolved_path = realpath(config["paths"]["android_external_files"].value_or<std::string>("./path_not_defined_external").c_str(), NULL);
    if (resolved_path == NULL) {
        return NULL;
    }
    size_t len = strlen(resolved_path);

    // Check if it already has a trailing slash
    if (len > 0 && resolved_path[len - 1] != '/') {
        // Reallocate to add space for slash and null terminator
        char* with_slash = (char*)realloc(resolved_path, len + 2);
        if (with_slash == NULL) {
            free(resolved_path);
            return NULL;
        }
        with_slash[len] = '/';
        with_slash[len + 1] = '\0';
        return std::make_shared<jnivm::java::io::File>(std::make_shared<FakeJni::JString>(with_slash));
    }
    return NULL;
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getFilesDir()
{
    char* resolved_path = realpath(config["paths"]["android_files"].value_or<std::string>("./path_not_defined").c_str(), NULL);
    if (resolved_path == NULL) {
        return NULL;
    }
    size_t len = strlen(resolved_path);

    // Check if it already has a trailing slash
    if (len > 0 && resolved_path[len - 1] != '/') {
        // Reallocate to add space for slash and null terminator
        char* with_slash = (char*)realloc(resolved_path, len + 2);
        if (with_slash == NULL) {
            free(resolved_path);
            return NULL;
        }
        with_slash[len] = '/';
        with_slash[len + 1] = '\0';
        return std::make_shared<jnivm::java::io::File>(std::make_shared<FakeJni::JString>(with_slash));
    }
    return NULL;
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getCacheDir()
{
    return std::make_shared<jnivm::java::io::File>(std::make_shared<FakeJni::JString>(config["paths"]["android_cache"].value_or<std::string>("./path_not_defined_cache")));
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getDataDir()
{
    // Same source of truth as ApplicationInfo.dataDir — Unity may use either
    // path API. See Context::getApplicationInfo() above for rationale.
    std::string dd = config["paths"]["android_data"].value_or<std::string>("../");
    if (char* abs = realpath(dd.c_str(), nullptr)) {
        auto f = std::make_shared<jnivm::java::io::File>(std::make_shared<FakeJni::JString>(abs));
        free(abs);
        return f;
    }
    return std::make_shared<jnivm::java::io::File>(std::make_shared<FakeJni::JString>(dd.c_str()));
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getExternalCacheDir()
{
    return std::make_shared<jnivm::java::io::File>(std::make_shared<FakeJni::JString>(config["paths"]["android_cache"].value_or<std::string>("./path_not_defined_cache")));
}

// getObbDir / getObbDirs return null — left to the STUB-MISS path
// (defaultVal<jobject> returns nullptr by default, same as before).

int jnivm::android::content::Context::checkCallingOrSelfPermission(std::shared_ptr<FakeJni::JString> permission)
{
    verbose("JBRIDGE", "Granting permission: %s", permission.get()->c_str());
    return jnivm::android::content::pm::PackageManager::PERMISSION_GRANTED; // Sure why not, what could go wrong....
}

///// Intent — getExtras migrated to registerFactory (Bundle).

///// Content Descriptors

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::ActivityInfo) { FakeJni::Constructor<ActivityInfo> {} },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_PORTRAIT> {}, "SCREEN_ORIENTATION_PORTRAIT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_REVERSE_PORTRAIT> {}, "SCREEN_ORIENTATION_REVERSE_PORTRAIT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_REVERSE_LANDSCAPE> {}, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_LANDSCAPE> {}, "SCREEN_ORIENTATION_LANDSCAPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_FULL_USER> {}, "SCREEN_ORIENTATION_FULL_USER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_USER_PORTRAIT> {}, "SCREEN_ORIENTATION_USER_PORTRAIT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_USER_LANDSCAPE> {}, "SCREEN_ORIENTATION_USER_LANDSCAPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_SENSOR> {}, "SCREEN_ORIENTATION_SENSOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_UNSPECIFIED> {}, "SCREEN_ORIENTATION_UNSPECIFIED", FakeJni::JFieldID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::PackageInfo) { FakeJni::Constructor<PackageInfo> {} },
    { FakeJni::Field<&PackageInfo::versionName> {}, "versionName", FakeJni::JFieldID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::ApplicationInfo) { FakeJni::Constructor<ApplicationInfo> {} },
    { FakeJni::Field<&ApplicationInfo::dataDir> {}, "dataDir", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::nativeLibraryDir> {}, "nativeLibraryDir", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::packageName> {}, "packageName", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::splitPublicSourceDirs> {}, "splitPublicSourceDirs", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::PackageManager) { FakeJni::Constructor<PackageManager> {} },
    { FakeJni::Field<&PackageManager::FEATURE_AUDIO_LOW_LATENCY> {}, "FEATURE_AUDIO_LOW_LATENCY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&PackageManager::PERMISSION_GRANTED> {}, "PERMISSION_GRANTED", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&PackageManager::hasSystemFeature> {}, "hasSystemFeature", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PackageManager::getInstallerPackageName> {}, "getInstallerPackageName", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::res::AssetManager) { FakeJni::Constructor<AssetManager> {} },
    { FakeJni::Function<&AssetManager::open> {}, "open", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetManager::list> {}, "list", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::res::Resources) { FakeJni::Constructor<Resources> {} },
    { FakeJni::Function<&Resources::getIdentifier> {}, "getIdentifier", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::SharedPreferencesEditor) { FakeJni::Constructor<SharedPreferencesEditor> {} },
    { FakeJni::Function<&SharedPreferencesEditor::apply> {}, "apply", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::commit> {}, "commit", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::putInt> {}, "putInt", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::putLong> {}, "putLong", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::putFloat> {}, "putFloat", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::putBoolean> {}, "putBoolean", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::putString> {}, "putString", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::remove> {}, "remove", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferencesEditor::clear> {}, "clear", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::SharedPreferences) { FakeJni::Constructor<SharedPreferences> {} },
    { FakeJni::Function<&SharedPreferences::contains> {}, "contains", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferences::getInt> {}, "getInt", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferences::getLong> {}, "getLong", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferences::getFloat> {}, "getFloat", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferences::getBoolean> {}, "getBoolean", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SharedPreferences::getString> {}, "getString", FakeJni::JMethodID::PUBLIC },
    // getAll -> registerFactory.
    { FakeJni::Function<&SharedPreferences::edit> {}, "edit", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::ContentResolver) { FakeJni::Constructor<ContentResolver> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::Context) { FakeJni::Constructor<Context> {} },
    { FakeJni::Field<&Context::LOCATION_SERVICE> {}, "LOCATION_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::DISPLAY_SERVICE> {}, "DISPLAY_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::AUDIO_SERVICE> {}, "AUDIO_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::MEDIA_ROUTER_SERVICE> {}, "MEDIA_ROUTER_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::POWER_SERVICE> {}, "POWER_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::INPUT_SERVICE> {}, "INPUT_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::MODE_PRIVATE> {}, "MODE_PRIVATE", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&Context::getSystemService> {}, "getSystemService", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getApplicationInfo> {}, "getApplicationInfo", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getPackageName> {}, "getPackageName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getPackageCodePath> {}, "getPackageCodePath", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getSharedPreferences> {}, "getSharedPreferences", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getExternalFilesDir> {}, "getExternalFilesDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getFilesDir> {}, "getFilesDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getDataDir> {}, "getDataDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getCacheDir> {}, "getCacheDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getExternalCacheDir> {}, "getExternalCacheDir", FakeJni::JMethodID::PUBLIC },
    // getAssets / getPackageManager / getResources / getWindow /
    // getContentResolver -> registerFactory in android_descriptors.cpp.
    // getObbDir / getObbDirs -> STUB-MISS path returns null (same as before).
    { FakeJni::Function<&Context::checkCallingOrSelfPermission> {}, "checkCallingOrSelfPermission", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::Intent) { FakeJni::Constructor<Intent> {} },
    // getExtras -> registerFactory in android_descriptors.cpp.
    END_NATIVE_DESCRIPTOR