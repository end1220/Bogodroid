#include "shader_cache.h"

#include "logging.h"
#include "toml++/toml.hpp"

#include <filesystem>
#include <fstream>
#include <string>

extern toml::table config;

namespace {

namespace fs = std::filesystem;

const char* const kCacheDirName = "UnityShaderCache";
// Name of the stamp file inside the cache directory. Deliberately dot-prefixed
// and unlikely to collide with Unity's own bookkeeping ("version").
const char* const kStampName = ".bd_rewrite";

std::string stamp_value()
{
    return std::to_string(BD_SHADER_REWRITE_VERSION);
}

bool stamp_matches(const fs::path& dir)
{
    std::ifstream in(dir / kStampName);
    if (!in)
        return false;
    std::string value;
    std::getline(in, value);
    return value == stamp_value();
}

void write_stamp(const fs::path& dir)
{
    std::ofstream out(dir / kStampName, std::ios::trunc);
    if (out)
        out << stamp_value() << "\n";
}

// Drops a cache directory that cannot be trusted. Removing the whole tree (not
// just the program files) also drops Unity's "version" file, so Unity re-creates
// the layout itself exactly as it would on a fresh install.
void handle_cache_dir(const fs::path& dir, const char* why)
{
    if (stamp_matches(dir))
        return;
    std::error_code ec;
    const uintmax_t removed = fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    write_stamp(dir);
    BD_LOG("SHADER",
           "shader cache %s: %s -> dropped %llu entries, stamp=%s",
           dir.string().c_str(), why, (unsigned long long)removed,
           stamp_value().c_str());
}

// The port's own cache directory, as configured (paths.android_cache points at
// the cache directory itself: "../cache/", "cache/", ...).
void consider_cache_root(const fs::path& cache_root)
{
    std::error_code ec;
    const fs::path dir = cache_root / kCacheDirName;
    if (fs::is_directory(dir, ec))
        handle_cache_dir(dir, "configured cache path");
}

// Unity has been seen writing the cache in more than one place (the port root
// and, for Unity 6, <data>/assets/bin/cache), so hunt for the directory rather
// than trusting one path. Depth-limited: a cache is never buried deep, and the
// walk must stay cheap on a slow SD card.
void hunt(const fs::path& root, int max_depth)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it.depth() > max_depth) {
            it.disable_recursion_pending();
            it.increment(ec);
            continue;
        }
        const fs::path path = it->path();
        std::error_code dir_ec;
        if (fs::is_directory(path, dir_ec) && path.filename() == kCacheDirName) {
            handle_cache_dir(path, "found in the port tree");
            it.disable_recursion_pending();
        }
        it.increment(ec);
    }
}

} // namespace

void bd_shader_cache::prepare()
{
    // The loader runs with the port directory as its working directory.
    hunt(".", 8);

    // Plus whatever the toml points at, in case the cache lives outside the port
    // tree (android_data is the parent the loader maps /data/data/... onto).
    const std::string cache_root =
        config["paths"]["android_cache"].value_or<std::string>("");
    if (!cache_root.empty())
        consider_cache_root(cache_root);
    const std::string data_root =
        config["paths"]["android_data"].value_or<std::string>("");
    if (!data_root.empty())
        consider_cache_root(fs::path(data_root) / "cache");
}
