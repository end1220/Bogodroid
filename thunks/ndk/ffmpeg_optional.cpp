#define BD_FFMPEG_OPTIONAL_IMPLEMENTATION
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "ffmpeg_optional.h"

#include <dlfcn.h>
#include <mutex>

namespace bd_ffmpeg {
#define BD_FFMPEG_DEFINE(name) decltype(&::name) name = nullptr;
BD_FFMPEG_SYMBOLS(BD_FFMPEG_DEFINE)
#undef BD_FFMPEG_DEFINE

namespace {
std::once_flag g_once;
bool g_available = false;

void* open_one(const char* soname) {
    return dlopen(soname, RTLD_LAZY | RTLD_GLOBAL);
}

void load_once() {
    void* avutil = open_one("libavutil.so.56");
    void* swresample = open_one("libswresample.so.3");
    void* swscale = open_one("libswscale.so.5");
    void* avcodec = open_one("libavcodec.so.58");
    void* avformat = open_one("libavformat.so.58");
    g_available = avutil && swresample && swscale && avcodec && avformat;
#define BD_FFMPEG_LOAD(name) \
    name = reinterpret_cast<decltype(name)>(dlsym(RTLD_DEFAULT, #name)); \
    g_available = g_available && name;
    BD_FFMPEG_SYMBOLS(BD_FFMPEG_LOAD)
#undef BD_FFMPEG_LOAD
}
} // namespace

bool ensure_loaded() {
    std::call_once(g_once, load_once);
    return g_available;
}
} // namespace bd_ffmpeg
