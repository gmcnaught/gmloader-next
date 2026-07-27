//
//  MiSTer native audio shim -- gmloader
//
//  See mister_native_audio.h. This file owns the track table and the
//  conversion streams; the pump and its thread are added on top.
//
//  Copyright (C) 2026 -- GPL-3.0
//

#include "mister_native_audio.h"
#include "native_audio_writer.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace {

// 500 ms of the sink format: 48000 * 4 * 500 / 1000.
const uint32_t kStagingCapBytes =
    (uint32_t)NA_SAMPLE_RATE * NA_BYTES_PER_FRAME * MISTER_AUDIO_STAGING_CAP_MS / 1000u;

struct Track {
    bool             open;
    bool             paused;
    bool             pull;          // callback-driven (FMOD) vs queue-driven
    SDL_AudioSpec    spec;          // exactly what the caller asked for
    SDL_AudioStream *conv;          // caller format -> 48k stereo S16
};

Track     g_tracks[MISTER_AUDIO_MAX_TRACKS];
bool      g_active = false;
uint64_t  g_dropped = 0;

// Guards the track table against the pump's mix pass. Non-recursive, never
// nested -- the same role as Solarus's audio_mutex.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

Track *track_of(MisterAudioTrack t) {
    if (t <= 0 || t > MISTER_AUDIO_MAX_TRACKS) return nullptr;
    Track *tr = &g_tracks[t - 1];
    return tr->open ? tr : nullptr;
}

}  // namespace

bool MisterAudio_Init(void) {
    if (g_active) return true;
    memset(g_tracks, 0, sizeof(g_tracks));
    g_dropped = 0;
    if (!NativeAudioWriter_Init()) {
        fprintf(stderr,
            "MisterAudio: /dev/mem unavailable, falling back to SDL devices\n");
        return false;
    }
    g_active = true;
    fprintf(stderr, "MisterAudio: native audio active (48000 Hz stereo S16)\n");
    return true;
}

void MisterAudio_Shutdown(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MISTER_AUDIO_MAX_TRACKS; ++i) {
        if (g_tracks[i].conv) SDL_FreeAudioStream(g_tracks[i].conv);
        g_tracks[i].conv = nullptr;
        g_tracks[i].open = false;
    }
    g_active = false;
    pthread_mutex_unlock(&g_lock);
    NativeAudioWriter_Shutdown();
}

bool MisterAudio_IsActive(void) { return g_active; }

MisterAudioTrack MisterAudio_Open(const SDL_AudioSpec *desired,
                                  SDL_AudioSpec *obtained) {
    if (!g_active || !desired) return 0;

    SDL_AudioStream *conv = SDL_NewAudioStream(
        desired->format, desired->channels, desired->freq,
        AUDIO_S16SYS, NA_CHANNELS, NA_SAMPLE_RATE);
    if (!conv) {
        fprintf(stderr, "MisterAudio: SDL_NewAudioStream failed: %s\n",
                SDL_GetError());
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MISTER_AUDIO_MAX_TRACKS; ++i)
        if (!g_tracks[i].open) { slot = i; break; }
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        SDL_FreeAudioStream(conv);
        fprintf(stderr, "MisterAudio: no free track slot\n");
        return 0;
    }

    Track *tr = &g_tracks[slot];
    tr->open   = true;
    tr->paused = true;              // SDL opens devices paused
    tr->pull   = (desired->callback != nullptr);
    tr->spec   = *desired;
    tr->conv   = conv;
    pthread_mutex_unlock(&g_lock);

    // Echo the request back verbatim so caller-side arithmetic is unchanged.
    if (obtained) *obtained = *desired;

    fprintf(stderr, "MisterAudio: track %d open (%d Hz, %d ch, %s)\n",
            slot + 1, desired->freq, desired->channels,
            tr->pull ? "pull" : "push");
    return slot + 1;
}

void MisterAudio_Close(MisterAudioTrack t) {
    pthread_mutex_lock(&g_lock);
    Track *tr = track_of(t);
    if (tr) {
        if (tr->conv) SDL_FreeAudioStream(tr->conv);
        tr->conv = nullptr;
        tr->open = false;
        tr->paused = true;
    }
    pthread_mutex_unlock(&g_lock);
}

int MisterAudio_Queue(MisterAudioTrack t, const void *data, uint32_t len) {
    if (!data || len == 0) return 0;
    pthread_mutex_lock(&g_lock);
    Track *tr = track_of(t);
    if (!tr || tr->pull) { pthread_mutex_unlock(&g_lock); return -1; }

    if ((uint32_t)SDL_AudioStreamAvailable(tr->conv) >= kStagingCapBytes) {
        g_dropped += len / NA_BYTES_PER_FRAME;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int rc = SDL_AudioStreamPut(tr->conv, data, (int)len);
    pthread_mutex_unlock(&g_lock);
    return rc == 0 ? 0 : -1;
}

uint32_t MisterAudio_QueuedBytes(MisterAudioTrack t) {
    pthread_mutex_lock(&g_lock);
    Track *tr = track_of(t);
    uint32_t n = tr ? (uint32_t)SDL_AudioStreamAvailable(tr->conv) : 0u;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void MisterAudio_Clear(MisterAudioTrack t) {
    pthread_mutex_lock(&g_lock);
    Track *tr = track_of(t);
    if (tr) SDL_AudioStreamClear(tr->conv);
    pthread_mutex_unlock(&g_lock);
}

void MisterAudio_Pause(MisterAudioTrack t, int pause_on) {
    pthread_mutex_lock(&g_lock);
    Track *tr = track_of(t);
    if (tr) tr->paused = (pause_on != 0);
    pthread_mutex_unlock(&g_lock);
}

uint64_t MisterAudio_DroppedFrames(void) { return g_dropped; }
