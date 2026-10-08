#include "plugin_api.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
const BogoPluginApi* g_api = nullptr;

std::filesystem::path relative_path(const char* raw) {
    std::string value = raw ? raw : "";
    while (!value.empty() && (value.front() == '/' || value.front() == '\\'))
        value.erase(value.begin());
    while (value.rfind("./", 0) == 0)
        value.erase(0, 2);
    return std::filesystem::path(value).lexically_normal();
}

std::filesystem::path asset_root() {
    return std::filesystem::current_path() / "assets";
}

struct Locator {
    std::filesystem::path root = asset_root();
};

struct Reader {
    FILE* file = nullptr;
    std::filesystem::path path;
    long long size = 0;
    long long position = 0;
    std::vector<int8_t> pending;

    explicit Reader(std::filesystem::path p) : path(std::move(p)) {
        file = std::fopen(path.string().c_str(), "rb");
        std::error_code ec;
        size = static_cast<long long>(std::filesystem::file_size(path, ec));
        if (ec) size = 0;
    }
    ~Reader() { if (file) std::fclose(file); }
};

void seek_file(FILE* file, long long position) {
#if defined(_WIN32)
    _fseeki64(file, position, SEEK_SET);
#else
    fseeko(file, static_cast<off_t>(position), SEEK_SET);
#endif
}

BogoJniValue result_object(void* value) {
    BogoJniValue result{};
    result.l = value;
    return result;
}

BogoJniValue locator_ctor(void* env, void*, const BogoJniValue* args,
                          uint32_t count, void*) {
    if (g_api->log) g_api->log("ASSETLOC", "ctor count=%u", count);
    auto locator = new Locator();
    if (count >= 2 && args && args[1].l) {
        char root[1024] = {};
        if (g_api->jni_string_utf8(env, args[1].l, root, sizeof(root))) {
            auto candidate = asset_root() / relative_path(root);
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec)) locator->root = candidate;
        }
    }
    return result_object(g_api->jni_new_object(env,
        "com/mobge/assetlocator/AssetLocator", locator));
}

BogoJniValue list_assets(void* env, void* receiver, const BogoJniValue* args,
                         uint32_t count, void*) {
    if (g_api->log) g_api->log("ASSETLOC", "ListAssets receiver=%p", receiver);
    auto* locator = static_cast<Locator*>(g_api->jni_object_userdata(receiver));
    if (!locator) return result_object(nullptr);
    char path[1024] = {};
    if (count && args && args[0].l)
        g_api->jni_string_utf8(env, args[0].l, path, sizeof(path));
    auto dir = path[0] ? locator->root / relative_path(path) : locator->root;
    std::vector<std::string> names;
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    void* result = g_api->jni_new_string_array(env, static_cast<uint32_t>(names.size()));
    for (uint32_t i = 0; result && i < names.size(); ++i) {
        void* value = g_api->jni_new_string_utf8(env, names[i].c_str());
        g_api->jni_string_array_set(env, result, i, value);
    }
    return result_object(result);
}

BogoJniValue get_reader(void* env, void* receiver, const BogoJniValue* args,
                        uint32_t count, void*) {
    auto* locator = static_cast<Locator*>(g_api->jni_object_userdata(receiver));
    if (!locator || !count || !args || !args[0].l) return result_object(nullptr);
    char path[2048] = {};
    g_api->jni_string_utf8(env, args[0].l, path, sizeof(path));
    auto* reader = new Reader(locator->root / relative_path(path));
    if (g_api->log) g_api->log("ASSETLOC", "GetReaderWrapper('%s') -> %s",
                               path, reader->path.string().c_str());
    return result_object(g_api->jni_new_object(env,
        "com/mobge/assetlocator/AssetReader", reader));
}

BogoJniValue seek_reader(void*, void* receiver, const BogoJniValue* args,
                          uint32_t count, void*) {
    auto* reader = static_cast<Reader*>(g_api->jni_object_userdata(receiver));
    BogoJniValue result{};
    if (!reader || count < 2 || !args) { result.j = 0; return result; }
    long long base = args[1].i == 1 ? reader->position : args[1].i == 2 ? reader->size : 0;
    reader->position = std::max(0LL, base + args[0].j);
    if (reader->file) seek_file(reader->file, reader->position);
    reader->pending.clear();
    result.j = reader->position;
    return result;
}

