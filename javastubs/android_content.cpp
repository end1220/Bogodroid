// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "device_display.h"
#include "javac.h"
#include "logging.h"
#include <fstream>
#include <inttypes.h>
#include <pthread.h>
#include <filesystem>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <atomic>
#include "json.hpp"
using bd_json = nlohmann::json;

///// PackageManager
// getInstallerPackageName stays here: registerFactory<JString> would catch
// every missing String-returning method, not just this one.

static std::string bd_abs_path(const std::string& path)
{
    if (char* abs = realpath(path.c_str(), nullptr)) {
        std::string out(abs);
        free(abs);
        return out;
    }
    try {
        return std::filesystem::absolute(path).lexically_normal().string();
    } catch (...) {
        return path;
    }
}

static std::shared_ptr<jnivm::android::content::pm::ApplicationInfo>
bd_make_application_info()
{
    auto info = std::make_shared<jnivm::android::content::pm::ApplicationInfo>();

    std::string dd = config["paths"]["android_data"].value_or<std::string>("../");
    info->dataDir = std::make_shared<FakeJni::JString>(bd_abs_path(dd).c_str());
    BD_LOG("DATADIR", "ApplicationInfo.dataDir = %s",
            info->dataDir->asStdString().c_str());

    std::string pkg = config["package"]["packageName"].value_or<std::string>("");
    info->packageName = std::make_shared<FakeJni::JString>(pkg.c_str());
    BD_LOG("DATADIR", "ApplicationInfo.packageName = %s", pkg.c_str());

    // Prefer the staged Unity data-pack APK. cwd is already paths.game_files
    // after init_config, so resolve against current_path() first — joining
    // "./gamedata" again produced the broken .../gamedata/gamedata/ path.
    std::string code = config["paths"]["android_package_code"].value_or<std::string>("./");
    std::string source = bd_abs_path(code);
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path staged_apk = cwd / "UnityDataAssetPack.apk";
    if (!std::filesystem::is_regular_file(staged_apk, ec)) {
        std::filesystem::path game_files(
            config["paths"]["game_files"].value_or<std::string>("."));
        staged_apk = std::filesystem::absolute(game_files) / "UnityDataAssetPack.apk";
    }
    if (std::filesystem::is_regular_file(staged_apk, ec)) {
        // Android ApplicationInfo.sourceDir is the APK file path. Unity builds
        // MountDataArchive paths as "<sourceDir>/assets/..." so the resulting
        // string contains the ".apk/" marker ZipCentralDirectory requires.
        source = staged_apk.lexically_normal().string();
    } else if (auto* sources = config["paths"]["android_source_dirs"].as_array();
               sources && !sources->empty()) {
        if (auto* first = sources->get(0)->as_string()) {
            std::filesystem::path cand(std::string(first->get()));
            if (cand.is_relative())
                cand = cwd / cand;
            // If config repeats the game_files leaf while cwd is already there,
            // prefer cwd instead of creating a nested duplicate.
            if (!std::filesystem::exists(cand, ec) &&
                cwd.filename() == cand.filename())
                cand = cwd;
            source = bd_abs_path(cand.string());
        }
    }
    info->sourceDir = std::make_shared<FakeJni::JString>(source.c_str());
    info->publicSourceDir = std::make_shared<FakeJni::JString>(source.c_str());
    BD_LOG("DATADIR", "ApplicationInfo.sourceDir = %s", source.c_str());

    std::filesystem::path libdir = cwd / "lib" / "arm64-v8a";
    if (!std::filesystem::is_directory(libdir, ec))
        libdir = cwd / "lib";
    info->nativeLibraryDir =
        std::make_shared<FakeJni::JString>(libdir.lexically_normal().string().c_str());
    BD_LOG("DATADIR", "ApplicationInfo.nativeLibraryDir = %s",
           info->nativeLibraryDir->asStdString().c_str());

    info->metaData = std::make_shared<jnivm::android::os::Bundle>();

    // Version gates Unity reads through JNI. Default: the API level this
    // loader claims everywhere else (Build.VERSION.SDK_INT = 26 in
    // javastubs/android.h) — everything newer wants framework APIs
    // (scoped storage >= 29, WindowInsetsController >= 30) that are not
    // stubbed here. Overridable per package for a title that needs them.
    info->minSdkVersion = static_cast<int>(
        config["package"]["minSdkVersion"].value_or<int64_t>(26));
    info->targetSdkVersion = static_cast<int>(
        config["package"]["targetSdkVersion"].value_or<int64_t>(26));
    BD_LOG("DATADIR", "ApplicationInfo minSdkVersion=%d targetSdkVersion=%d",
           info->minSdkVersion, info->targetSdkVersion);
    return info;
}

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

