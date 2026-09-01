#include "sys_volume.h"
#include "logging.h"

#include "toml++/toml.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

#if defined(__linux__)
#include <filesystem>
#endif

extern toml::table config;

enum class Backend {
    Passthrough, // mixer / Pulse / CFW already owns loudness
    Sysfs,       // volume stored in a sysfs file; PCM is full-scale
    Software,    // in-game keys only, no persistence
};

static std::atomic<int> g_percent{100};
static std::once_flag g_once;
static Backend g_backend = Backend::Passthrough;
static bool g_owns_keys = false;
static std::string g_sysfs_path;
static int g_sysfs_max = 10;

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static bool file_readable(const char* path)
{
    if (!path || !path[0])
        return false;
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static bool sdl_driver_is_pulse()
{
    const char* drv = getenv("SDL_AUDIODRIVER");
    return drv && (!strcmp(drv, "pulse") || !strcmp(drv, "pipewire"));
}

static std::string probe_sysfs_path()
{
    static const char* kKnown[] = {
        // Anbernic stock dmenu (volumeCtrl.dge)
        "/sys/class/power_supply/axp2202-battery/openbor_volume",
        "/sys/class/power_supply/axp20x-battery/openbor_volume",
    };
    for (const char* p : kKnown) {
        if (file_readable(p))
            return p;
    }
#if defined(__linux__)
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) {
        auto p = e.path() / "openbor_volume";
        if (file_readable(p.c_str()))
            return p.string();
    }
#endif
    return {};
}

static int read_sysfs_level()
{
    int v = g_sysfs_max;
    FILE* f = fopen(g_sysfs_path.c_str(), "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = g_sysfs_max;
        fclose(f);
    }
    return clamp_i(v, 0, g_sysfs_max);
}

static void store_from_sysfs()
{
    int v = read_sysfs_level();
    int pct = (g_sysfs_max > 0) ? (v * 100 / g_sysfs_max) : 100;
    g_percent.store(clamp_i(pct, 0, 100), std::memory_order_relaxed);
}

static const char* backend_str(Backend b)
{
    switch (b) {
    case Backend::Sysfs:
        return "sysfs";
    case Backend::Software:
        return "software";
    case Backend::Passthrough:
    default:
        return "passthrough";
    }
}

static void init_unlocked()
{
    std::string want = "auto";
    std::string sysfs;
    int sysfs_max = 10;
    int intercept_mode = 0; // 0 auto, 1 force on, -1 force off

    if (auto audio = config["audio"].as_table()) {
        want = (*audio)["backend"].value_or<std::string>("auto");
        sysfs = (*audio)["sysfs_path"].value_or<std::string>("");
        sysfs_max = (*audio)["sysfs_max"].value_or<int>(10);
        if (auto n = (*audio)["intercept_volume_keys"]) {
            if (auto b = n.value<bool>())
                intercept_mode = *b ? 1 : -1;
            else if (auto s = n.value<std::string>()) {
                if (*s == "true")
                    intercept_mode = 1;
                else if (*s == "false")
                    intercept_mode = -1;
            }
        }
    }

    if (const char* e = getenv("BD_SYS_VOLUME_BACKEND"); e && e[0])
        want = e;
    if (const char* e = getenv("BD_SYS_VOLUME_PATH"); e && e[0])
        sysfs = e;
    if (const char* e = getenv("BD_SYS_VOLUME_MAX"); e && e[0]) {
        int n = atoi(e);
        if (n > 0)
            sysfs_max = n;
    }

    g_sysfs_max = sysfs_max > 0 ? sysfs_max : 10;

    Backend b = Backend::Passthrough;
    if (want == "sysfs") {
        if (sysfs.empty())
            sysfs = probe_sysfs_path();
        b = sysfs.empty() ? Backend::Passthrough : Backend::Sysfs;
    } else if (want == "software") {
        b = Backend::Software;
    } else if (want == "passthrough" || want == "pulse") {
        b = Backend::Passthrough;
    } else {
        // auto: if SDL is actually talking to Pulse, do not also software-scale.
        // A leftover pulse socket must not hide Anbernic-style sysfs volume
        // when the game is on ALSA (hk.sh sets SDL_AUDIODRIVER=alsa).
        if (sysfs.empty())
            sysfs = probe_sysfs_path();
        if (sdl_driver_is_pulse())
            b = Backend::Passthrough;
        else if (!sysfs.empty())
            b = Backend::Sysfs;
        else
            b = Backend::Passthrough;
    }

    if (b == Backend::Sysfs) {
        g_sysfs_path = sysfs;
        if (!file_readable(g_sysfs_path.c_str())) {
            BD_LOG("AUDIO", "sysfs volume %s not readable, falling back to passthrough",
                   g_sysfs_path.c_str());
            b = Backend::Passthrough;
            g_sysfs_path.clear();
        }
    }

    g_backend = b;
    if (intercept_mode == 1)
        g_owns_keys = true;
    else if (intercept_mode == -1)
        g_owns_keys = false;
    else
        g_owns_keys = (b == Backend::Sysfs || b == Backend::Software);

    if (b == Backend::Sysfs)
        store_from_sysfs();
    else
        g_percent.store(100, std::memory_order_relaxed);

    if (b == Backend::Sysfs) {
        BD_LOG("AUDIO", "volume backend=sysfs path=%s max=%d percent=%d keys=%d",
               g_sysfs_path.c_str(), g_sysfs_max,
               g_percent.load(std::memory_order_relaxed), (int)g_owns_keys);
    } else {
        BD_LOG("AUDIO", "volume backend=%s percent=%d keys=%d",
               backend_str(b), g_percent.load(std::memory_order_relaxed),
               (int)g_owns_keys);
    }

}

static void init_once()
{
    std::call_once(g_once, init_unlocked);
}

int bd_sys_volume_percent()
{
    init_once();
    int p = g_percent.load(std::memory_order_relaxed);
    if (p < 0)
        p = 100;
    if (p > 100)
        p = 100;
    return p;
}

void bd_sys_volume_poll()
{
    init_once();
    if (g_backend == Backend::Sysfs)
        store_from_sysfs();
}

void bd_sys_volume_adjust(int delta)
{
    init_once();
    if (g_backend == Backend::Passthrough && !g_owns_keys)
        return;

    if (g_backend == Backend::Sysfs) {
        int v = clamp_i(read_sysfs_level() + delta, 0, g_sysfs_max);
        FILE* f = fopen(g_sysfs_path.c_str(), "w");
        if (f) {
            fprintf(f, "%d\n", v);
            fclose(f);
        }
        int pct = (g_sysfs_max > 0) ? (v * 100 / g_sysfs_max) : 100;
        g_percent.store(clamp_i(pct, 0, 100), std::memory_order_relaxed);
        BD_LOG("AUDIO", "system volume %d/%d (%d%%)", v, g_sysfs_max, pct);
        return;
    }

    int step = (g_sysfs_max > 0) ? (100 / g_sysfs_max) : 10;
    if (step < 1)
        step = 10;
    int p = clamp_i(g_percent.load(std::memory_order_relaxed) + delta * step, 0, 100);
    g_percent.store(p, std::memory_order_relaxed);
    BD_LOG("AUDIO", "software volume %d%%", p);
}

bool bd_sys_volume_owns_keys()
{
    init_once();
    return g_owns_keys;
}

const char* bd_sys_volume_backend_name()
{
    init_once();
    return backend_str(g_backend);
}
