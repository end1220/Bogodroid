// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
//
// See platform/common/audio_bus.h for why this exists at all.

#include "audio_bus.h"
#include "sys_volume.h"
#include "logging.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

namespace {

std::mutex g_mutex;

SDL_AudioDeviceID g_dev = 0;
int g_freq = 0;
int g_channels = 0;
int g_bits = 0;

std::atomic<int> g_owner{BD_AUDIO_OWNER_NONE};

struct Ring {
    std::deque<int16_t> q;
    size_t cap = 0;         // samples; 0 = unlimited
    uint64_t dropped = 0;   // samples discarded because the cap was reached
    uint64_t pushed = 0;    // samples ever handed over, for the state dump
    int peak = 0;           // largest |sample| since the last state dump, so
                            // "producing audio right now" can be told apart
                            // from "produced silence" and from "produced audio
                            // a minute ago"
};

Ring g_rings[BD_AUDIO_OWNER_COUNT];

std::vector<int16_t> g_scratch;
int g_gain = 100;
unsigned g_gain_tick = 0;
uint64_t g_mixed_bytes = 0;

// How deep the SDL queue is kept. Deep enough that Wwise's small refills cannot
// starve it, shallow enough that a movie's soundtrack does not drift away from
// its picture (the movie's PCM here is Unity's own mixer output, see
// audio_bus.h). Override with BD_AUDIO_QUEUE_MS while tuning.
uint32_t target_bytes_locked()
{
    if (g_freq <= 0 || g_channels <= 0)
        return 0;
    int ms = 75;
    if (const char* e = getenv("BD_AUDIO_QUEUE_MS")) {
        int n = atoi(e);
        if (n > 0 && n <= 1000)
            ms = n;
    }
    return (uint32_t)((int64_t)g_freq * g_channels * 2 * ms / 1000);
}

void reset_locked()
{
    for (Ring& r : g_rings) {
        r.q.clear();
        r.dropped = 0;
        r.pushed = 0;
        r.peak = 0;
    }
    g_mixed_bytes = 0;
    g_gain_tick = 0;
}

} // namespace

void bd_audio_bus_publish(SDL_AudioDeviceID dev, int freq, int channels, int bits)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dev = dev;
    g_freq = freq;
    g_channels = channels;
    g_bits = bits;
    reset_locked();
    BD_LOG("AUDIO", "bus published dev=%u %d Hz x %d ch x %d bit, queue target %u B",
           (unsigned)dev, freq, channels, bits, (unsigned)target_bytes_locked());
}

void bd_audio_bus_unpublish(SDL_AudioDeviceID dev)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dev == dev) {
        g_dev = 0;
        g_freq = 0;
        g_channels = 0;
        g_bits = 0;
        reset_locked();
    }
}

SDL_AudioDeviceID bd_audio_bus_device(int* freq, int* channels, int* bits)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (freq) *freq = g_freq;
    if (channels) *channels = g_channels;
    if (bits) *bits = g_bits;
    return g_dev;
}

void bd_audio_bus_set_owner(BdAudioOwner who)
{
    g_owner.store(who);
}

BdAudioOwner bd_audio_bus_owner(void)
{
    return (BdAudioOwner)g_owner.load();
}

void bd_audio_bus_push(BdAudioOwner who, const int16_t* pcm, size_t samples)
{
    if (!pcm || samples == 0)
        return;
    if ((int)who <= BD_AUDIO_OWNER_NONE || (int)who >= BD_AUDIO_OWNER_COUNT)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    Ring& r = g_rings[(int)who];

    r.pushed += samples;
    for (size_t i = 0; i < samples; i++) {
        int v = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];
        if (v > r.peak)
            r.peak = v;
    }

    // A producer that has fallen behind must lose its oldest audio rather than
    // push playback later and later behind the picture.
    if (r.cap && r.q.size() + samples > r.cap) {
        size_t excess = r.q.size() + samples - r.cap;
        if (excess >= r.q.size()) {
            r.dropped += r.q.size();
            r.q.clear();
        } else {
            r.q.erase(r.q.begin(), r.q.begin() + excess);
            r.dropped += excess;
        }
    }
    r.q.insert(r.q.end(), pcm, pcm + samples);
}

