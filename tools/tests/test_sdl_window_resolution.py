#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "thunks" / "egl_sdl" / "egl_sdl.cpp"


def main() -> int:
    src = SRC.read_text()
    func_pos = src.find("EGLDisplay eglGetDisplay_impl")
    create_pos = src.find('SDL_CreateWindow("Teapot"', func_pos)
    end_pos = src.find("EGLBoolean eglInitialize_impl", create_pos)
    if func_pos < 0 or create_pos < 0 or end_pos < 0:
        raise AssertionError("main EGL display/window initialization block was not found")

    window_block = src[func_pos:end_pos]
    assert 'config["device"]["displayWidth"]' in window_block
    assert 'config["device"]["displayHeight"]' in window_block
    assert "SDL_WINDOW_FULLSCREEN_DESKTOP" not in window_block
    assert "SDL_WINDOW_FULLSCREEN" in window_block
    assert "SDL_GetCurrentVideoDriver" in window_block
    assert "SDL_GL_GetDrawableSize" in window_block

    query_pos = src.find("EGLBoolean eglQuerySurface_impl")
    query_end = src.find("EGLContext eglCreateContext_impl", query_pos)
    if query_pos < 0 or query_end < 0:
        raise AssertionError("eglQuerySurface implementation block was not found")
    query_block = src[query_pos:query_end]
    assert "eglQuerySurface(EGL_WIDTH)" in query_block
    assert "eglQuerySurface(EGL_HEIGHT)" in query_block

    ndk = (ROOT / "thunks" / "ndk" / "ndk.cpp").read_text()
    assert "ANativeWindow_getWidth(%p) -> %d" in ndk
    assert "ANativeWindow_getHeight(%p) -> %d" in ndk
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
