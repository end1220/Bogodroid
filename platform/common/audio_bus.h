// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
//
// Shared SDL audio output device + software mixer.
//
// SDL2 permits exactly one output device per process: a second
// SDL_OpenAudioDevice() fails with "Audio device already open", whether it asks
// for the default device or for a device by name. Measured against SDL 2.0.10
// with the dummy driver; ALSA/OSS go through the same single-device bookkeeping.
//
// A Wwise title has two independent consumers for that one device:
//
//   * Unity's built-in FMOD (projects/unityloader/javastubs/fakefmod.cpp) opens
//     it first, very early, and pumps PCM in from libunity's fmodProcess().
//     Unity's *whole* audio system lives behind that call, so this is also the
//     path a VideoPlayer's soundtrack takes: with audioOutputMode = AudioSource
//     (Unity's default) the movie's AAC track never reaches AudioTrack, it is
//     mixed by Unity and comes out of fmodProcess.
//   * Wwise's OpenSL sink (thunks/opensles/opensles.cpp) wants it later. If it
//     cannot have it, the buffer-queue refill callback never fires, Wwise's
//     hardware watchdog expires, and the engine drops to
//     "Hardware audio subsystem stopped responding. Silent mode is enabled."
//
// SDL_QueueAudio() is a plain FIFO, so two engines pushing into it directly
// interleave their streams into noise. The first cut of this file therefore let
// one engine own the queue and muted the other, which is wrong on two counts:
//
//   * a movie soundtrack (FMOD) is silent for as long as Wwise holds the queue,
//     because fmodProcess() stops being called at all -- Unity's mixer is idle,
//     so the clip never advances;
//   * whichever engine is muted also loses its share of the system volume, so
//     the volume keys visibly do nothing to the engine that is still playing.
//
// So the device now has a single software mixer in front of it. Both engines
// push their PCM into their own ring through bd_audio_bus_push(); the mixer
// takes one block off each, adds them sample by sample with saturation, applies
// the system volume once, and hands the result to SDL_QueueAudio(). Each engine
// keeps its own clock and its own backlog, and neither can starve the other.
//
// The mixer runs on whichever producer thread calls bd_audio_bus_pump() first;
// both the FMOD loop and Wwise's refill pump call it every few milliseconds, and
// the ring mutex makes that safe.

#pragma once

#include <SDL2/SDL.h>
#include <cstddef>
#include <cstdint>

enum BdAudioOwner {
    BD_AUDIO_OWNER_NONE  = 0,
    BD_AUDIO_OWNER_FMOD  = 1,
    BD_AUDIO_OWNER_WWISE = 2,
    BD_AUDIO_OWNER_COUNT = 3,
};

// Publisher side: whoever successfully called SDL_OpenAudioDevice. `freq` /
// `channels` / `bits` describe the device's *actual* configuration, which both
// the producers and the mixer need in order to match it.
void bd_audio_bus_publish(SDL_AudioDeviceID dev, int freq, int channels, int bits);
void bd_audio_bus_unpublish(SDL_AudioDeviceID dev);

// Consumer side: the already-open device, or 0 when nobody has one. Any of the
// out-parameters may be NULL.
SDL_AudioDeviceID bd_audio_bus_device(int* freq, int* channels, int* bits);

// Bookkeeping only -- "which engine touched the device last". The mixer runs
// both engines at once, so this no longer gates anything.
void bd_audio_bus_set_owner(BdAudioOwner who);
BdAudioOwner bd_audio_bus_owner(void);

// ───────── Producer side ─────────

// Append interleaved S16 PCM, already at the device's rate and channel count.
// Samples past the engine's backlog cap are dropped oldest-first so a slow
// consumer can never push playback behind live audio.
void bd_audio_bus_push(BdAudioOwner who, const int16_t* pcm, size_t samples);

// Samples currently buffered for one engine. A producer that keeps its backlog
// under one refill's worth of audio never lets the mixer run dry.
size_t bd_audio_bus_available(BdAudioOwner who);

// Backlog cap in samples (0 = unlimited).
void bd_audio_bus_set_capacity(BdAudioOwner who, size_t samples);

// Throw away everything buffered for one engine (player destroyed, queue
// cleared). Returns how many samples were dropped.
size_t bd_audio_bus_clear(BdAudioOwner who);

// ───────── Consumer side ─────────

// Mix both engines, apply the system volume, and top the SDL queue up to its
// target depth. Returns the number of bytes handed to SDL, 0 when there was
// nothing to play or the queue was already deep enough. Safe to call from both
// producer threads.
size_t bd_audio_bus_pump(SDL_AudioDeviceID dev);