std::shared_ptr<jnivm::android::content::pm::ApplicationInfo>
jnivm::android::content::pm::PackageManager::getApplicationInfo(
    std::shared_ptr<FakeJni::JString> packageName, int flags)
{
    BD_LOG("DATADIR", "PackageManager.getApplicationInfo(%s, 0x%x)",
           packageName ? packageName->c_str() : "(null)", flags);
    return bd_make_application_info();
}

std::shared_ptr<jnivm::android::content::pm::PackageInfo>
jnivm::android::content::pm::PackageManager::getPackageInfo(
    std::shared_ptr<FakeJni::JString> packageName, int flags)
{
    BD_LOG("DATADIR", "PackageManager.getPackageInfo(%s, 0x%x)",
           packageName ? packageName->c_str() : "(null)", flags);
    auto info = std::make_shared<PackageInfo>();
    info->versionName = (FakeJni::JString)config["package"]["versionName"]
                            .value_or<std::string>("0.1")
                            .c_str();
    info->versionCode = static_cast<FakeJni::JInt>(
        config["package"]["versionCode"].value_or<int64_t>(1));
    return info;
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
    // Full path, not just the leaf: android_files is a TOML setting and the
    // directory it names need not exist yet (the shipped configs point at
    // "./conf/", which a fresh extraction does not contain). A single mkdir()
    // fails with ENOENT there, and the only symptom is "save FAIL: cannot open
    // .../shared_prefs/....json.tmp" at shutdown, after PlayerPrefs already
    // reported success.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        BD_LOG("PREFS", "cannot create %s (%s); PlayerPrefs writes will fail",
                dir.c_str(), ec.message().c_str());
    }
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
        BD_LOG("PREFS", "save FAIL: cannot open %s (errno=%d %s)",
                tmp.c_str(), errno, strerror(errno));
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

// getAll() -> java.util.Map. The map is deliberately empty: making it useful
// needs Map.Entry plus Set/Iterator over it, which nothing here has needed yet
// (Unity's PlayerPrefs path reads keys individually through get*()). Returning
// a real (empty) Map instead of the STUB-MISS default keeps the call honest and
// keeps the log clean; see docs/UNITY6.md "known limitations".
std::shared_ptr<jnivm::java::util::Map>
jnivm::android::content::SharedPreferences::getAll()
{
    verbose("JBRIDGE", "SharedPreferences.getAll('%s') -> empty map "
                       "(entries are not exposed)", name.c_str());
    return std::make_shared<jnivm::java::util::Map>();
}

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
    return bd_make_application_info();
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

    if (*service == WINDOW_SERVICE)
        return std::make_shared<jnivm::android::view::WindowManager>();

    verbose("JBRIDGE", "App requesting unknown system service %s", service.get()->c_str());

    return nullptr;
}

