// Host test for the SDL-shaped audio shim. Uses the GMLOADER_AUDIO_DDR file
// seam so no /dev/mem is needed, and never opens a real audio device.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
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

// Stand in for a PARTIAL FPGA drain: consume exactly `frames` from the ring,
// leaving the rest as backlog. drain_ring() cannot express "the ring still
// holds a cushion", which is the whole subject of the starvation tests.
static void consume_frames(size_t frames) {
    volatile uint32_t *rd = (volatile uint32_t *)(g_map + RD_OFF);
    *rd = (*rd + (uint32_t)(frames * NA_BYTES_PER_FRAME)) & 0xFFFFu;
}

// Top the ring up to the pump's target from empty.
static void fill_ring_to_target(void) {
    for (int i = 0; i < 8 && MisterAudio_PumpOnce() > 0; ++i) {}
}

int main(void) {
    make_region();
    CHECK(MisterAudio_Init());
    CHECK(MisterAudio_IsActive());
    // Deterministic sections run with the pump thread disabled.
    CHECK(!MisterAudio_ThreadActive());

    // A push track at the sink's own format opens and echoes its spec back.
    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq = NA_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL;
    MisterAudioTrack t = MisterAudio_Open(&want, &got);
    CHECK(t != 0);
    CHECK(got.freq == NA_SAMPLE_RATE && got.format == AUDIO_S16SYS &&
          got.channels == 2);
    CHECK(got.samples == 1024);           // caller's arithmetic is preserved

    // Queued bytes reflect staging only, and start empty.
    CHECK(MisterAudio_QueuedBytes(t) == 0);

    // 480 frames in == 1920 bytes staged: a track at the sink's own rate is an
    // identity copy, which is the whole point of carrying the native rate in the
    // ring -- no resampler runs on the A9 for the game's own audio.
    static int16_t pcm[480 * 2];
    for (int i = 0; i < 480 * 2; ++i) pcm[i] = (int16_t)(i & 0x7FFF);
    CHECK(MisterAudio_Queue(t, pcm, sizeof(pcm)) == 0);
    CHECK(MisterAudio_QueuedBytes(t) == sizeof(pcm));

    // Clear drops staging.
    MisterAudio_Clear(t);
    CHECK(MisterAudio_QueuedBytes(t) == 0);

    // A 44.1 kHz mono track is resampled and upmixed -- now DOWN to the sink's
    // 22.05 kHz rather than up to 48 kHz. Naively, 4410 frames in (100 ms)
    // should stage 2205 frames (100 ms) of 22.05 kHz stereo == 8820 bytes. SDL's
    // resampler (SDL2 2.32.10, measured on this host) withholds a fixed ~11.6 ms
    // FIR filter delay from the very first put, which at 22.05 kHz is ~512
    // frames == 2048 bytes, so the measured value is a deterministic 6772.
    // (At 48 kHz the same 11.6 ms was ~558 frames and the value was 16968; the
    // delay is constant in TIME, so it rescales with the sink rate.)
    //
    // The band below is tight around the real value and excludes both failure
    // directions: a stuck/broken track (0 bytes) below, and a bug that skips
    // conversion and passes the raw mono bytes through untouched (4410 * 2 =
    // 8820 bytes) above. Note the passthrough value now sits ABOVE the correct
    // one -- downsampling produces FEWER bytes than the input, where upsampling
    // produced more -- so the upper bound is doing real work here.
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
    CHECK(staged > 6000u && staged < 8000u);

    // Staging cap: flooding a track past 500 ms is refused, not buffered.
    static int16_t flood[NA_SAMPLE_RATE * 2];
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
    // Each refused queue of 4410 frames (8820 bytes at 2 bytes/frame) is
    // counted in the track's own format. With the fix, dropped frames counts
    // len / track->bytes_per_frame = 8820 / 2 = 4410 frames per refusal.
    // With the bug (using sink's 4 bytes/frame instead), it would count
    // 8820 / 4 = 2205 per refusal. The exact check discriminates the two.
    CHECK(t44_dropped_delta == (uint64_t)t44_refusals * 4410u);

    // --- Pump ------------------------------------------------------------
    // Start clean: drop the flood staged above and drain what the ring holds.
    MisterAudio_Clear(t);
    MisterAudio_Clear(t44);
    drain_ring();

    // With every track paused the pump still feeds the ring, so the DAC sees
    // real silence rather than a held DC level.
    CHECK(MisterAudio_PumpOnce() > 0);
    CHECK(NativeAudioWriter_FreeFrames() < NativeAudioWriter_CapacityFrames());

    // Pump until topped up to kTargetFillFrames, then a further pass must be a
    // no-op. (At 48 kHz the target exceeded the 4096-frame per-pass cap and this
    // needed several passes; at the native rate it may top up in one. Either way
    // the invariant under test is the same: it converges, then stops.)
    for (int i = 0; i < 8 && MisterAudio_PumpOnce() > 0; ++i) {}
    CHECK(MisterAudio_PumpOnce() == 0);

    // Unpause and stage a known full-scale ramp; the pump must consume it.
    drain_ring();
    MisterAudio_Pause(t, 0);
    // 50 ms, comfortably under kTargetFillFrames (100 ms) so a single pass
    // drains it. Hardcoding 2400 encoded "half the 48 kHz target" implicitly.
    static const int kToneFrames = NA_SAMPLE_RATE / 20;
    static int16_t tone[(NA_SAMPLE_RATE / 20) * 2];
    for (int i = 0; i < kToneFrames * 2; ++i) tone[i] = 1000;
    CHECK(MisterAudio_Queue(t, tone, sizeof(tone)) == 0);
    CHECK(MisterAudio_QueuedBytes(t) == sizeof(tone));
    CHECK(MisterAudio_PumpOnce() > 0);
    CHECK(MisterAudio_QueuedBytes(t) == 0);       // staging drained into ring

    // Two unpaused tracks sum. Feed both the same constant and check the ring
    // holds the doubled value, saturating rather than wrapping.
    drain_ring();
    MisterAudio_Pause(t44, 0);
    SDL_AudioSpec want2;
    SDL_zero(want2);
    want2.freq = NA_SAMPLE_RATE; want2.format = AUDIO_S16SYS; want2.channels = 2;
    want2.samples = 1024;
    MisterAudioTrack tb = MisterAudio_Open(&want2, NULL);
    CHECK(tb != 0);
    MisterAudio_Pause(tb, 0);
    static int16_t loud[480 * 2];
    for (int i = 0; i < 480 * 2; ++i) loud[i] = 20000;
    CHECK(MisterAudio_Queue(t, loud, sizeof(loud)) == 0);
    CHECK(MisterAudio_Queue(tb, loud, sizeof(loud)) == 0);
    const uint32_t wr_before =
        *(volatile uint32_t *)(g_map + 0x30u) & 0xFFFFu;
    CHECK(MisterAudio_PumpOnce() > 0);
    // 20000 + 20000 saturates to 32767, never wraps to a negative sample.
    const int16_t *ring =
        (const int16_t *)(const void *)(g_map + 0x000D0000u + wr_before);
    CHECK(ring[0] == 32767);
    CHECK(ring[1] == 32767);
    MisterAudio_Close(tb);
    MisterAudio_Pause(t44, 1);
    MisterAudio_Pause(t, 1);

    // --- Starvation hold-off ---------------------------------------------
    // The ring is a 100 ms latency cushion. A producer that is briefly late
    // must be covered BY that cushion; splicing silence into a playing
    // track's stream instead spends the cushion on nothing and puts an
    // audible gap in the music. It also hides the shortfall from gm_audio's
    // slew loop, which reads ring occupancy as its only rate signal.
    MisterAudio_Clear(t);
    MisterAudio_Clear(t44);
    drain_ring();

    // Fill to target with every track paused -- that is the one case where
    // silence IS the right submit, so the setup exercises it too.
    fill_ring_to_target();
    CHECK(MisterAudio_PumpOnce() == 0);

    MisterAudio_Pause(t, 0);
    const uint64_t starved0 = MisterAudio_StarvedFrames();

    // 10 ms drained, ~90 ms of backlog left, staging dry: submit nothing.
    consume_frames(NA_SAMPLE_RATE / 100);
    CHECK(MisterAudio_PumpOnce() == 0);
    CHECK(MisterAudio_StarvedFrames() == starved0);

    // Room for 10 ms but only 64 frames staged: submit the 64 real frames,
    // not 64 real frames followed by a silent tail.
    static const size_t kShortFrames = 64;
    static int16_t shortbuf[64 * 2];
    for (size_t i = 0; i < kShortFrames * 2; ++i) shortbuf[i] = 500;
    CHECK(MisterAudio_Queue(t, shortbuf, sizeof(shortbuf)) == 0);
    CHECK(MisterAudio_PumpOnce() == kShortFrames);
    CHECK(MisterAudio_StarvedFrames() == starved0);

    // Cushion spent: below the floor the FPGA FIFO really would run dry and
    // hold its last sample as DC, so silence is correct here -- and counted,
    // because on device a non-zero rate is the starvation signal.
    {
        const size_t used_now = NativeAudioWriter_CapacityFrames() -
                                NativeAudioWriter_FreeFrames();
        CHECK(used_now > 100);
        consume_frames(used_now - 100);
        CHECK(MisterAudio_PumpOnce() > 0);
        CHECK(MisterAudio_StarvedFrames() > starved0);
    }
    MisterAudio_Pause(t, 1);

    // --- Blocking-write high-water ---------------------------------------
    // android.media.AudioTrack.write(WRITE_BLOCKING) blocks until the data is
    // ACCEPTED into the track's buffer, not until it has been played. The
    // mark below is that buffer expressed in sink bytes; waiting for staging
    // to reach ZERO instead would drain the producer's slack to nothing on
    // every chunk and manufacture the gap the section above guards against.
    // t: 1024 frames at the sink's own rate -> 1024 sink frames.
    CHECK(MisterAudio_StagingHighWater(t) == 1024u * NA_BYTES_PER_FRAME);
    // t44: 512 frames at 44.1 kHz -> 256 sink frames == 1024 bytes, under the
    // 20 ms floor, so the floor is what a caller with a tiny buffer gets.
    CHECK(MisterAudio_StagingHighWater(t44) ==
          (uint32_t)(NA_SAMPLE_RATE / 50) * NA_BYTES_PER_FRAME);
    CHECK(MisterAudio_StagingHighWater(0) == 0u);   // closed handle: no wait

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
    CHECK(!MisterAudio_ThreadActive());

    // --- Thread ----------------------------------------------------------
    // A second cycle with the pump thread enabled: it must keep the ring fed
    // with nobody calling PumpOnce, and must join cleanly on shutdown.
    setenv("GMLOADER_AUDIO_PUMP_THREAD", "1", 1);
    CHECK(MisterAudio_Init());
    CHECK(MisterAudio_ThreadActive());
    drain_ring();
    struct timespec nap = { 0, 50 * 1000 * 1000 };   // 50 ms
    nanosleep(&nap, NULL);
    CHECK(NativeAudioWriter_FreeFrames() < NativeAudioWriter_CapacityFrames());
    MisterAudio_Shutdown();
    CHECK(!MisterAudio_ThreadActive());

    munmap((void *)g_map, REGION_SIZE);
    unlink(PATH);
    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
    return fails ? 1 : 0;
}
