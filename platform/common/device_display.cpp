#include "device_display.h"
#include "logging.h"
#include "toml++/toml.hpp"

#include <SDL2/SDL.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern toml::table config;

namespace {

enum class Src : int { Unset = 0, Toml, Sdl, Fb0, Default };

struct FieldI {
    int value = 0;
    Src src = Src::Unset;
};

struct FieldF {
    float value = 0.f;
    Src src = Src::Unset;
};

FieldI g_w;
FieldI g_h;
FieldF g_rr;

// Last resolved set that was logged. The probe runs twice on the normal
// path (JNI stubs before EGL, then again after SDL_Init(VIDEO)), so this
// keeps the log to one line per distinct outcome.
FieldI g_logged_w;
FieldI g_logged_h;
FieldF g_logged_rr;
bool g_sdl_mode_logged = false;

const char* src_name(Src s)
{
    switch (s) {
    case Src::Toml: return "toml";
    case Src::Sdl: return "sdl";
    case Src::Fb0: return "fb0";
    case Src::Default: return "default";
    default: return "unset";
    }
}

// TOML locks permanently. SDL (GLEScene path) upgrades fb0/default.
// fb0 only fills unset/default before VIDEO is up.
bool can_set(Src current, Src incoming)
{
    if (current == Src::Toml)
        return false;
    if (incoming == Src::Toml)
        return true;
    if (incoming == Src::Sdl)
        return current != Src::Sdl;
    if (incoming == Src::Fb0)
        return current == Src::Unset || current == Src::Default;
    if (incoming == Src::Default)
        return current == Src::Unset;
    return false;
}

void apply_toml()
{
    const int tw = config["device"]["displayWidth"].value_or<int>(0);
    const int th = config["device"]["displayHeight"].value_or<int>(0);
    const float tr = config["device"]["displayRefreshRate"].value_or<float>(0);
    if (tw > 0 && can_set(g_w.src, Src::Toml))
        g_w = {tw, Src::Toml};
    if (th > 0 && can_set(g_h.src, Src::Toml))
        g_h = {th, Src::Toml};
    if (tr > 0.f && can_set(g_rr.src, Src::Toml))
        g_rr = {tr, Src::Toml};
}

// Same as GLEScene PlatformContext::init (Linux): after VIDEO init,
// SDL_GetCurrentDisplayMode(0) supplies omitted/<=0 dimensions.
void try_sdl_current_display_mode()
{
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO))
        return;

    SDL_DisplayMode current = {};
    if (SDL_GetCurrentDisplayMode(0, &current) != 0) {
        BD_LOG("DEVICE", "SDL_GetCurrentDisplayMode failed: %s", SDL_GetError());
        return;
    }

    if (!g_sdl_mode_logged) {
        g_sdl_mode_logged = true;
        BD_LOG("DEVICE", "SDL display mode: %dx%d @ %dHz",
               current.w, current.h, current.refresh_rate);
    }

    if (current.w > 0 && can_set(g_w.src, Src::Sdl))
        g_w = {current.w, Src::Sdl};
    if (current.h > 0 && can_set(g_h.src, Src::Sdl))
        g_h = {current.h, Src::Sdl};
    if (current.refresh_rate > 0 && can_set(g_rr.src, Src::Sdl))
        g_rr = {static_cast<float>(current.refresh_rate), Src::Sdl};
}

// Fallback for JNI stubs that query the display before EGL/SDL video exists.
void try_fb0_fallback()
{
    if (!can_set(g_w.src, Src::Fb0) && !can_set(g_h.src, Src::Fb0))
        return;

    const int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;

    fb_var_screeninfo vinfo = {};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        if (static_cast<int>(vinfo.xres) > 0 && can_set(g_w.src, Src::Fb0))
            g_w = {static_cast<int>(vinfo.xres), Src::Fb0};
        if (static_cast<int>(vinfo.yres) > 0 && can_set(g_h.src, Src::Fb0))
            g_h = {static_cast<int>(vinfo.yres), Src::Fb0};
    }
    close(fd);
}

void apply_defaults()
{
    if (can_set(g_w.src, Src::Default))
        g_w = {640, Src::Default};
    if (can_set(g_h.src, Src::Default))
        g_h = {480, Src::Default};
    if (can_set(g_rr.src, Src::Default))
        g_rr = {60.f, Src::Default};
}

void maybe_log()
{
    if (g_w.src == Src::Unset || g_h.src == Src::Unset || g_rr.src == Src::Unset)
        return;

    if (g_logged_w.value == g_w.value && g_logged_w.src == g_w.src &&
        g_logged_h.value == g_h.value && g_logged_h.src == g_h.src &&
        g_logged_rr.value == g_rr.value && g_logged_rr.src == g_rr.src)
        return;

    g_logged_w = g_w;
    g_logged_h = g_h;
    g_logged_rr = g_rr;

    BD_LOG("DEVICE", "using display %dx%d@%.0f (w=%s h=%s rr=%s)",
           g_w.value, g_h.value, g_rr.value,
           src_name(g_w.src), src_name(g_h.src), src_name(g_rr.src));
}

} // namespace

void bd_device_display_probe()
{
    apply_toml();
    // GLEScene primary path first; fb0 only when SDL is not up / failed.
    try_sdl_current_display_mode();
    try_fb0_fallback();
    apply_defaults();
    maybe_log();
}

int bd_device_display_width()
{
    if (g_w.src == Src::Unset || g_w.src == Src::Default)
        bd_device_display_probe();
    return g_w.value > 0 ? g_w.value : 640;
}

int bd_device_display_height()
{
    if (g_h.src == Src::Unset || g_h.src == Src::Default)
        bd_device_display_probe();
    return g_h.value > 0 ? g_h.value : 480;
}

float bd_device_display_refresh_rate()
{
    if (g_rr.src == Src::Unset || g_rr.src == Src::Default)
        bd_device_display_probe();
    return g_rr.value > 0.f ? g_rr.value : 60.f;
}
