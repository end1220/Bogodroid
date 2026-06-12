/*
 * shim_egl.c — 诊断 shim:hook eglGetProcAddress + eglQueryString,
 *              捕获 godot 启动时查询的每个 GL 函数名 / 扩展字符串。
 *
 * Mali libmali 闭源,有时对未知函数名/扩展名不返 NULL 而是 NULL deref。
 * LD_PRELOAD 这个 .so 进去,godot 调每个 eglGetProcAddress 都会先写一行
 * `[EGL-SHIM] query('xxx') -> 0xYYY` 到 stderr。SIGSEGV 之前最后一行
 * 就是死结函数名 —— 我们就知道要 patch godot 跳过哪个 GL 查询。
 *
 * 用法:
 *   LD_PRELOAD=/path/to/shim_egl.so ./godot --display-driver sdl2 ...
 *
 * 编译(aarch64-linux,在 docker arm64 容器或 zig cc 里):
 *   gcc -shared -fPIC -O0 shim_egl.c -o shim_egl.so -ldl
 *   或
 *   zig cc -target aarch64-linux-gnu -shared -fPIC shim_egl.c -o shim_egl.so -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *(*pfn_egl_get_proc_address)(const char *);
typedef const char *(*pfn_egl_query_string)(void *display, int name);

static pfn_egl_get_proc_address real_getproc = NULL;
static pfn_egl_query_string real_querystr = NULL;

/* 用 write(2,...) 直接 syscall — stdio 缓冲跟我们无关,SIGSEGV 也丢不了 */
static void shim_log(const char *prefix, const char *arg, void *ret) {
	char buf[512];
	int len;
	if (ret) {
		len = snprintf(buf, sizeof(buf), "[EGL-SHIM] %s('%s') -> %p\n",
				prefix, arg ? arg : "(null)", ret);
	} else {
		len = snprintf(buf, sizeof(buf), "[EGL-SHIM] %s('%s') -> NULL (Mali 不认 / 不支持)\n",
				prefix, arg ? arg : "(null)");
	}
	if (len > 0) {
		write(2, buf, len);
	}

	/* 可选:同时写 SHIM_LOG_FILE 文件(SIGSEGV 双保险)*/
	const char *file = getenv("EGL_SHIM_LOG_FILE");
	if (file && *file && len > 0) {
		extern int open(const char *, int, ...);
		extern int close(int);
		int fd = open(file, 0x0001 | 0x0040 | 0x0400, 0644); /* O_WRONLY|O_CREAT|O_APPEND */
		if (fd >= 0) {
			write(fd, buf, len);
			close(fd);
		}
	}
}

/* eglGetProcAddress 包装:godot 用它查 GL 函数指针 */
void *eglGetProcAddress(const char *procname) {
	if (!real_getproc) {
		real_getproc = (pfn_egl_get_proc_address)dlsym(RTLD_NEXT, "eglGetProcAddress");
		if (!real_getproc) {
			char err[] = "[EGL-SHIM] FATAL: dlsym(RTLD_NEXT, eglGetProcAddress) returned NULL\n";
			write(2, err, sizeof(err) - 1);
			return NULL;
		}
	}
	void *ret = real_getproc(procname);
	shim_log("eglGetProcAddress", procname, ret);

	/* 黑名单环境变量:如果列表里有这个函数名,强制返 NULL 给 glad
	 * (godot 不调,免触发 Mali bug)。冒号分隔。
	 * 用法:EGL_SHIM_BLACKLIST="glDispatchCompute:glObjectLabel" */
	const char *bl = getenv("EGL_SHIM_BLACKLIST");
	if (bl && *bl && procname && ret) {
		const char *p = bl;
		size_t namelen = strlen(procname);
		while (*p) {
			const char *colon = strchr(p, ':');
			size_t seglen = colon ? (size_t)(colon - p) : strlen(p);
			if (seglen == namelen && memcmp(p, procname, seglen) == 0) {
				char msg[] = "[EGL-SHIM] BLACKLIST hit, returning NULL\n";
				write(2, msg, sizeof(msg) - 1);
				return NULL;
			}
			if (!colon) {
				break;
			}
			p = colon + 1;
		}
	}
	return ret;
}

