#define _GNU_SOURCE 1
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

#include <atomic>
#include <sched.h>      /* cpu_set_t / CPU_SET, under _GNU_SOURCE */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

namespace {

// MISTER_AUDIO_STAGING_CAP_MS of the sink format.
const uint32_t kStagingCapBytes =
    (uint32_t)NA_SAMPLE_RATE * NA_BYTES_PER_FRAME * MISTER_AUDIO_STAGING_CAP_MS / 1000u;

// Solarus's constants (mister_native_audio.cpp:56-57,105): keep ~100 ms of
// audio queued in the ring, and never render more than 16 KiB in one pass.
//
// Derived from NA_SAMPLE_RATE, not hardcoded: this was 4800, which read as
// "100 ms" only while the sink was 48 kHz and would have silently become 218 ms
// at the native rate.
//
// LOCKSTEP: gm_audio's TARGET_QW parameter is this in QWORDS (two frames per
// qword), and its slew loop pulls ring occupancy toward that value.
const size_t kTargetFillFrames = NA_SAMPLE_RATE / 10;
const size_t kMaxFramesPerPass = 4096;

// Backlog below which the pump pads a short pass with silence even though a
// track is playing. Above it the ring covers the producer's jitter; below it
// the FPGA FIFO is close enough to running dry that a held-DC artifact is the
// worse of the two, so silence wins.
//
// 25 ms of the 100 ms target: it absorbs a ~75 ms producer stall without a
// single spliced sample, and the resulting occupancy dip is what gm_audio's
// slew loop is FOR -- it reads ring occupancy as its rate signal and answers
// with at most +/-0.21% of pitch, which is inaudible where a gap is not.
const size_t kStarveFloorFrames = NA_SAMPLE_RATE / 40;

// Smallest staging allowance handed to a blocking writer, whatever buffer it
// asked for: a caller that opened a tiny AudioTrack still needs enough slack
// to mix its next chunk without the pump going dry behind it.
const uint32_t kMinHighWaterBytes =
    (uint32_t)(NA_SAMPLE_RATE / 50) * NA_BYTES_PER_FRAME;

int16_t g_mixbuf[kMaxFramesPerPass * NA_CHANNELS];
int16_t g_tmpbuf[kMaxFramesPerPass * NA_CHANNELS];
// Scratch for pull tracks, in the TRACK's format. Sized for the worst case
// this shim accepts: 4 bytes/frame at the highest rate any caller asks for.
uint8_t g_pullbuf[kMaxFramesPerPass * 8];

int16_t sat_add_s16(int16_t a, int16_t b) {
    int32_t s = (int32_t)a + (int32_t)b;
    if (s >  32767) return  32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

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
uint64_t  g_starved = 0;

// Diagnostic escape hatch: restores the unconditional silence pad so the two
// behaviours can be compared on one device build. Read once at Init -- the
// pump reads it every pass and getenv is not something to put on that path.
bool      g_always_pad = false;

// Guards the track table against the pump's mix pass. Non-recursive, never
// nested -- the same role as Solarus's audio_mutex.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

Track *track_of(MisterAudioTrack t) {
    if (t <= 0 || t > MISTER_AUDIO_MAX_TRACKS) return nullptr;
    Track *tr = &g_tracks[t - 1];
    return tr->open ? tr : nullptr;
}

// Ask a pull track's callback for enough source bytes to cover `want_bytes`
// of sink output, and push the result through its converter.
void fill_pull_track(Track *tr, int want_bytes) {
    const int src_frame_bytes = SDL_AUDIO_BITSIZE(tr->spec.format) / 8 *
                                tr->spec.channels;
    if (src_frame_bytes <= 0) return;

    // Sink frames -> source frames, rounded up.
    const int want_frames = want_bytes / NA_BYTES_PER_FRAME;
    long src_frames = ((long)want_frames * tr->spec.freq + NA_SAMPLE_RATE - 1) /
                      NA_SAMPLE_RATE;
    long src_bytes = src_frames * src_frame_bytes;
    if (src_bytes > (long)sizeof(g_pullbuf)) src_bytes = sizeof(g_pullbuf);
    if (src_bytes <= 0) return;

    // Callbacks expect a fully-initialised buffer; SDL guarantees silence.
    SDL_memset(g_pullbuf, SDL_AUDIO_ISSIGNED(tr->spec.format) ? 0 : 0x80,
               (size_t)src_bytes);
    tr->spec.callback(tr->spec.userdata, g_pullbuf, (int)src_bytes);
    SDL_AudioStreamPut(tr->conv, g_pullbuf, (int)src_bytes);
}

pthread_t g_pump_tid;
// ATOMIC, not a plain bool: this is the pump loop's exit condition. A plain
// non-atomic flag read in a spin loop may legally be hoisted out of the loop by
// the compiler, so the pump could never observe Shutdown()'s store and would
// hang the join forever.
std::atomic<bool> g_pump_running{false};

// Pin a thread to one core. Linux-only; returns false elsewhere (host tests).
// Returns true if pinning succeeded, false if it failed or is unavailable.
bool pin_to_core(pthread_t th, int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(th, sizeof(set), &set) != 0) {
        fprintf(stderr, "MisterAudio: pin to core %d failed\n", cpu);
        return false;
    }
    return true;
#else
    (void)th; (void)cpu;
    return false;
#endif
}

bool pinning_enabled(void) {
    const char *v = getenv("GMLOADER_AUDIO_PIN");
    if (v && v[0] == '0') return false;
    return sysconf(_SC_NPROCESSORS_ONLN) >= 2;
}

// GMLOADER_AUDIO_PUMP_THREAD=0 keeps the pump off the clock so a caller (the
// host test) can drive MisterAudio_PumpOnce() deterministically.
bool pump_thread_enabled(void) {
    const char *v = getenv("GMLOADER_AUDIO_PUMP_THREAD");
    return !(v && v[0] == '0');
}

void *pump_main(void *) {
    // Ring-driven: the FPGA drain is the clock. PumpOnce refills TO a fixed
    // level, so the long-run submit rate equals the drain rate and the pitch is
    // exact. gm_audio closes the other half of that loop -- it slews its own
    // consumption toward kTargetFillFrames of occupancy -- so neither side has
    // to assume the other's rate. Sleep only when there is nothing to do.
    const struct timespec idle = { 0, 1000 * 1000 };   // 1 ms
    // Starvation report. The FPGA cannot raise this: gm_audio's underflow
    // counter stays at zero whenever the host pads, because a padded ring
    // never actually runs dry. Silent while the number is zero.
    uint64_t last_starved = 0;
    struct timespec last_report = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &last_report);

    while (g_pump_running.load(std::memory_order_acquire)) {
        if (MisterAudio_PumpOnce() != 0) continue;
        nanosleep(&idle, nullptr);

        const uint64_t now_starved = g_starved;
        if (now_starved == last_starved) continue;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long elapsed_s = now.tv_sec - last_report.tv_sec;
        if (elapsed_s < 5) continue;
        fprintf(stderr, "MisterAudio: starved %llu frames (%.1f ms/s) over %lds\n",
                (unsigned long long)(now_starved - last_starved),
                (double)(now_starved - last_starved) * 1000.0 /
                    ((double)NA_SAMPLE_RATE * (double)elapsed_s),
                elapsed_s);
        last_starved = now_starved;
        last_report = now;
    }
    return nullptr;
}

}  // namespace

bool MisterAudio_Init(void) {
    if (g_active) return true;
    memset(g_tracks, 0, sizeof(g_tracks));
    g_dropped = 0;
    g_starved = 0;
    {
        const char *v = getenv("GMLOADER_AUDIO_SILENCE_PAD");
        g_always_pad = (v && v[0] == '1');
        if (g_always_pad)
            fprintf(stderr, "MisterAudio: silence padding forced on (A/B)\n");
    }
    if (!NativeAudioWriter_Init()) {
        fprintf(stderr,
            "MisterAudio: /dev/mem unavailable, falling back to SDL devices\n");
        return false;
    }
    g_active = true;

    if (!pump_thread_enabled()) {
        fprintf(stderr, "MisterAudio: pump thread disabled by env\n");
    } else {
        g_pump_running = true;
        if (pthread_create(&g_pump_tid, nullptr, pump_main, nullptr) != 0) {
            g_pump_running = false;
            fprintf(stderr, "MisterAudio: pump thread failed to start\n");
        } else if (pinning_enabled()) {
            // The fabric backend pure-spins on C_DONE by default and pegs the
            // thread it runs on, so keep the pump off that core entirely.
            bool pump_pinned = pin_to_core(g_pump_tid, 1);
            bool main_pinned = pin_to_core(pthread_self(), 0);
            if (pump_pinned && main_pinned) {
                fprintf(stderr,
                        "MisterAudio: pump pinned to core 1, main to core 0\n");
            } else {
                fprintf(stderr, "MisterAudio: pump thread running unpinned\n");
            }
        }
    }

    fprintf(stderr, "MisterAudio: native audio active (%d Hz stereo S16)\n",
            NA_SAMPLE_RATE);
    return true;
}

