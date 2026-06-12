# shim_egl — godot GL 启动 hook

LD_PRELOAD shim,用于在闭源 Mali libmali 上 bisect "godot 启动哪个 GL 函数让 Mali NULL deref"。

## 编译(Mac M-series / Linux)

```bash
zig cc -target aarch64-linux-gnu -shared -fPIC -O2 -Wl,--strip-all shim_egl.c -o shim_egl.so
```

产物 ~5.8KB,GLIBC_2.17 only,链 libc + libdl,适用于任何 arm64 PortMaster handheld。

## 用法

```bash
LD_PRELOAD=/path/to/shim_egl.so \
EGL_SHIM_LOG_FILE=/tmp/shim.log \
EGL_SHIM_BLACKLIST="glDispatchCompute:glObjectLabel" \
./godot ...
```

- `EGL_SHIM_LOG_FILE`(可选):shim 同时把 hook log 写到这个文件(stderr 双保险)
- `EGL_SHIM_BLACKLIST`(可选,冒号分隔):shim 对这些 GL 函数名调 `eglGetProcAddress` 时强返 NULL,让 godot 的 glad 跳过 → 不调用 → 不触发 Mali bug

## hook 的函数

- `eglGetProcAddress(name)` — godot 调它查 GL 函数指针;每次记录 name + 返回值
- `eglQueryString(display, name)` — godot 调它查 EGL 扩展 / 版本 / vendor;记录返回字符串
- `glGetString(name)` — godot RasterizerGLES3 第一批调用,查 VENDOR/RENDERER/VERSION/EXTENSIONS
- `glGetIntegerv(pname, params)` — godot Config 构造里查 MAX_TEXTURE_SIZE 等

## 流程

1. 跑 godot + shim → 看 shim 输出最后几行
2. SIGSEGV 前最后一次 hook = Mali NULL deref 的罪魁
3. 加进 `EGL_SHIM_BLACKLIST` → 重跑
4. 死另一个?再加 → 循环到 godot 启动完