size_t bd_audio_bus_available(BdAudioOwner who)
{
    if ((int)who <= BD_AUDIO_OWNER_NONE || (int)who >= BD_AUDIO_OWNER_COUNT)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_rings[(int)who].q.size();
}

void bd_audio_bus_set_capacity(BdAudioOwner who, size_t samples)
{
    if ((int)who <= BD_AUDIO_OWNER_NONE || (int)who >= BD_AUDIO_OWNER_COUNT)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rings[(int)who].cap = samples;
}

size_t bd_audio_bus_clear(BdAudioOwner who)
{
    if ((int)who <= BD_AUDIO_OWNER_NONE || (int)who >= BD_AUDIO_OWNER_COUNT)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    Ring& r = g_rings[(int)who];
    size_t n = r.q.size();
    r.q.clear();
    return n;
}

size_t bd_audio_bus_pump(SDL_AudioDeviceID dev)
{
    if (dev == 0)
        return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (dev != g_dev)
        return 0;
    const uint32_t target = target_bytes_locked();
    if (target == 0 || g_channels <= 0)
        return 0;

    // System volume is applied here and only here, so it reaches every engine.
    // Polling the backend (a sysfs file on Anbernic hardware) every ~80 ms keeps
    // the key handler's writes visible without reading a file per callback.
    if ((++g_gain_tick & 15u) == 0)
        bd_sys_volume_poll();
    g_gain = bd_sys_volume_percent();

    Ring& fmod  = g_rings[BD_AUDIO_OWNER_FMOD];
    Ring& wwise = g_rings[BD_AUDIO_OWNER_WWISE];

    size_t written = 0;
    for (int guard = 0; guard < 64; guard++) {
        const uint32_t queued = SDL_GetQueuedAudioSize(dev);
        if (queued >= target)
            break;

        size_t need = (size_t)(target - queued) / sizeof(int16_t);
        need -= need % (size_t)g_channels;
        if (need == 0)
            break;

        const size_t avail = std::max(fmod.q.size(), wwise.q.size());
        if (avail == 0)
            break; // both dry -- do not pad the queue with silence

        size_t n = std::min(need, avail);
        n -= n % (size_t)g_channels;
        if (n == 0)
            break;

        g_scratch.resize(n);
        for (size_t i = 0; i < n; i++) {
            int32_t v = 0;
            if (i < fmod.q.size())
                v += fmod.q[i];
            if (i < wwise.q.size())
                v += wwise.q[i];
            if (g_gain != 100)
                v = v * g_gain / 100;
            if (v > 32767)
                v = 32767;
            else if (v < -32768)
                v = -32768;
            g_scratch[i] = (int16_t)v;
        }

        const size_t take_f = std::min(n, fmod.q.size());
        const size_t take_w = std::min(n, wwise.q.size());
        if (take_f)
            fmod.q.erase(fmod.q.begin(), fmod.q.begin() + take_f);
        if (take_w)
            wwise.q.erase(wwise.q.begin(), wwise.q.begin() + take_w);

        if (SDL_QueueAudio(dev, g_scratch.data(), (Uint32)(n * sizeof(int16_t))) != 0)
            break;
        written += n * sizeof(int16_t);
    }

    g_mixed_bytes += written;

    // Rate-limited state dump: one line per ~1 s of audio work. pushed is
    // cumulative and peak is per-window, so a movie soundtrack shows up here as
    // "pushed grew and peak is non-zero" even when the ring happens to be empty
    // at the sampling instant -- and a track that has gone quiet goes back to
    // peak=0 instead of being hidden by an old maximum.
    static uint64_t pumps = 0;
    if ((++pumps % 400) == 0) {
        BD_LOG("AUDIO",
               "mix %llu B | fmod pushed=%llu smp peak(1s)=%d (q=%zu drop=%llu) | "
               "wwise pushed=%llu smp peak(1s)=%d (q=%zu drop=%llu) | queued=%u/%u B | gain=%d%%",
               (unsigned long long)g_mixed_bytes,
               (unsigned long long)fmod.pushed, fmod.peak,
               fmod.q.size(), (unsigned long long)fmod.dropped,
               (unsigned long long)wwise.pushed, wwise.peak,
               wwise.q.size(), (unsigned long long)wwise.dropped,
               (unsigned)SDL_GetQueuedAudioSize(dev), (unsigned)target, g_gain);
        fmod.peak = 0;
        wwise.peak = 0;
    }

    return written;
}
