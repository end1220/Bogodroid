// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#define _LARGEFILE64_SOURCE /* See feature_test_macros(7) */
#define _FILE_OFFSET_BITS 64
#include <sys/types.h>
#include <unistd.h>

#include "fcntl.h"
#include <unistd.h>
#include "platform.h"
#include "logging.h"
#include "so_util.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <cerrno>
#include <cinttypes>

char* clean_jar_path(const char* path) {
    if (!path) return NULL;

    const char* needles[] = {"jar:file:/!", "jar:file://!"};
    size_t num_needles = sizeof(needles) / sizeof(needles[0]);

    const char* src = path;
    int prefix_at_start = 0;

    // Check if jar prefix is at the start
    for (size_t i = 0; i < num_needles; ++i) {
        size_t needle_len = strlen(needles[i]);
        if (strncmp(path, needles[i], needle_len) == 0) {
            prefix_at_start = 1;
            break;
        }
    }

    // Get current working directory if needed
    char cwd[PATH_MAX];
    if (prefix_at_start) {
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return NULL;
        }
    }

    size_t len = strlen(path);
    size_t cwd_len = prefix_at_start ? strlen(cwd) : 0;
    char* clean_path = (char*)malloc(len + cwd_len + 2);  // +2 for potential "." and null
    if (!clean_path) return NULL;

    char* dst = clean_path;

    // Prepend CWD if jar prefix was at start
    if (prefix_at_start) {
        strcpy(dst, cwd);
        dst += cwd_len;
    }

    // Copy path while removing jar prefixes
    while (*src) {
        int matched = 0;
        for (size_t i = 0; i < num_needles; ++i) {
            size_t needle_len = strlen(needles[i]);
            if (strncmp(src, needles[i], needle_len) == 0) {
                src += needle_len; // skip just the jar prefix
                matched = 1;
                break;
            }
        }
        if (!matched) {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    // If path starts with "/", check existence
    if (clean_path[0] == '/') {
        struct stat st;
        // Try original path
        if (stat(clean_path, &st) == 0) {
            // Path exists as-is, use it
            return clean_path;
        }
        
        // Try with "." prefix
        char* dot_path = (char*)malloc(strlen(clean_path) + 2);
        if (dot_path) {
            dot_path[0] = '.';
            strcpy(dot_path + 1, clean_path);
            
            if (stat(dot_path, &st) == 0) {
                // Dot version exists, use it
                free(clean_path);
                return dot_path;
            }
            free(dot_path);
        }
        // Neither exists, return original clean_path
    }

    return clean_path;
}
// Rewrites absolute /data/data/<pkg>/... to the host dataDir.
// Returns NULL if no rewrite; otherwise malloc'd, caller must keep alive.
#include "toml++/toml.hpp"
extern toml::table config;
extern "C" char* bd_redirect_datadir(const char* path)
{
    if (!path || path[0] != '/') return nullptr;
    static std::string prefix; static std::string real_root;
    static bool initialized = false;
    if (!initialized) {
        std::string pkg = config["package"]["packageName"].value_or<std::string>("");
        if (!pkg.empty()) prefix = "/data/data/" + pkg + "/";
        std::string dd = config["paths"]["android_data"].value_or<std::string>("../");
        if (char* abs = realpath(dd.c_str(), nullptr)) {
            real_root = abs;
            free(abs);
            if (!real_root.empty() && real_root.back() != '/') real_root.push_back('/');
        }
        initialized = true;
    }
    if (prefix.empty() || real_root.empty()) return nullptr;
    if (strncmp(path, prefix.c_str(), prefix.size()) != 0) return nullptr;
    std::string redirected = real_root + (path + prefix.size());
    BD_LOG("DATADIR", "redirect %s -> %s", path, redirected.c_str());
    return strdup(redirected.c_str());
}

// Rewrite Android's hard-coded font lookup paths into $cwd/fonts/ so each
// port can ship its own CJK fallback. Without this, Unity's FontEngine probes
// /etc/fonts.xml and /system/fonts/ — both empty on a Linux handheld — and
// renders non-Latin text blank.
extern "C" char* bd_redirect_system_fonts(const char* path)
{
    if (!path) return nullptr;

    const char* tail;  // what to append after $cwd/fonts/
    if      (strncmp(path, "/system/fonts/", 14) == 0)        tail = path + 14;
    else if (strcmp (path, "/system/fonts") == 0)             tail = "";
    else if (strcmp (path, "/etc/fonts.xml") == 0)            tail = "fonts.xml";
    else if (strcmp (path, "/etc/system_fonts.xml") == 0)     tail = "system_fonts.xml";
    else if (strcmp (path, "/vendor/etc/fallback_fonts.xml") == 0)
                                                              tail = "fallback_fonts.xml";
    else return nullptr;

    static std::string root;
    if (root.empty()) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return nullptr;
        root = std::string(cwd) + "/fonts";
    }

    std::string out = *tail ? root + "/" + tail : root;
    BD_LOG("FONTS", "%s -> %s", path, out.c_str());
    return strdup(out.c_str());
}

// Redirect Unity Analytics event writes to /dev/null. The game writes 30+
// tiny files/sec; without this they grind eMMC and contend for IO.
static const char* bd_kill_analytics(const char* path)
{
    static int cached = -1; // -1 = unknown, 0 = pass, 1 = block
    if (cached < 0) {
        cached = config["unity"]["block_analytics"].value_or<bool>(true) ? 1 : 0;
    }
    if (cached && path && strstr(path, "/Analytics/")) {
        return "/dev/null";
    }
    return path;
}

extern "C" const char* bd_kill_analytics_check(const char* path)
{
    return bd_kill_analytics(path);
}

static constexpr unsigned long long BD_MIN_FREE_BYTES = 32ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr unsigned long long BD_TOTAL_BYTES = 64ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr unsigned long BD_SPACE_BLOCK_SIZE = 4096;

template <typename StatT>
static unsigned long long bd_min_blocks(unsigned long long block_size)
{
    return (BD_MIN_FREE_BYTES + block_size - 1) / block_size;
}

static unsigned long long bd_total_blocks(unsigned long long block_size)
{
    return (BD_TOTAL_BYTES + block_size - 1) / block_size;
}

template <typename StatT>
static unsigned long long bd_statvfs_block_size(const StatT* st)
{
    unsigned long long block_size = st->f_frsize ? st->f_frsize : st->f_bsize;
    return block_size ? block_size : BD_SPACE_BLOCK_SIZE;
}

template <typename StatT>
static unsigned long long bd_statfs_block_size(const StatT* st)
{
    return st->f_bsize ? st->f_bsize : BD_SPACE_BLOCK_SIZE;
}

template <typename StatT>
static void bd_clamp_statvfs(StatT* st)
{
    unsigned long long block_size = bd_statvfs_block_size(st);
    st->f_bsize = block_size;
    st->f_frsize = block_size;
    if (st->f_bavail < bd_min_blocks<StatT>(block_size)) st->f_bavail = bd_min_blocks<StatT>(block_size);
    if (st->f_bfree < bd_min_blocks<StatT>(block_size)) st->f_bfree = bd_min_blocks<StatT>(block_size);
    if (st->f_blocks < bd_total_blocks(block_size)) st->f_blocks = bd_total_blocks(block_size);
}

template <typename StatT>
static void bd_clamp_statfs(StatT* st)
{
    unsigned long long block_size = bd_statfs_block_size(st);
    st->f_bsize = block_size;
    if (st->f_bavail < bd_min_blocks<StatT>(block_size)) st->f_bavail = bd_min_blocks<StatT>(block_size);
    if (st->f_bfree < bd_min_blocks<StatT>(block_size)) st->f_bfree = bd_min_blocks<StatT>(block_size);
    if (st->f_blocks < bd_total_blocks(block_size)) st->f_blocks = bd_total_blocks(block_size);
}

template <typename StatT>
static void bd_fill_mock_statvfs(StatT* st)
{
    memset(st, 0, sizeof(*st));
    st->f_bsize = BD_SPACE_BLOCK_SIZE;
    st->f_frsize = BD_SPACE_BLOCK_SIZE;
    st->f_bavail = bd_min_blocks<StatT>(BD_SPACE_BLOCK_SIZE);
    st->f_bfree = bd_min_blocks<StatT>(BD_SPACE_BLOCK_SIZE);
    st->f_blocks = bd_total_blocks(BD_SPACE_BLOCK_SIZE);
    st->f_namemax = 255;
}

template <typename StatT>
static void bd_fill_mock_statfs(StatT* st)
{
    memset(st, 0, sizeof(*st));
    st->f_bsize = BD_SPACE_BLOCK_SIZE;
    st->f_bavail = bd_min_blocks<StatT>(BD_SPACE_BLOCK_SIZE);
    st->f_bfree = bd_min_blocks<StatT>(BD_SPACE_BLOCK_SIZE);
    st->f_blocks = bd_total_blocks(BD_SPACE_BLOCK_SIZE);
    st->f_namelen = 255;
}

template <typename StatT, typename StatFn, typename FillFn, typename ClampFn>
static int bd_space_path_impl(const char* path, StatT* buf,
                              StatFn stat_fn, FillFn fill_fn, ClampFn clamp_fn)
{
    const char* query = path ? path : ".";
    int ret = stat_fn(query, buf);
    if (ret != 0) {
        ret = stat_fn(".", buf);
    }
    if (ret != 0) {
        fill_fn(buf);
        return 0;
    }
    clamp_fn(buf);
    return 0;
}

template <typename StatT, typename StatFn, typename FillFn, typename ClampFn>
static int bd_space_fd_impl(int fd, StatT* buf,
                            StatFn stat_fn, FillFn fill_fn, ClampFn clamp_fn)
{
    int ret = stat_fn(fd, buf);
    if (ret != 0) {
        fill_fn(buf);
        return 0;
    }
    clamp_fn(buf);
    return 0;
}

extern "C" ABI_ATTR int statvfs_impl(const char* path, struct statvfs* buf)
{
    return bd_space_path_impl(path, buf, statvfs,
                              bd_fill_mock_statvfs<struct statvfs>,
                              bd_clamp_statvfs<struct statvfs>);
}

extern "C" ABI_ATTR int statvfs64_impl(const char* path, struct statvfs64* buf)
{
    return bd_space_path_impl(path, buf, statvfs64,
                              bd_fill_mock_statvfs<struct statvfs64>,
                              bd_clamp_statvfs<struct statvfs64>);
}

extern "C" ABI_ATTR int fstatvfs_impl(int fd, struct statvfs* buf)
{
    return bd_space_fd_impl(fd, buf, fstatvfs,
                            bd_fill_mock_statvfs<struct statvfs>,
                            bd_clamp_statvfs<struct statvfs>);
}

extern "C" ABI_ATTR int fstatvfs64_impl(int fd, struct statvfs64* buf)
{
    return bd_space_fd_impl(fd, buf, fstatvfs64,
                            bd_fill_mock_statvfs<struct statvfs64>,
                            bd_clamp_statvfs<struct statvfs64>);
}

extern "C" ABI_ATTR int statfs_impl(const char* path, struct statfs* buf)
{
    return bd_space_path_impl(path, buf, statfs,
                              bd_fill_mock_statfs<struct statfs>,
                              bd_clamp_statfs<struct statfs>);
}

extern "C" ABI_ATTR int statfs64_impl(const char* path, struct statfs64* buf)
{
    return bd_space_path_impl(path, buf, statfs64,
                              bd_fill_mock_statfs<struct statfs64>,
                              bd_clamp_statfs<struct statfs64>);
}

extern "C" ABI_ATTR int fstatfs_impl(int fd, struct statfs* buf)
{
    return bd_space_fd_impl(fd, buf, fstatfs,
                            bd_fill_mock_statfs<struct statfs>,
                            bd_clamp_statfs<struct statfs>);
}

extern "C" ABI_ATTR int fstatfs64_impl(int fd, struct statfs64* buf)
{
    return bd_space_fd_impl(fd, buf, fstatfs64,
                            bd_fill_mock_statfs<struct statfs64>,
                            bd_clamp_statfs<struct statfs64>);
}