void MisterAudio_Shutdown(void) {
    // Join before anything is torn down: no mix may be in flight over a dead
    // mapping. Mirrors Solarus stopping the thread at the top of Sound::quit().
    if (g_pump_running.exchange(false, std::memory_order_release)) {
        pthread_join(g_pump_tid, nullptr);
    }

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
        const int src_frame_bytes = SDL_AUDIO_BITSIZE(tr->spec.format) / 8 * tr->spec.channels;
        g_dropped += (src_frame_bytes > 0) ? (len / (uint32_t)src_frame_bytes) : 0u;
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

uint64_t MisterAudio_StarvedFrames(void) { return g_starved; }

size_t MisterAudio_PumpOnce(void) {
    if (!g_active) return 0;

    const size_t cap  = NativeAudioWriter_CapacityFrames();
    const size_t freeF = NativeAudioWriter_FreeFrames();
    const size_t used = cap - freeF;
    if (used >= kTargetFillFrames) return 0;

    size_t want = kTargetFillFrames - used;
    if (want > kMaxFramesPerPass) want = kMaxFramesPerPass;
    if (want > freeF) want = freeF;
    if (want == 0) return 0;

    const int want_bytes = (int)(want * NA_BYTES_PER_FRAME);

    pthread_mutex_lock(&g_lock);

    memset(g_mixbuf, 0, (size_t)want_bytes);

    int mixed = 0;
    int have_bytes = 0;         // real audio in the mix, in bytes
    bool any_playing = false;
    for (int i = 0; i < MISTER_AUDIO_MAX_TRACKS; ++i) {
        Track *tr = &g_tracks[i];
        if (!tr->open || tr->paused || !tr->conv) continue;
        any_playing = true;

        if (tr->pull && SDL_AudioStreamAvailable(tr->conv) < want_bytes)
            fill_pull_track(tr, want_bytes);

        int got = SDL_AudioStreamGet(tr->conv,
                                     mixed == 0 ? g_mixbuf : g_tmpbuf,
                                     want_bytes);
        if (got <= 0) {
            // Nothing available from this track this pass. It contributes
            // silence to the mix; whether that silence is SUBMITTED is decided
            // below, from the ring backlog.
            continue;
        }
        if (mixed == 0) {
            // First contributor wrote straight into the mix buffer; zero any
            // shortfall so the tail is silence rather than stale samples.
            if (got < want_bytes)
                memset((uint8_t *)g_mixbuf + got, 0, (size_t)(want_bytes - got));
        } else {
            const int n = got / 2;   // samples, not frames
            for (int s = 0; s < n; ++s)
                g_mixbuf[s] = sat_add_s16(g_mixbuf[s], g_tmpbuf[s]);
        }
        if (got > have_bytes) have_bytes = got;
        ++mixed;
    }

    pthread_mutex_unlock(&g_lock);

    const size_t have = (size_t)have_bytes / NA_BYTES_PER_FRAME;

    // Nothing playing: silence is the correct submit and always has been. The
    // FPGA FIFO holds its last sample when starved, so a dry ring parks the
    // DAC at a DC level rather than at zero.
    if (!any_playing || g_always_pad) {
        if (any_playing && have < want) g_starved += want - have;
        return NativeAudioWriter_Submit(g_mixbuf, want);
    }

    // A track is playing and its producer is short this pass. While the ring
    // still holds a real cushion, submit only what is real and let the backlog
    // cover the rest -- that is what the cushion is for, and a short submit is
    // invisible where spliced silence is not. Only once the backlog falls
    // through the floor does padding become the lesser artifact.
    if (have >= want) return NativeAudioWriter_Submit(g_mixbuf, want);
    if (used >= kStarveFloorFrames)
        return have ? NativeAudioWriter_Submit(g_mixbuf, have) : 0;

    g_starved += want - have;
    return NativeAudioWriter_Submit(g_mixbuf, want);
}

uint32_t MisterAudio_StagingHighWater(MisterAudioTrack t) {
    pthread_mutex_lock(&g_lock);
    Track *tr = track_of(t);
    uint32_t hw = 0;
    if (tr) {
        // The caller's buffer, in ITS frames, restated in sink frames.
        long sink_frames = 0;
        if (tr->spec.freq > 0)
            sink_frames = ((long)tr->spec.samples * NA_SAMPLE_RATE) /
                          tr->spec.freq;
        long bytes = sink_frames * NA_BYTES_PER_FRAME;
        if (bytes < (long)kMinHighWaterBytes) bytes = kMinHighWaterBytes;
        // Never let the mark reach the staging cap, or a blocking write would
        // wait on a level MisterAudio_Queue is already refusing to reach.
        if (bytes > (long)(kStagingCapBytes / 2))
            bytes = (long)(kStagingCapBytes / 2);
        hw = (uint32_t)bytes;
    }
    pthread_mutex_unlock(&g_lock);
    return hw;
}

bool MisterAudio_ThreadActive(void) {
    return g_pump_running.load(std::memory_order_acquire);
}