std::shared_ptr<jnivm::android::content::ContentResolver>
jnivm::android::content::Context::getContentResolver()
{
    // Rewired's Android input helper (and Unity's proxy code) call this
    // during startup and dereference the result. Before this existed the
    // stub-miss path returned a dummy object, which surfaced as
    // "System.NullReferenceException" from Rewired and then took down the
    // process on the exception path.
    return std::make_shared<jnivm::android::content::ContentResolver>();
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
    std::error_code ec;
    const std::filesystem::path staged_apk =
        std::filesystem::current_path() / "UnityDataAssetPack.apk";
    if (std::filesystem::is_regular_file(staged_apk, ec)) {
        const std::string path = staged_apk.lexically_normal().string();
        BD_LOG("DATADIR", "getPackageCodePath -> %s", path.c_str());
        return std::make_shared<FakeJni::JString>(path.c_str());
    }
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

// Android exposes assets through Context.getAssets(); Unity and its plugins
// open files under assets/ this way (the rest of Unity's IO goes straight
// through libc on the staged game_files tree). AssetManager is the real stub.
std::shared_ptr<jnivm::android::content::res::AssetManager>
jnivm::android::content::Context::getAssets()
{
    return std::make_shared<jnivm::android::content::res::AssetManager>();
}

std::shared_ptr<jnivm::android::content::pm::PackageManager>
jnivm::android::content::Context::getPackageManager()
{
    return std::make_shared<jnivm::android::content::pm::PackageManager>();
}

// Same instance the Activity path hands out, so Configuration/DisplayMetrics
// cannot disagree between a Context-typed and an Activity-typed caller.
static std::shared_ptr<jnivm::android::content::res::Resources> bd_resources_singleton()
{
    static std::shared_ptr<jnivm::android::content::res::Resources> instance =
        std::make_shared<jnivm::android::content::res::Resources>();
    return instance;
}

std::shared_ptr<jnivm::android::content::res::Resources>
jnivm::android::content::Context::getResources()
{
    return bd_resources_singleton();
}

// [paths] android_obb_dirs mirrors what ApplicationInfo.sourceDir does for
// android_source_dirs: first entry wins for the singular getObbDir().
static std::vector<std::string> bd_obb_dirs()
{
    std::vector<std::string> out;
    if (auto* dirs = config["paths"]["android_obb_dirs"].as_array()) {
        for (auto&& entry : *dirs) {
            if (auto* s = entry.as_string())
                out.push_back(bd_abs_path(std::string(s->get())));
        }
    }
    return out;
}

std::shared_ptr<jnivm::java::io::File>
jnivm::android::content::Context::getObbDir()
{
    const std::vector<std::string> dirs = bd_obb_dirs();
    if (dirs.empty())
        return nullptr;
    BD_LOG("DATADIR", "getObbDir -> %s", dirs.front().c_str());
    return std::make_shared<jnivm::java::io::File>(
        std::make_shared<FakeJni::JString>(dirs.front().c_str()));
}

std::shared_ptr<jnivm::Array<jnivm::java::io::File>>
jnivm::android::content::Context::getObbDirs()
{
    const std::vector<std::string> dirs = bd_obb_dirs();
    auto array = std::make_shared<jnivm::Array<jnivm::java::io::File>>(dirs.size());
    for (size_t i = 0; i < dirs.size(); i++) {
        (*array)[i] = std::make_shared<jnivm::java::io::File>(
            std::make_shared<FakeJni::JString>(dirs[i].c_str()));
    }
    return array;
}

int jnivm::android::content::Context::checkCallingOrSelfPermission(std::shared_ptr<FakeJni::JString> permission)
{
    verbose("JBRIDGE", "Granting permission: %s", permission.get()->c_str());
    return jnivm::android::content::pm::PackageManager::PERMISSION_GRANTED; // Sure why not, what could go wrong....
}

///// Intent

// getExtras() is the path Unity uses to read the engine command line on
// Android ("unity" extra). It reads through Bundle, which already serves
// [package] mainIntentBundle, so hand back a Bundle rather than the factory's
// blank instance — same data, but the intent of the call is explicit.
std::shared_ptr<jnivm::android::os::Bundle>
jnivm::android::content::Intent::getExtras()
{
    return std::make_shared<jnivm::android::os::Bundle>();
}

///// Configuration

std::shared_ptr<jnivm::android::content::res::Configuration>
jnivm::android::content::res::Configuration::make_current()
{
    auto c = std::make_shared<Configuration>();
    const int w = bd_device_display_width();
    const int h = bd_device_display_height();
    c->densityDpi = bd_device_display_dpi();
    c->fontScale = 1.0f;
    c->orientation = w > h ? ORIENTATION_LANDSCAPE : ORIENTATION_PORTRAIT;
    // dp == px at densityDpi 160. Scaling by dpi/160 matches what Android does
    // and keeps Screen.width/height (px) and the dp values coherent for Unity.
    const double dp_per_px = 160.0 / (c->densityDpi > 0 ? c->densityDpi : 160);
    c->screenWidthDp = static_cast<int>(w * dp_per_px);
    c->screenHeightDp = static_cast<int>(h * dp_per_px);
    c->smallestScreenWidthDp = std::min(c->screenWidthDp, c->screenHeightDp);
    // SCREENLAYOUT_SIZE_LARGE: no size-code / long / layoutdir bits set, which
    // is what Android reports for a plain fullscreen activity.
    c->screenLayout = 0x02;
    c->uiMode = UI_MODE_TYPE_NORMAL;
    // Input / network capabilities. Values mirror thunks/ndk/ndk.cpp's
    // AConfiguration_* answers ([device] keyboard / displayTouchscreen), so a
    // title that reads either view sees the same device.
    c->mcc = 0;
    c->mnc = 0;
    c->keyboard = config["device"]["keyboard"].value_or<int>(static_cast<int>(KEYBOARD_QWERTY));
    c->keyboardHidden = KEYBOARDHIDDEN_UNDEFINED;
    c->hardKeyboardHidden = HARDKEYBOARDHIDDEN_UNDEFINED;
    c->navigation = NAVIGATION_UNDEFINED;
    c->navigationHidden = NAVIGATIONHIDDEN_UNDEFINED;
    c->touchscreen = config["device"]["displayTouchscreen"].value_or<int>(static_cast<int>(TOUCHSCREEN_NOTOUCH));
    c->colorMode = COLOR_MODE_UNDEFINED;
    BD_DEBUG("RES", "Configuration %dx%d dp %dx%d orientation=%d densityDpi=%d touchscreen=%d keyboard=%d",
             w, h, c->screenWidthDp, c->screenHeightDp, c->orientation, c->densityDpi,
             c->touchscreen, c->keyboard);
    return c;
}

std::shared_ptr<jnivm::android::content::res::Configuration>
jnivm::android::content::res::Resources::getConfiguration()
{
    return jnivm::android::content::res::Configuration::make_current();
}

// The UI language. One entry (the process default locale): Unity 6 reads
// get(0) and never checks size() first, so an empty list would leave the
// language empty.
std::shared_ptr<jnivm::android::os::LocaleList>
jnivm::android::content::res::Configuration::getLocales()
{
    return jnivm::android::os::LocaleList::getDefault();
}

std::shared_ptr<jnivm::android::util::DisplayMetrics>
jnivm::android::content::res::Resources::getDisplayMetrics()
{
    auto metrics = std::make_shared<jnivm::android::util::DisplayMetrics>();
    metrics->widthPixels = bd_device_display_width();
    metrics->heightPixels = bd_device_display_height();
    metrics->densityDpi = bd_device_display_dpi();
    return metrics;
}

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
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_USER> {}, "SCREEN_ORIENTATION_USER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_BEHIND> {}, "SCREEN_ORIENTATION_BEHIND", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_NOSENSOR> {}, "SCREEN_ORIENTATION_NOSENSOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_SENSOR_LANDSCAPE> {}, "SCREEN_ORIENTATION_SENSOR_LANDSCAPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_SENSOR_PORTRAIT> {}, "SCREEN_ORIENTATION_SENSOR_PORTRAIT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_FULL_SENSOR> {}, "SCREEN_ORIENTATION_FULL_SENSOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&ActivityInfo::SCREEN_ORIENTATION_LOCKED> {}, "SCREEN_ORIENTATION_LOCKED", FakeJni::JFieldID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::PackageInfo) { FakeJni::Constructor<PackageInfo> {} },
    { FakeJni::Field<&PackageInfo::versionName> {}, "versionName", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&PackageInfo::versionCode> {}, "versionCode", FakeJni::JFieldID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::ApplicationInfo) { FakeJni::Constructor<ApplicationInfo> {} },
    { FakeJni::Field<&ApplicationInfo::dataDir> {}, "dataDir", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::nativeLibraryDir> {}, "nativeLibraryDir", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::sourceDir> {}, "sourceDir", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::publicSourceDir> {}, "publicSourceDir", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::packageName> {}, "packageName", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::metaData> {}, "metaData", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::splitPublicSourceDirs> {}, "splitPublicSourceDirs", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::minSdkVersion> {}, "minSdkVersion", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&ApplicationInfo::targetSdkVersion> {}, "targetSdkVersion", FakeJni::JFieldID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::pm::PackageManager) { FakeJni::Constructor<PackageManager> {} },
    { FakeJni::Field<&PackageManager::FEATURE_AUDIO_LOW_LATENCY> {}, "FEATURE_AUDIO_LOW_LATENCY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&PackageManager::PERMISSION_GRANTED> {}, "PERMISSION_GRANTED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&PackageManager::GET_META_DATA> {}, "GET_META_DATA", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&PackageManager::hasSystemFeature> {}, "hasSystemFeature", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PackageManager::getInstallerPackageName> {}, "getInstallerPackageName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PackageManager::getApplicationInfo> {}, "getApplicationInfo", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&PackageManager::getPackageInfo> {}, "getPackageInfo", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::res::AssetManager) { FakeJni::Constructor<AssetManager> {} },
    { FakeJni::Function<&AssetManager::open> {}, "open", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetManager::list> {}, "list", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::res::Configuration) { FakeJni::Constructor<Configuration> {} },
    { FakeJni::Field<&Configuration::ORIENTATION_UNDEFINED> {}, "ORIENTATION_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::ORIENTATION_PORTRAIT> {}, "ORIENTATION_PORTRAIT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::ORIENTATION_LANDSCAPE> {}, "ORIENTATION_LANDSCAPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::ORIENTATION_SQUARE> {}, "ORIENTATION_SQUARE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::SCREENLAYOUT_SIZE_MASK> {}, "SCREENLAYOUT_SIZE_MASK", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::UI_MODE_TYPE_MASK> {}, "UI_MODE_TYPE_MASK", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::UI_MODE_TYPE_UNDEFINED> {}, "UI_MODE_TYPE_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::UI_MODE_TYPE_NORMAL> {}, "UI_MODE_TYPE_NORMAL", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::TOUCHSCREEN_NOTOUCH> {}, "TOUCHSCREEN_NOTOUCH", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::TOUCHSCREEN_STYLUS> {}, "TOUCHSCREEN_STYLUS", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::TOUCHSCREEN_FINGER> {}, "TOUCHSCREEN_FINGER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::KEYBOARD_NOKEYS> {}, "KEYBOARD_NOKEYS", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::KEYBOARD_QWERTY> {}, "KEYBOARD_QWERTY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::KEYBOARD_12KEY> {}, "KEYBOARD_12KEY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::KEYBOARDHIDDEN_UNDEFINED> {}, "KEYBOARDHIDDEN_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::KEYBOARDHIDDEN_NO> {}, "KEYBOARDHIDDEN_NO", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::KEYBOARDHIDDEN_YES> {}, "KEYBOARDHIDDEN_YES", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::HARDKEYBOARDHIDDEN_UNDEFINED> {}, "HARDKEYBOARDHIDDEN_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::HARDKEYBOARDHIDDEN_NO> {}, "HARDKEYBOARDHIDDEN_NO", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::HARDKEYBOARDHIDDEN_YES> {}, "HARDKEYBOARDHIDDEN_YES", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATION_UNDEFINED> {}, "NAVIGATION_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATION_NONAV> {}, "NAVIGATION_NONAV", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATION_DPAD> {}, "NAVIGATION_DPAD", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATION_TRACKBALL> {}, "NAVIGATION_TRACKBALL", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATION_WHEEL> {}, "NAVIGATION_WHEEL", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATIONHIDDEN_UNDEFINED> {}, "NAVIGATIONHIDDEN_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATIONHIDDEN_NO> {}, "NAVIGATIONHIDDEN_NO", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::NAVIGATIONHIDDEN_YES> {}, "NAVIGATIONHIDDEN_YES", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::COLOR_MODE_UNDEFINED> {}, "COLOR_MODE_UNDEFINED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::COLOR_MODE_WIDE_COLOR_GAMUT_NO> {}, "COLOR_MODE_WIDE_COLOR_GAMUT_NO", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::COLOR_MODE_HDR_NO> {}, "COLOR_MODE_HDR_NO", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Configuration::COLOR_MODE_DEFAULT> {}, "COLOR_MODE_DEFAULT", FakeJni::JFieldID::STATIC },
    // Public instance fields: Unity reads these with GetFieldID, so they have
    // to be registered as real fields and not just C++ members.
    { FakeJni::Field<&Configuration::densityDpi> {}, "densityDpi", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::screenWidthDp> {}, "screenWidthDp", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::screenHeightDp> {}, "screenHeightDp", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::smallestScreenWidthDp> {}, "smallestScreenWidthDp", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::orientation> {}, "orientation", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::screenLayout> {}, "screenLayout", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::uiMode> {}, "uiMode", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::fontScale> {}, "fontScale", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::mcc> {}, "mcc", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::mnc> {}, "mnc", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::keyboard> {}, "keyboard", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::keyboardHidden> {}, "keyboardHidden", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::hardKeyboardHidden> {}, "hardKeyboardHidden", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::navigation> {}, "navigation", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::navigationHidden> {}, "navigationHidden", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::touchscreen> {}, "touchscreen", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Configuration::colorMode> {}, "colorMode", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Function<&Configuration::getLocales> {}, "getLocales", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::res::Resources) { FakeJni::Constructor<Resources> {} },
    { FakeJni::Function<&Resources::getIdentifier> {}, "getIdentifier", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Resources::getConfiguration> {}, "getConfiguration", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Resources::getDisplayMetrics> {}, "getDisplayMetrics", FakeJni::JMethodID::PUBLIC },
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
    { FakeJni::Function<&SharedPreferences::getAll> {}, "getAll", FakeJni::JMethodID::PUBLIC },
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
    { FakeJni::Field<&Context::WINDOW_SERVICE> {}, "WINDOW_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::SENSOR_SERVICE> {}, "SENSOR_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::VIBRATOR_SERVICE> {}, "VIBRATOR_SERVICE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Context::MODE_PRIVATE> {}, "MODE_PRIVATE", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&Context::getSystemService> {}, "getSystemService", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getContentResolver> {}, "getContentResolver", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getApplicationInfo> {}, "getApplicationInfo", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getPackageName> {}, "getPackageName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getPackageCodePath> {}, "getPackageCodePath", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getSharedPreferences> {}, "getSharedPreferences", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getExternalFilesDir> {}, "getExternalFilesDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getFilesDir> {}, "getFilesDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getDataDir> {}, "getDataDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getCacheDir> {}, "getCacheDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getExternalCacheDir> {}, "getExternalCacheDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getAssets> {}, "getAssets", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getPackageManager> {}, "getPackageManager", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getResources> {}, "getResources", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getObbDir> {}, "getObbDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getObbDirs> {}, "getObbDirs", FakeJni::JMethodID::PUBLIC },
    // getWindow / getWindowManager -> registerFactory in android_descriptors.cpp.
    { FakeJni::Function<&Context::checkCallingOrSelfPermission> {}, "checkCallingOrSelfPermission", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::content::Intent) { FakeJni::Constructor<Intent> {} },
    { FakeJni::Function<&Intent::getExtras> {}, "getExtras", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR
