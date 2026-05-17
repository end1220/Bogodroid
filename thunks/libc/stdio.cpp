#include <fcntl.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <bsd/stdio.h>

#include "platform.h"
#include "bionic_file.h"

#define _get_from(x) ((FILE*)((x)->_cookie))

BIONIC_FILE __sF_fake[3] = {
    {._cookie = stdin},
    {._cookie = stdout},
    {._cookie = stderr}
};

BIONIC_FILE *stdin_impl = &__sF_fake[0];
BIONIC_FILE *stdout_impl = &__sF_fake[1];
BIONIC_FILE *stderr_impl = &__sF_fake[2];

extern "C" char* bd_redirect_datadir(const char* path);  // from fcntl.cpp
extern "C" char* bd_redirect_system_fonts(const char* path); // from fcntl.cpp
extern "C" const char* bd_kill_analytics_check(const char* path); // from fcntl.cpp

#include <sys/stat.h>
#include <errno.h>

// On ENOENT for write-mode open, mkdir all parents then retry once.
// Unity ships shader cache writes to a non-existent ../cache/UnityShaderCache/.
static void bd_mkdir_parents(const char* path)
{
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return;
    memcpy(buf, path, len + 1);
    char* slash = strrchr(buf, '/');
    if (!slash || slash == buf) return;
    *slash = '\0';
    for (char* p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

ABI_ATTR BIONIC_FILE *fopen_impl(const char *arg1, const char* arg2)
{
    arg1 = bd_kill_analytics_check(arg1);

    char* redirected = bd_redirect_datadir(arg1);
    if (!redirected) redirected = bd_redirect_system_fonts(arg1);
    const char* path = redirected ? redirected : arg1;
    FILE *f = fopen(path, arg2);

    // Retry once after mkdir -p on ENOENT.
    if (!f && path && arg2 && errno == ENOENT &&
        (strchr(arg2, 'w') || strchr(arg2, 'a'))) {
        bd_mkdir_parents(path);
        f = fopen(path, arg2);
        if (f) {
            BD_LOG("MKPARENT", "auto-created parent for %s", path);
        }
    }

    if (path && (strstr(path, "catalog.json") || strstr(path, ".bundle")
                 || strstr(path, "settings.json")
                 || strstr(path, "/aa/")
                 || strstr(path, "AddressablesLink"))) {
        BD_DEBUG("ASSET", "fopen(%s, \"%s\") = %s",
                path, arg2 ? arg2 : "(null)", f ? "OK" : "FAIL");
    }
    if (arg2 && (strchr(arg2, 'w') || strchr(arg2, 'a') || strchr(arg2, '+'))) {
        BD_DEBUG("WROPEN", "fopen(%s, \"%s\") = %p",
                path ? path : "(null)", arg2, (void*)f);
    }
    if (path && (strstr(path, "playerprefs") || strstr(path, "shared_prefs"))) {
        BD_DEBUG("PREFS-IO", "fopen(%s, \"%s\") = %p",
                path, arg2 ? arg2 : "(null)", (void*)f);
    }
    if (path && strstr(path, "Settings.txt")) {
        BD_DEBUG("GRAPHICS-IO", "fopen(%s, \"%s\") = %s",
                path, arg2 ? arg2 : "(null)", f ? "OK" : "FAIL");
    }

    if (redirected) free(redirected);

    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
}

ABI_ATTR BIONIC_FILE *fopen64_impl(const char *arg1, const char* arg2)
{
    FILE *f = fopen64(arg1, arg2);
    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
}

ABI_ATTR BIONIC_FILE *fdopen_impl(int fildes, const char *mode)
{
    FILE *f = fdopen(fildes, mode);
    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
}

ABI_ATTR BIONIC_FILE *freopen_impl(const char * arg1, const char * arg2, BIONIC_FILE * _arg3)
{
    FILE *arg3 = _get_from(_arg3);
    _arg3->_cookie = freopen(arg1, arg2, arg3);
    if (!_arg3->_cookie) {
        free(_arg3);
        return NULL;
    }

    return _arg3;
}

ABI_ATTR BIONIC_FILE *freopen64_impl(const char * arg1, const char * arg2, BIONIC_FILE * _arg3)
{
    FILE *arg3 = _get_from(_arg3);
    _arg3->_cookie = freopen64(arg1, arg2, arg3);
    if (!_arg3->_cookie) {
        free(_arg3);
        return NULL;
    }

    return _arg3;
}

ABI_ATTR BIONIC_FILE *funopen_impl(const  void  *cookie,  int  (*readfn)(void  *,  char  *,	 int),
	   int	   (*writefn)(void     *,     const	char	 *,	 int),
	   off_t (*seekfn)(void *, off_t, int), int (*closefn)(void *))
{
    // TODO:: This is bad... we need to code a way to intermediate the callbacks!
    WARN_STUB
    return NULL;
/*
    FILE *f = funopen(cookie, readfn, writefn, seekfn, closefn);
    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
*/
}

ABI_ATTR BIONIC_FILE *fmemopen_impl(void *buf, size_t size, const char * mode)
{
    FILE *f = fmemopen(buf, size, mode);
    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
}

ABI_ATTR BIONIC_FILE *open_memstream_impl(char **bufp, size_t *sizep)
{
    FILE *f = open_memstream(bufp, sizep);
    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
}

ABI_ATTR BIONIC_FILE *open_wmemstream_impl(wchar_t **__bufloc, size_t *__sizeloc)
{
    FILE *f = open_wmemstream(__bufloc, __sizeloc);
    if (!f)
        return NULL;

    BIONIC_FILE *bridge = (BIONIC_FILE*)calloc(1, sizeof(BIONIC_FILE));
    bridge->_cookie = f;
    return bridge;
}

ABI_ATTR int fclose_impl(BIONIC_FILE * _arg1)
{
    int ret;
    FILE *arg1 = _get_from(_arg1);
    ret = fclose(arg1);
    free(_arg1);

    return ret;
}

ABI_ATTR BIONIC_FILE *popen_impl(const char *command, const char *mode)
{
    warning("Application attempted RCE: %s\n", command);
    return NULL;
}

ABI_ATTR BIONIC_FILE *tmpfile_impl()
{
    WARN_STUB
    return NULL;
}

ABI_ATTR BIONIC_FILE *tmpfile64_impl()
{
    WARN_STUB
    return NULL;
}

ABI_ATTR int fscanf_impl(BIONIC_FILE *fp, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int ret = vfscanf(_get_from(fp), fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int fprintf_impl(BIONIC_FILE *fp, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int ret = vfprintf(_get_from(fp), fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int printf_impl(const char *fmt, ...)
{
    int ret;
    va_list va;
    va_start(va, fmt);
    ret = vprintf(fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int sprintf_impl(char *s, const char *fmt, ...)
{
    int ret;
    va_list va;
    va_start(va, fmt);
    ret = vsprintf(s, fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int snprintf_impl(char *s, int max, const char *fmt, ...)
{
    int ret;
    va_list va;
    va_start(va, fmt);
    ret = vsnprintf(s, max, fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int vsnprintf_impl(char *s, int max, const char *fmt, va_list va)
{
    int ret;
    ret = vsnprintf(s, max, fmt, va);

    return ret;
}

ABI_ATTR int vsprintf_impl(char *s, const char *fmt, va_list va)
{
    int ret;
    ret = vsprintf(s, fmt, va);
    return ret;
}

ABI_ATTR int vasprintf_impl(char **s, const char *fmt, va_list args)
{
    int ret;
    ret = vasprintf(s, fmt, args);
    return ret;
}

ABI_ATTR int asprintf_impl(char **strp, const char *fmt, ...)
{
    int ret;
    va_list va;
    va_start(va, fmt);
    ret = vasprintf(strp, fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int sscanf_impl(const char *str, const char *fmt, ...)
{
    int ret;
    va_list va;
    va_start(va, fmt);
    ret = vsscanf(str, fmt, va);
    va_end(va);

    return ret;
}

ABI_ATTR int vprintf_impl(const char *fmt, va_list ap)
{
    return vprintf(fmt, ap);
}

ABI_ATTR char *__strncat_chk_impl(char *s1, const char *s2, size_t n, size_t s1len) {
	return strncat(s1, s2, n);
}

ABI_ATTR size_t strlcat_impl(char * dst, const char * src, size_t maxlen) {
    const size_t srclen = strlen(src);
    const size_t dstlen = strnlen(dst, maxlen);
    if (dstlen == maxlen) return maxlen+srclen;
    if (srclen < maxlen-dstlen) {
        memcpy(dst+dstlen, src, srclen+1);
    } else {
        memcpy(dst+dstlen, src, maxlen-1);
        dst[dstlen+maxlen-1] = '\0';
    }
    return dstlen + srclen;
}

ABI_ATTR size_t strlcpy_impl(char * dst, const char * src, size_t maxlen) {
    const size_t srclen = strlen(src);
    if (srclen < maxlen) {
        memcpy(dst, src, srclen+1);
    } else if (maxlen != 0) {
        memcpy(dst, src, maxlen-1);
        dst[maxlen-1] = '\0';
    }
    return srclen;
}

ABI_ATTR int swprintf_impl(wchar_t* buf, size_t n, const wchar_t* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int r = vswprintf(buf, n, fmt, va);
    va_end(va);
    return r;
}
