// Host test for the SDL-shaped audio shim. Uses the GMLOADER_AUDIO_DDR file
// seam so no /dev/mem is needed, and never opens a real audio device.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mister_native_audio.h"
#include "native_audio_writer.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)){printf("FAIL %s (line %d)\n",#c,__LINE__);fails++;} } while(0)

#define REGION_SIZE 0x00100000u
#define RD_OFF      0x00000038u

static const char *PATH = "/tmp/gmloader-audio-shim-test";
static volatile uint8_t *g_map = NULL;

static void make_region(void) {
    int fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    if (ftruncate(fd, REGION_SIZE) != 0) { perror("ftruncate"); exit(1); }
    void *p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    g_map = (volatile uint8_t *)p;
    setenv("GMLOADER_AUDIO_DDR", PATH, 1);
    // Task 4 adds a pump thread that starts inside MisterAudio_Init. Keep it
    // off for the deterministic sections below -- a background pump racing the
    // test's own MisterAudio_PumpOnce() calls would make them flaky. Task 4
    // re-enables it for its own Init/Shutdown cycle at the end.
    setenv("GMLOADER_AUDIO_PUMP_THREAD", "0", 1);
}

// Stand in for the FPGA drain: mark the whole ring consumed.
static void drain_ring(void) {
    *(volatile uint32_t *)(g_map + RD_OFF) =
        *(volatile uint32_t *)(g_map + 0x30u);
}

int main(void) {
    make_region();
    CHECK(MisterAudio_Init());
    CHECK(MisterAudio_IsActive());

    // A push track at the sink's own format opens and echoes its spec back.
    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;
    MisterAudioTrack t = MisterAudio_Open(&want, &got);
    CHECK(t != 0);
    CHECK(got.freq == 48000 && got.format == AUDIO_S16SYS && got.channels == 2);
    CHECK(got.samples == 1024);           // caller's arithmetic is preserved

    // Queued bytes reflect staging only, and start empty.
    CHECK(MisterAudio_QueuedBytes(t) == 0);

    // 480 frames in == 1920 bytes staged (no conversion at 48k stereo S16).
    static int16_t pcm[480 * 2];
    for (int i = 0; i < 480 * 2; ++i) pcm[i] = (int16_t)(i & 0x7FFF);
    CHECK(MisterAudio_Queue(t, pcm, sizeof(pcm)) == 0);
    CHECK(MisterAudio_QueuedBytes(t) == sizeof(pcm));

    // Clear drops staging.
    MisterAudio_Clear(t);
    CHECK(MisterAudio_QueuedBytes(t) == 0);

    // A 44.1 kHz mono track is resampled and upmixed. Naively, 441 frames in
    // (10 ms) should stage ~480 frames (10 ms) of 48 kHz stereo -- but SDL's
    // resampler (SDL2 2.32.10, measured on this host) withholds a *fixed*
    // ~558-output-frame (~11.6 ms) FIR filter delay from the very first put,
    // independent of how much is queued. A single 441-frame put therefore
    // legitimately stages 0 bytes here -- that isn't conversion being broken,
    // it is the filter delay exceeding the block. To get a measurable,
    // non-vacuous signal in one call, queue 10x as much (4410 frames, 100 ms)
    // so the fixed delay is a small fraction of the block: measured staged
    // bytes are a deterministic, repeatable 16968 (verified across 3 runs).
    // The band below is tight around that real value, and comfortably
    // excludes both a stuck/broken track (0 bytes) and a bug that skips
    // conversion entirely and passes the raw mono bytes through untouched
    // (4410 * 2 = 8820 bytes, well under the lower bound).
    SDL_AudioSpec want44;
    SDL_zero(want44);
    want44.freq = 44100;
    want44.format = AUDIO_S16SYS;
    want44.channels = 1;
    want44.samples = 512;
    MisterAudioTrack t44 = MisterAudio_Open(&want44, NULL);
    CHECK(t44 != 0);
    static int16_t mono[4410];
    memset(mono, 0, sizeof(mono));
    CHECK(MisterAudio_Queue(t44, mono, sizeof(mono)) == 0);
    const uint32_t staged = MisterAudio_QueuedBytes(t44);
    CHECK(staged > 16000u && staged < 18000u);

    // Staging cap: flooding a track past 500 ms is refused, not buffered.
    static int16_t flood[48000 * 2];
    memset(flood, 0, sizeof(flood));
    int refusals = 0;
    for (int i = 0; i < 4; ++i)
        if (MisterAudio_Queue(t, flood, sizeof(flood)) != 0) refusals++;
    CHECK(refusals > 0);
    CHECK(MisterAudio_DroppedFrames() > 0);
    // The cap is checked before a put, so staging tops out at one oversized
    // batch beyond it -- bounded, which is the point, rather than unbounded.
    CHECK(MisterAudio_QueuedBytes(t) <= 256000u);

    // Staging cap flood for t44 (44.1 kHz mono): verify dropped frames are
    // counted in the track's own format, not the sink's. t44 has 2 bytes per
    // frame (44.1 kHz mono S16), so each 8820-byte buffer is 4410 frames.
    // With the fix, dropped frames should count 4410 per refused queue.
    // With the bug (dividing by sink's 4 bytes instead of track's 2),
    // it would count 2205 per refused queue -- off by a factor of 2.
    uint64_t t44_dropped_before = MisterAudio_DroppedFrames();
    static int16_t flood44[4410];  // 100 ms of 44.1 kHz mono
    memset(flood44, 0, sizeof(flood44));
    int t44_refusals = 0;
    for (int i = 0; i < 15; ++i) {
        if (MisterAudio_Queue(t44, flood44, sizeof(flood44)) != 0) {
            t44_refusals++;
        }
    }
    uint64_t t44_dropped_after = MisterAudio_DroppedFrames();
    uint64_t t44_dropped_delta = t44_dropped_after - t44_dropped_before;
    CHECK(t44_refusals > 0);
    CHECK(t44_dropped_delta > 0);
    // Each refused queue of 4410 frames (8820 bytes at 2 bytes/frame) should
    // increment dropped by 4410. With the bug, it would be 2205 per refusal.
    // Check that dropped is closer to the correct value: > t44_refusals * 3000
    // ensures it's above the buggy value (t44_refusals * 2205) but below the
    // correct value (t44_refusals * 4410), ruling out the buggy calculation.
    CHECK(t44_dropped_delta > (uint64_t)t44_refusals * 3000u);

    // Closing frees the slot so a later open succeeds.
    MisterAudio_Close(t);
    MisterAudio_Close(t44);
    CHECK(MisterAudio_QueuedBytes(t) == 0);            // stale handle is inert
    MisterAudioTrack t2 = MisterAudio_Open(&want, NULL);
    CHECK(t2 != 0);
    MisterAudio_Close(t2);

    drain_ring();
    MisterAudio_Shutdown();
    CHECK(!MisterAudio_IsActive());

    munmap((void *)g_map, REGION_SIZE);
    unlink(PATH);
    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
    return fails ? 1 : 0;
}
