#ifndef __BD_SHADER_CACHE_H__
#define __BD_SHADER_CACHE_H__

// Unity caches compiled GL programs under <port>/cache/UnityShaderCache and
// replays them with glProgramBinary on the next run. That path never calls
// glShaderSource, and the port rewrites Unity's video blit shader there (see
// thunks/khronos/gles2.cpp: samplerExternalOES -> sampler2D). A cache written by
// a build without the rewrite - or by a build whose rewrite has since changed -
// therefore contains a program that samples an external texture the loader can
// never fill: decode is healthy, uploads count up, and the video area stays
// black.
//
// The cache is a pure compile-time optimisation, so the loader stamps it with
// the rewrite version and throws it away when the stamp does not match. That
// keeps the (legitimate, rewritten) programs reusable across runs while making a
// stale cache impossible to keep.
//
// Bump BD_SHADER_REWRITE_VERSION whenever the video shader rewrite changes in a
// way that invalidates previously compiled programs.
#define BD_SHADER_REWRITE_VERSION 2

namespace bd_shader_cache {

// Searches the loader's working directory (and the configured game/data paths)
// for UnityShaderCache directories and removes the ones whose stamp is not the
// current rewrite version. Safe to call when nothing exists yet.
void prepare();

}

#endif