ABI_ATTR int open_impl(const char *filename, int flags, mode_t mode)
{
    verbose("NATIVE","Opening file %s",filename);
    filename = bd_kill_analytics(filename);

    char* redirected = bd_redirect_datadir(filename);
    // Leaked intentionally — bounded by one strdup per /data/data write open.
    if (redirected) filename = redirected;

    char* fonts_redirect = bd_redirect_system_fonts(filename);
    // Leaked intentionally — bounded by font count probed at startup.
    if (fonts_redirect) filename = fonts_redirect;

    if (filename && (strstr(filename, "playerprefs") || strstr(filename, "shared_prefs"))) {
        BD_DEBUG("PREFS-IO", "open(%s, flags=0x%x)", filename, flags);
    }
    if (filename && strstr(filename, "Settings.txt")) {
        BD_DEBUG("GRAPHICS-IO", "open(%s, flags=0x%x)", filename, flags);
    }

    // if (strcmp(filename,"/proc/cpuinfo") == 0)
    // {
    //     filename = "../support_files/cpuinfo.txt";
    //     verbose("NATIVE","Changing cpuinfo request to fake cpuinfo.txt");
    // }

    if (strcmp(filename,"/sys/devices/system/cpu/present") == 0)
    {
        filename = "../support_files/cpu_present.txt";
        verbose("NATIVE","Changing cpu/present request to fake cpu_present.txt");
    }

    if (strcmp(filename,"/sys/devices/system/cpu/possible") == 0)
    {
        filename = "../support_files/cpu_possible.txt";
        verbose("NATIVE","Changing cpu/possible request to fake cpu_possible.txt");
    }

    // if (strcmp(filename,"/proc/self/maps") == 0)
    // {
    //     verbose("NATIVE","No maps for you >:)");
    //     return -1;
    // }
    
    char* clean_path = clean_jar_path(filename);
    int fd = open(clean_path, flags, mode);
    verbose("NATIVE","Got file descriptor %d",fd);

    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) {
        BD_DEBUG("WROPEN", "open(%s, flags=0x%x) = %d",
                clean_path, flags, fd);
    }
    if (clean_path && (strstr(clean_path, "catalog.json") ||
                       strstr(clean_path, ".bundle") ||
                       strstr(clean_path, "settings.json") ||
                       strstr(clean_path, "/aa/") ||
                       strstr(clean_path, "AddressablesLink"))) {
        BD_DEBUG("ASSET", "open(%s, flags=0x%x) = %d %s",
                clean_path, flags, fd, fd >= 0 ? "OK" : "FAIL");
    }
    return fd;
}


ABI_ATTR ssize_t read_impl(int fd, void *buf, size_t count)
{
    verbose("NATIVE","reading %zu bytes from file %d",count,fd);
    int ret=read(fd, buf, count);
    // int i;
    // for (i = 0; i < count; i++)
    // {
    //     if (i > 0) printf(":");
    //     printf("%02X", ((char*)buf)[i]);
    // }
    // printf("\n");
    return ret;
}

ABI_ATTR 


ABI_ATTR ssize_t write_impl(int fd, void *buf, size_t count)
{
    // verbose("NATIVE","writing %zu bytes to file %d",count,fd);
    // int i;
    // for (i = 0; i < count; i++)
    // {
    //     if (i > 0) printf(":");
    //     printf("%02X", ((char*)buf)[i]);
    // }
    // printf("\n");
    return write(fd, buf, count);
}

ABI_ATTR int close_impl(int fd)
{
    verbose("NATIVE","Closing file %d",fd);
    return close(fd);
}

// Font-path thunks: redirect /system/fonts/ + /etc/*fonts.xml etc. before
// falling through to the normal path. Leaked strdup is bounded by ~10 font
// probes at startup, same pattern as bd_redirect_datadir in open_impl.
ABI_ATTR int access_impl(const char* path, int mode) {
    char* redir = bd_redirect_system_fonts(path);
    return access(redir ? redir : path, mode);
}