BogoJniValue read_reader(void*, void* receiver, const BogoJniValue* args,
                         uint32_t count, void*) {
    auto* reader = static_cast<Reader*>(g_api->jni_object_userdata(receiver));
    BogoJniValue result{};
    if (!reader || !reader->file || !count || !args || args[0].i <= 0) return result;
    long long amount = std::min<long long>(args[0].i, reader->size - reader->position);
    if (amount <= 0) return result;
    reader->pending.resize(static_cast<size_t>(amount));
    seek_file(reader->file, reader->position);
    size_t got = std::fread(reader->pending.data(), 1, reader->pending.size(), reader->file);
    reader->pending.resize(got);
    result.i = static_cast<int32_t>(got);
    return result;
}

BogoJniValue get_bytes(void* env, void* receiver, const BogoJniValue* args,
                       uint32_t count, void*) {
    auto* reader = static_cast<Reader*>(g_api->jni_object_userdata(receiver));
    if (!reader) return result_object(nullptr);
    if (reader->pending.empty() && count && args && args[0].i > 0 && reader->file) {
        long long amount = std::min<long long>(args[0].i, reader->size - reader->position);
        if (amount > 0) {
            reader->pending.resize(static_cast<size_t>(amount));
            seek_file(reader->file, reader->position);
            reader->pending.resize(std::fread(reader->pending.data(), 1,
                                              reader->pending.size(), reader->file));
        }
    }
    void* result = g_api->jni_new_byte_array(env, static_cast<uint32_t>(reader->pending.size()));
    if (result && !reader->pending.empty())
        g_api->jni_byte_array_write(env, result, reader->pending.data(),
                                    static_cast<uint32_t>(reader->pending.size()));
    reader->position += static_cast<long long>(reader->pending.size());
    reader->pending.clear();
    return result_object(result);
}

BogoJniValue reader_length(void*, void* receiver, const BogoJniValue*, uint32_t, void*) {
    BogoJniValue result{};
    auto* reader = static_cast<Reader*>(g_api->jni_object_userdata(receiver));
    result.j = reader ? reader->size : 0;
    return result;
}

BogoJniValue reader_position(void*, void* receiver, const BogoJniValue*, uint32_t, void*) {
    BogoJniValue result{};
    auto* reader = static_cast<Reader*>(g_api->jni_object_userdata(receiver));
    result.j = reader ? reader->position : 0;
    return result;
}

BogoJniValue reader_close(void*, void* receiver, const BogoJniValue*, uint32_t, void*) {
    auto* reader = static_cast<Reader*>(g_api->jni_object_userdata(receiver));
    if (reader && reader->file) { std::fclose(reader->file); reader->file = nullptr; }
    return {};
}

const BogoJniMethod locator_methods[] = {
    {"<init>", "(Ljava/lang/Object;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;", 1u, locator_ctor, nullptr},
    {"<init>", "(Landroid/content/Context;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;", 1u, locator_ctor, nullptr},
    {"<init>", "(Landroid/app/Activity;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;", 1u, locator_ctor, nullptr},
    {"<init>", "(Lcom/unity3d/player/UnityPlayerActivity;Ljava/lang/String;)Lcom/mobge/assetlocator/AssetLocator;", 1u, locator_ctor, nullptr},
    {"ListAssets", "(Ljava/lang/String;)[Ljava/lang/String;", 0u, list_assets, nullptr},
    {"GetReaderWrapper", "(Ljava/lang/String;)Ljava/lang/Object;", 0u, get_reader, nullptr},
};

const BogoJniMethod reader_methods[] = {
    {"Seek", "(JI)J", 0u, seek_reader, nullptr},
    {"Read", "(I)I", 0u, read_reader, nullptr},
    {"GetBytes", "(I)[B", 0u, get_bytes, nullptr},
    {"GetBytes", "()[B", 0u, get_bytes, nullptr},
    {"GetLength", "()J", 0u, reader_length, nullptr},
    {"GetPosition", "()J", 0u, reader_position, nullptr},
    {"Close", "()V", 0u, reader_close, nullptr},
};
}

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api) {
    if (!api || api->abi_version < 7 || !api->register_jni_class ||
        !api->jni_new_object || !api->jni_object_userdata ||
        !api->jni_new_byte_array || !api->jni_new_string_array) return 0;
    g_api = api;
    return api->register_jni_class("com/mobge/assetlocator/AssetLocator",
                                   locator_methods, sizeof(locator_methods) / sizeof(locator_methods[0])) &&
           api->register_jni_class("com/mobge/assetlocator/AssetReader",
                                   reader_methods, sizeof(reader_methods) / sizeof(reader_methods[0]));
}
