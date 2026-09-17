#ifndef __BD_VIDEO_H__
#define __BD_VIDEO_H__

#include <cstddef>
#include <cstdint>
#include <functional>

// VideoPlayer bridge for the emulated Android media stack.
//
// Unity's Android video player never reads decoded pixels itself. It hands a
// Surface to MediaCodec and samples the GL_TEXTURE_EXTERNAL_OES texture that a
// SurfaceTexture is supposed to feed from the codec's buffer queue:
//
//   AndroidVideoMedia --new SurfaceTexture(tex)------> SurfaceTexture
//                     --setOnFrameAvailableListener--> native callback
//                     --new Surface(SurfaceTexture)---> ANativeWindow
//                     --AMediaCodec_configure(window)-> codec renders into it
//
// Bogodroid has neither a BufferQueue nor gralloc, so nothing ever lands in
// that external texture and the video quad stays black while the UI keeps
// drawing. MediaCodec is a thunk (thunks/ndk/media.cpp), so the loop can be
// closed by hand:
//
//   1. media.cpp hands every decoded frame it releases to submit_i420();
//   2. bd_video converts it to RGBA, keeps it as the pending frame and runs the
//      guest's OnFrameAvailableListener so Unity starts its blit;
//   3. thunks/khronos/gles2.cpp redirects glBindTexture(GL_TEXTURE_EXTERNAL_OES,
//      <video texture>) to a private GL_TEXTURE_2D (backing_texture()) that
//      begin_upload()/end_upload() keeps current, and rewrites samplerExternalOES
//      to sampler2D in glShaderSource so the blit shader reads that texture.
//
// Every entry point is safe to call when no video is playing: the bridge then
// reports "nothing to do" and the loader behaves exactly as before.
namespace bd_video {

///// SurfaceTexture JNI stub (javastubs/android_view.cpp)

// SurfaceTexture(int texture) constructed. Note that Unity's argument is its own
// texture handle, NOT the GL texture name it later binds (observed: constructed
// with 22 while binding GL texture 176), so the bridge never keys off it.
void surface_texture_created(int texture_id);
// setOnFrameAvailableListener(): `notify` forwards a published frame to the
// guest listener. The most recent registration becomes the frame sink.
void register_sink(int texture_id, std::function<void()> notify);
// setOnFrameAvailableListener(null): the guest dropped its listener while the
// SurfaceTexture and the codec are still alive (Unity does this when it tears a
// clip's video pipeline down). Keeping the old closure would call a listener
// whose C++ owner is already gone - a virtual dispatch on a destroyed object,
// i.e. "Pure virtual function called!". Never notify without a live listener.
void clear_sink(int texture_id);
// setDefaultBufferSize(width, height).
void surface_texture_buffer_size(int texture_id, int width, int height);
// Surface(SurfaceTexture) constructed: the codec's ANativeWindow exists.
void surface_texture_attached(int texture_id);
void surface_texture_released(int texture_id);
// updateTexImage(): the guest consumed the last published frame.
void surface_texture_update_tex_image(int texture_id);

///// GL thunks (thunks/khronos/gles2.cpp)

// True while a guest SurfaceTexture is registered as a frame sink. On this
// device a GL_TEXTURE_EXTERNAL_OES can never carry decoder output, so every
// external bind during playback is redirected to backing_texture().
bool has_sink();
// Unity's last-registered texture handle, or -1 (diagnostics only).
int video_texture_name();
// GL_TEXTURE_2D the loader uploads frames into. gles2.cpp creates it on first
// use (the caller must have a current GL context) and stores the name here.
unsigned backing_texture();
void set_backing_texture(unsigned texture_name);
// Installed by gles2.cpp: binds the backing texture and uploads the newest
// decoded frame into it. Only gles2.cpp owns the GL entry points, so the
// SurfaceTexture stub calls back through this hook - exactly where a real
// SurfaceTexture does its glEGLImageTargetTexture2DOES work.
using UploadHook = void (*)();
void set_upload_hook(UploadHook hook);

// Locks the frame buffer and hands back the newest frame that has not been
// uploaded yet, or nullptr. The caller must call end_upload() once it is done
// reading - glTexImage2D in between is the whole point.
const uint8_t* begin_upload(int* width, int* height, uint64_t* serial);
void end_upload();

// A decoded frame finished travelling through the codec. `packed` is tightly
// packed I420 of `size` bytes (exactly what AMediaCodec_getOutputBuffer hands
// out). Returns true when a sink accepted it.
bool submit_i420(const uint8_t* packed, size_t size, int width, int height,
                 int64_t pts_us);

// Delivers a decoded frame to the guest's OnFrameAvailableListener. Must be
// called from the render thread with the GL context current (the loader calls it
// from eglSwapBuffers): the guest reacts with updateTexImage() and therefore our
// upload hook, which needs the context.
void pump();

// Tells the bridge how big the surface being presented is (called by the GL side
// on every swap). Kept for diagnostics and for callers that compare the video
// against the drawable; the bridge no longer downscales decoded frames to this
// size — resolution belongs in the offline encode before the APK is built.
void set_display_size(int width, int height);
// The size handed to set_display_size() (the presentation surface). 0 until the
// GL side has swapped once.
int display_width();
int display_height();

// Diagnostics: counts and last-seen sizes, logged by the memory tracer.
void log_state(const char* where);

}

#endif