ABI_ATTR int faccessat_impl(int dirfd, const char* path, int mode, int flags) {
    char* redir = bd_redirect_system_fonts(path);
    return faccessat(dirfd, redir ? redir : path, mode, flags);
}

ABI_ATTR DIR* opendir_impl(const char* path) {
    char* redir = bd_redirect_system_fonts(path);
    if (redir) return opendir(redir);
    char* clean_path = clean_jar_path(path);
    if (!clean_path) return NULL;
    DIR* dir = opendir(clean_path);
    free(clean_path);
    return dir;
}

// fstatat_impl
ABI_ATTR int fstatat_impl(int dirfd, const char* path, struct stat* buf, int flags) {
    verbose("NATIVE","fstatat(%d, %s, flags=%d)", dirfd, path, flags);
    char* redir = bd_redirect_system_fonts(path);
    if (redir) return fstatat(dirfd, redir, buf, flags);
    char* clean_path = clean_jar_path(path);
    if (!clean_path) {
        return -1;
    }
    int ret = fstatat(dirfd, clean_path, buf, flags);
    free(clean_path);
    return ret;
}

// stat
ABI_ATTR int stat_impl(const char* path, struct stat* buf) {
    verbose("NATIVE", "stat(%s)", path);
    char* redir = bd_redirect_system_fonts(path);
    if (redir) return stat(redir, buf);
    char* clean_path = clean_jar_path(path);
    if (!clean_path) {
        return -1;
    }
    int ret = stat(clean_path, buf);
    free(clean_path);
    return ret;
}

// lstat
ABI_ATTR int lstat_impl(const char* path, struct stat* buf) {
    verbose("NATIVE","lstat(%s)", path);
    char* redir = bd_redirect_system_fonts(path);
    if (redir) return lstat(redir, buf);
    char* clean_path = clean_jar_path(path);
    if (!clean_path) {
        return -1;
    }
    int ret = lstat(clean_path, buf);
    free(clean_path);
    return ret;
}

// ABI_ATTR int chdir_bridge(const char* dir)
// {
//     verbose("NATIVE","Changing directory to %s",dir);
//     return chdir(dir);
// }


// //Maybe this could be int fd, int unused, long offset, int whence 
// //needs to be investigated
// ABI_ATTR long lseek64_bridge()
// {
//     //Father, forgive me for i have sinned
//     int stackPointer, fd, off_low, off_high;
//     asm volatile (
//         "mov %0, sp\n"
//         "mov %1, r0\n"
//         "mov %2, r2\n"
//         "mov %3, r3\n"
//         : "=r" (stackPointer), "=r" (fd), "=r" (off_low), "=r" (off_high)
//         :
//         : "memory", "r0", "r2", "r3"
//     );
//     int whence=*((int*)stackPointer+0x9);
//     //Do not put any lines of code before this point, this is very fragile
//     verbose("NATIVE","fd=%d whence=%d off_low=%d off_high=%d",fd,whence,off_low,off_high);    
//     long new_off=lseek64(fd,off_low+(off_high<<32),whence);
//     return new_off;
// }


/* flock() operation flags */
#ifndef LOCK_SH
#define LOCK_SH 1    /* shared lock */
#endif
#ifndef LOCK_EX
#define LOCK_EX 2    /* exclusive lock */
#endif
#ifndef LOCK_UN
#define LOCK_UN 8    /* unlock */
#endif
#ifndef LOCK_NB
#define LOCK_NB 4    /* non-blocking */
#endif

ABI_ATTR int flock_impl(int fd, int operation) {
    struct flock fl = {0};

    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;   /* whole file */

    switch (operation & (LOCK_SH | LOCK_EX | LOCK_UN)) {
        case LOCK_SH:
            fl.l_type = F_RDLCK;
            break;
        case LOCK_EX:
            fl.l_type = F_WRLCK;
            break;
        case LOCK_UN:
            fl.l_type = F_UNLCK;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    int cmd = (operation & LOCK_NB) ? F_SETLK : F_SETLKW;

    return fcntl(fd, cmd, &fl);
}