/* ===== GL 函数 hook(godot RasterizerGLES3::Config::Config 首批调用)===== */

typedef const unsigned char *(*pfn_glGetString)(unsigned int);
typedef void (*pfn_glGetIntegerv)(unsigned int, int *);
typedef unsigned int (*pfn_glGetError)(void);

static pfn_glGetString real_glGetString = NULL;
static pfn_glGetIntegerv real_glGetIntegerv = NULL;
static pfn_glGetError real_glGetError = NULL;

const unsigned char *glGetString(unsigned int name) {
	if (!real_glGetString) {
		real_glGetString = (pfn_glGetString)dlsym(RTLD_NEXT, "glGetString");
	}
	char buf[128];
	const char *nm;
	switch (name) {
		case 0x1F00: nm = "VENDOR"; break;
		case 0x1F01: nm = "RENDERER"; break;
		case 0x1F02: nm = "VERSION"; break;
		case 0x1F03: nm = "EXTENSIONS"; break;
		case 0x8B8C: nm = "SHADING_LANGUAGE_VERSION"; break;
		default: snprintf(buf, sizeof(buf), "0x%x", name); nm = buf;
	}
	int len = snprintf(buf, sizeof(buf), "[EGL-SHIM] glGetString(%s) -> ", nm);
	write(2, buf, len);
	const unsigned char *ret = real_glGetString ? real_glGetString(name) : NULL;
	if (ret) {
		write(2, "'", 1);
		write(2, (const char *)ret, strlen((const char *)ret));
		write(2, "'\n", 2);
	} else {
		write(2, "NULL\n", 5);
	}
	return ret;
}

void glGetIntegerv(unsigned int pname, int *params) {
	if (!real_glGetIntegerv) {
		real_glGetIntegerv = (pfn_glGetIntegerv)dlsym(RTLD_NEXT, "glGetIntegerv");
	}
	char buf[128];
	int len = snprintf(buf, sizeof(buf), "[EGL-SHIM] glGetIntegerv(pname=0x%x) called\n", pname);
	write(2, buf, len);
	if (real_glGetIntegerv) {
		real_glGetIntegerv(pname, params);
		len = snprintf(buf, sizeof(buf), "[EGL-SHIM] glGetIntegerv(0x%x) -> %d\n", pname, params ? *params : -1);
		write(2, buf, len);
	}
}

/* eglQueryString 包装:godot 用它查 EGL 客户端扩展、版本、vendor */
const char *eglQueryString(void *display, int name) {
	if (!real_querystr) {
		real_querystr = (pfn_egl_query_string)dlsym(RTLD_NEXT, "eglQueryString");
		if (!real_querystr) {
			char err[] = "[EGL-SHIM] FATAL: dlsym(RTLD_NEXT, eglQueryString) returned NULL\n";
			write(2, err, sizeof(err) - 1);
			return NULL;
		}
	}
	const char *ret = real_querystr(display, name);
	/* name: 0x3052=EXTENSIONS 0x3053=VERSION 0x3054=VENDOR 0x308E=CLIENT_APIS */
	char namebuf[32];
	const char *nstr;
	switch (name) {
		case 0x3052:
			nstr = "EXTENSIONS";
			break;
		case 0x3053:
			nstr = "VERSION";
			break;
		case 0x3054:
			nstr = "VENDOR";
			break;
		case 0x308E:
			nstr = "CLIENT_APIS";
			break;
		default:
			snprintf(namebuf, sizeof(namebuf), "0x%x", name);
			nstr = namebuf;
			break;
	}
	char buf[1024];
	int len = snprintf(buf, sizeof(buf), "[EGL-SHIM] eglQueryString(display=%p, %s) -> '%s'\n",
			display, nstr, ret ? ret : "(null)");
	if (len > 0) {
		write(2, buf, len);
	}
	return ret;
}
