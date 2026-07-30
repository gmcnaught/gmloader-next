#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <zip.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <errno.h>

#include "platform.h"
#include "so_util.h"
#include "io_util.h"
#include "jni.h"
#include "jni_internals.h"
#include "classes/RunnerJNILib.h"
#include "khronos/gles2.h"
#include "libyoyo.h"
#include "configuration.h"
#include "texture.h"
#include "video.h"
#include "splash.h"

#ifdef MISTER_NATIVE_VIDEO
#include <dlfcn.h>
#include <libgen.h>
#include "mister/native_video_writer.h"
#include "mister/frame_capture.h"
#include "mister/draw_trace.h"
#include "mister/blitter.h"
#include "mister/raster_backend.h"
#include "mister/mister_native_audio.h"
#include "mister/bench_godmode.h"
// Global handle to bundled libGLES_sw.so — also used by egl.cpp and gles2.cpp via extern
void* g_gles_handle = nullptr;

// Task 7: GMLOADER_RASTER=mfgpu device wiring. backend_mfgpu / RasterBackend_Select
// live in raster_backend_{mfgpu,sw}.cpp; RasterBackend_MFGPU_GetFB565 hands back
// the fabric's already-RGB565 scanout buffer so it can go straight to
// NativeVideoWriter_WriteFrame, skipping Blitter_ToRGB565 (see the present block
// below).
extern "C" const RasterBackend backend_mfgpu;
extern "C" const uint16_t *RasterBackend_MFGPU_GetFB565(int *w, int *h);

// ── [Phase 3 Stage B] Display-derived frame CAP ──────────────────────────────
// The cap exists because the blitter can run the GM VM far above the display
// rate, which races the game's logic/intro and triggers relaunch loops. What it
// replaced was a wall-clock `SDL_Delay` cap on a 1 ms grid at frame_ms =
// 1000/60 = 16 (integer truncation → it actually targeted 62.5 fps, against a
// real scanout period of 16.6882 ms), and whose catch-up branch is followed by
// an unconditional `next_ms += frame_ms`, so tripping it produced a FULL
// frame_ms sleep rather than none. Measured cost on .62: 19.79 ms capped vs
// 18.09 ms with `GMLOADER_FPS=0`, fabric identical — ~1.7 ms/frame.
//
// This is a CAP, NOT a vsync SYNC, and the distinction is the whole design:
// the loop body is ~18.09 ms against a 16.6882 ms scanout period, so a classic
// "block until the next scanout frame" would miss every window and land on the
// following one — 33.38 ms, ~30 fps, far worse than the 50.5 fps it replaces.
// The gate is a LEAKY BUCKET over the scanout counter, not a per-frame test.
// Scanout boundaries are banked as credit (bucket depth FCAP_BURST_DEFAULT
// frames), each game frame spends one, and a frame waits only when the bucket
// is empty:
//   • loop faster than scanout → the bucket drains, every frame waits, the
//     long-run rate is pinned at the true 59.9228 Hz (the cap does its job)
//   • loop at or slower than scanout → boundaries arrive faster than they are
//     spent → ZERO wait, i.e. exactly the `GMLOADER_FPS=0` behaviour
// A game frame can never get more than FCAP_BURST_DEFAULT frames ahead of the
// display, so this caps the RATE while staying immune to the loop's shape.
//
// The bucket is why this is not the simpler "has the counter advanced since
// this frame began?" test. That test was built first and measured on .62: even
// at the steady quiet pose it waited on 37 % of frames for a mean 6.6-7.7 ms,
// delivering 19.05 ms/frame — better than the 19.79 ms it replaced but well
// short of the 18.09 ms the loop runs at uncapped. The cause is the Phase 2
// host lever: with the C_DONE await moved to the publish site the loop is
// PIPELINED, so frame times alternate short/long around an 18.09 ms mean. A
// per-frame test pads every short frame while the long ones stay long, so it
// raises the mean it was supposed to leave alone. Banking the extra boundary a
// long frame crossed and spending it on the short frame that follows removes
// that penalty without loosening the long-run cap.
//
// GMLOADER_FPS: 0 disables the cap entirely (unchanged — the benches rely on
// it); an explicit positive N keeps the old wall-clock cap at N fps (a numeric
// fps is a wall-clock request); unset/default = this scanout cap, with the old
// 16 ms wall-clock cap as the fallback if the instrument is unavailable or
// stops advancing. A hang here bricks the game loop, so every wait is bounded.
// GMLOADER_FCAP_BURST overrides the bucket depth (1 = the strict per-frame test
// above; larger allows a longer burst before the cap bites).
//
// Note that the GM refresh rate is resolved independently of which cap is in
// force: `GMLOADER_FPS=0` and `GMLOADER_FPS=N` still hand GM the measured
// 59.9228 Hz when the instrument reads sanely. Only the PACING differs.
//
// The bucket discards surplus credit (it clamps at `burst`), so the cap is
// one-sided by construction: a loop that runs fast but is punctuated by hitches
// cannot spend the boundaries those hitches crossed beyond the bucket's depth,
// and so settles slightly BELOW 59.9228 Hz rather than averaging up to it. That
// is the intended direction — a rate cap must never let a stall license a later
// overshoot — and `GMLOADER_FCAP_BURST` is the knob if a scene wants more slack.
enum {
    FCAP_STALL_MS       = 50,      // give up on ONE frame's wait after this (≈3 periods)
    FCAP_STALL_MAX      = 20,      // consecutive stalled frames before permanent demotion
    FCAP_CYC_MIN        = 492187,  // sane scan_period_cyc band: 5 ms …
    FCAP_CYC_MAX        = 4921875, // … 50 ms at clk_sys 98.4375 MHz
    FCAP_BURST_DEFAULT  = 2,       // bucket depth, in scanout frames (see above)
    FCAP_RESOLVE_ATTEMPTS = 240,   // resolve retries before giving up (~120 frames)
};
static const double FCAP_CLK_SYS_HZ = 98437500.0;   // clk_sys — the counter's clock

enum FCapMode { FCAP_UNSET = 0, FCAP_OFF, FCAP_SCANOUT, FCAP_TICKS };
static FCapMode  g_fcap_mode      = FCAP_UNSET;
static Uint32    g_fcap_frame_ms  = 0;      // wall-clock fallback period (0 = no cap)
static float     g_fcap_refresh   = 0.0f;   // measured scanout Hz; 0 = not resolved yet
static uint32_t  g_fcap_last_scan = 0;
static bool      g_fcap_have_last = false;
static int       g_fcap_stall_run = 0;
static int32_t   g_fcap_credit    = 0;      // banked scanout boundaries (leaky bucket)
static int32_t   g_fcap_burst     = FCAP_BURST_DEFAULT;
// Instrument (GMLOADER_FCAP_STAT=1): proves the cap engages rather than idling.
static int       g_fcap_stat      = -1;
static uint32_t  g_fcap_frames    = 0, g_fcap_waits = 0;
static double    g_fcap_wait_ms   = 0.0;

// Poll interval inside the wait, in microseconds (GMLOADER_FCAP_POLL_US;
// 0 = pure spin, the default). Same shape and same reasoning as the backend's
// GMLOADER_MFGPU_POLL_US: one uncached control-word read costs ~1.2 us, so a
// spin releases within ~1 us of the scanout boundary, whereas even a 100 us
// usleep is rounded up by the kernel's timer slack and would systematically
// overshoot the 16.6882 ms period by ~1 %. Poll traffic at that rate was
// already A/B'd on device against the fabric and is not what it waits on.
static long fcap_poll_us(void) {
    static long v = -1;
    if (v < 0) { const char *e = getenv("GMLOADER_FCAP_POLL_US"); v = (e && *e) ? atol(e) : 0; if (v < 0) v = 0; }
    return v;
}

static bool fcap_period_sane(uint32_t cyc) {
    return cyc >= (uint32_t)FCAP_CYC_MIN && cyc <= (uint32_t)FCAP_CYC_MAX;
}
static bool fcap_scanout_read(uint32_t *cnt, uint32_t *cyc) {
    if (RasterBackend_Select() != &backend_mfgpu) return false;
    if (!RasterBackend_MFGPU_ScanoutRead(cnt, cyc)) return false;
    // Fault injection for the fallback path (GMLOADER_FCAP_FREEZE=<ms>): from
    // that many ms of SDL uptime on, pin the reported count, so "the counter
    // stopped advancing" can be exercised on real hardware without wedging one.
    // Unset (the default) costs one comparison. Diagnostic only.
    static long freeze = -2; static uint32_t frozen = 0;
    if (freeze == -2) { const char *e = getenv("GMLOADER_FCAP_FREEZE"); freeze = (e && *e) ? atol(e) : -1; }
    if (freeze >= 0 && cnt && (long)SDL_GetTicks() >= freeze) {
        if (!frozen) frozen = *cnt ? *cnt : 1u;
        *cnt = frozen;
    }
    return true;
}

// Idempotent, cheap, and safe to call before the mapping exists: the fabric
// back-end only mmaps /dev/mem on its first use (inside frame 1's Process), so
// the first call or two legitimately fail and simply retry next frame.
static void fcap_resolve(void) {
    if (g_fcap_stat < 0) {
        const char *s = getenv("GMLOADER_FCAP_STAT");
        g_fcap_stat = (s && *s && *s != '0') ? 1 : 0;
    }
    if (g_fcap_mode == FCAP_UNSET) {
        const char *fe = getenv("GMLOADER_FPS");
        int fps = fe ? atoi(fe) : 60;
        if (fps <= 0) {                       // GMLOADER_FPS=0 — no cap at all
            g_fcap_frame_ms = 0;
            g_fcap_mode = FCAP_OFF;
            warning("frame-cap: disabled (GMLOADER_FPS=0)\n");
            return;
        }
        g_fcap_frame_ms = (Uint32)(1000 / fps);
        if (fe && *fe) {                      // explicit fps = explicit wall clock
            g_fcap_mode = g_fcap_frame_ms ? FCAP_TICKS : FCAP_OFF;
            warning("frame-cap: wall-clock cap at %d fps (%u ms) — GMLOADER_FPS set explicitly\n",
                    fps, (unsigned)g_fcap_frame_ms);
            return;
        }
    }
    if (g_fcap_mode != FCAP_UNSET && g_fcap_refresh != 0.0f) return;

    uint32_t cnt = 0, cyc = 0;
    const bool readable = fcap_scanout_read(&cnt, &cyc);
    if (!readable || !fcap_period_sane(cyc)) {
        // Three cases, one shared budget:
        //   • not readable YET — the fabric back-end only mmaps /dev/mem on its
        //     first use, inside frame 1's Process, so the first calls legitimately
        //     fail;
        //   • not readable at all — sw back-end, or no /dev/mem;
        //   • readable but reporting a period outside 5–50 ms — these words are
        //     not the instrument (mismatched/older RBF), not a slow display.
        // The third case used to retry forever with no log line, which is a silent
        // undiagnosable stall in resolution; it now gives up and says so, like the
        // other two. Retrying costs two uncached loads.
        //
        // This counts resolve ATTEMPTS, not frames: fcap_resolve() runs twice per
        // frame (fcap_refresh_hz() at the top of the loop, fcap_wait() at the
        // bottom), so the budget is ~half as many frames. Reported as attempts
        // rather than converted, because that ratio is an implementation detail.
        static uint32_t attempts = 0;
        if (++attempts > (uint32_t)FCAP_RESOLVE_ATTEMPTS && g_fcap_refresh == 0.0f) {
            if (g_fcap_mode == FCAP_UNSET)
                g_fcap_mode = g_fcap_frame_ms ? FCAP_TICKS : FCAP_OFF;
            g_fcap_refresh = 60.0f;
            warning("frame-cap: scanout counter %s after %u attempts — %s, "
                    "GM refresh 60.00 Hz\n",
                    readable ? "reporting an implausible period" : "unavailable",
                    (unsigned)attempts,
                    g_fcap_frame_ms ? "falling back to the wall-clock cap"
                                    : "no cap in force");
        }
        return;
    }
    g_fcap_refresh = (float)(FCAP_CLK_SYS_HZ / (double)cyc);
    if (g_fcap_mode == FCAP_UNSET) {
        const char *b = getenv("GMLOADER_FCAP_BURST");
        if (b && *b) { long v = atol(b); if (v >= 1 && v <= 16) g_fcap_burst = (int32_t)v; }
        g_fcap_mode = FCAP_SCANOUT;
        g_fcap_last_scan = cnt;
        g_fcap_credit = 0;
        g_fcap_have_last = true;
    }
    warning("frame-cap: scanout period %u cyc = %.4f ms (%.4f Hz); burst=%d; mode=%s\n",
            (unsigned)cyc, 1000.0 / g_fcap_refresh, (double)g_fcap_refresh, (int)g_fcap_burst,
            g_fcap_mode == FCAP_SCANOUT ? "scanout" :
            g_fcap_mode == FCAP_TICKS   ? "wall-clock" : "off");
}

// Refresh rate handed to RunnerJNILib::Process. GM's delta/pacing math is
// display-gated by design; feeding it the measured 59.9228 Hz instead of a
// hardcoded 60 stops its internal clock drifting ~0.13 % against the display.
static float fcap_refresh_hz(void) {
    fcap_resolve();
    return g_fcap_refresh != 0.0f ? g_fcap_refresh : 60.0f;
}

// The pre-existing wall-clock cap, byte-for-byte, kept as the fallback.
static void fcap_wait_ticks(void) {
    static Uint32 next_ms = 0;
    if (!g_fcap_frame_ms) return;
    Uint32 now = SDL_GetTicks();
    if (next_ms == 0 || now > next_ms + g_fcap_frame_ms) next_ms = now;
    next_ms += g_fcap_frame_ms;
    if (now < next_ms) SDL_Delay(next_ms - now);
}

// Fold the boundaries observed since the last read into the credit bucket.
// The difference is taken in uint32 so the counter's 2^32 wrap is transparent;
// a NEGATIVE delta can only mean the counter restarted (core reload) or the
// read was garbage, so it re-bases instead of banking a huge or negative debt.
static void fcap_credit_add(uint32_t cnt) {
    int32_t d = (int32_t)(cnt - g_fcap_last_scan);
    g_fcap_last_scan = cnt;
    if (d < 0) return;
    g_fcap_credit += d;
    if (g_fcap_credit > g_fcap_burst) g_fcap_credit = g_fcap_burst;
}

static void fcap_demote(const char *why) {
    g_fcap_mode = g_fcap_frame_ms ? FCAP_TICKS : FCAP_OFF;
    // Drop the measured refresh too. Demotion means the instrument is not
    // trustworthy, and `scan_period_cyc` can be wrong while still landing inside
    // the 5–50 ms sane band — a mismatched or older RBF where 0x3BFB0018 is not
    // this counter would otherwise keep feeding GM that bogus rate forever, long
    // after the cap itself stopped believing the same register.
    g_fcap_refresh = 60.0f;
    warning("frame-cap: scanout counter %s — falling back to the %u ms wall-clock "
            "cap, GM refresh 60.00 Hz\n", why, (unsigned)g_fcap_frame_ms);
}

static void fcap_wait(void) {
    fcap_resolve();
    if (g_fcap_mode == FCAP_OFF) return;
    if (g_fcap_mode == FCAP_SCANOUT) {
        uint32_t cnt = 0;
        if (!fcap_scanout_read(&cnt, NULL)) {
            fcap_demote("became unreadable");
        } else if (!g_fcap_have_last) {
            g_fcap_last_scan = cnt; g_fcap_have_last = true;
        } else {
            fcap_credit_add(cnt);
            g_fcap_credit -= 1;             // this game frame consumes one boundary
            if (g_fcap_credit >= 0) {
                g_fcap_stall_run = 0;       // already late/in credit — ZERO wait
            } else {
                const Uint32 t0 = SDL_GetTicks();
                const Uint32 deadline = t0 + (Uint32)FCAP_STALL_MS;
                const long   poll_us = fcap_poll_us();
                bool stalled = false;
                while (g_fcap_credit < 0) {
                    if (poll_us) usleep((useconds_t)poll_us);
                    if (!fcap_scanout_read(&cnt, NULL)) { fcap_demote("became unreadable"); break; }
                    fcap_credit_add(cnt);
                    // Wrap-safe: SDL_GetTicks() wraps at ~49.7 days, and a plain
                    // `> deadline` on a wrapped clock never becomes true, so a
                    // frozen counter after that point would spin here forever.
                    // The signed difference stays correct across the wrap.
                    if (g_fcap_credit < 0 && (Sint32)(SDL_GetTicks() - deadline) > 0) {
                        stalled = true; break;
                    }
                }
                if (g_fcap_stat) { g_fcap_waits++; g_fcap_wait_ms += (double)(SDL_GetTicks() - t0); }
                if (stalled) {
                    // Bounded, never a hang: this frame proceeds regardless. Only a
                    // RUN of stalls (a wedged / non-scanning core) demotes for good.
                    g_fcap_credit = 0;
                    if (++g_fcap_stall_run >= FCAP_STALL_MAX)
                        fcap_demote("stopped advancing");
                } else {
                    g_fcap_stall_run = 0;
                }
            }
        }
        if (g_fcap_mode == FCAP_SCANOUT) {
            if (g_fcap_stat && ++g_fcap_frames % 300 == 0)
                warning("FCAP n=%u waited=%u (%.1f%%) wait_ms_avg=%.2f mode=scanout\n",
                        (unsigned)g_fcap_frames, (unsigned)g_fcap_waits,
                        100.0 * g_fcap_waits / g_fcap_frames,
                        g_fcap_waits ? g_fcap_wait_ms / g_fcap_waits : 0.0);
            return;
        }
        // demoted this frame — fall through to the wall clock below
    }
    fcap_wait_ticks();
}
#endif


int relaunch_flag = 0;
char *program_name = nullptr;
const char* gc_workdir = nullptr;
bool override_apk = false;

/*
      Don't touch this incantation. It serves no practical
    reason that you can grep this source code for, nothing
    mentions this on the code base, but disturbing what lies
    here will give you headaches, nausea and sleep loss.
      If you *properly* understand why this fixes a stack smash
    on shader loading code, then please, go ahead and explain
    it to me proper, thanks.
*/
thread_local int tls0[2<<12] = {};
int foo() { return tls0[0]++; }

#define CONFIG_FILE     "config.json"

extern DynLibFunction symtable_libc[];
extern DynLibFunction symtable_zlib[];
extern DynLibFunction symtable_gles2[];

extern double FORCE_PLATFORM;

DynLibFunction *so_static_patches[32] = {
    NULL,
};

DynLibFunction *so_dynamic_libraries[32] = {
    symtable_libc,
    symtable_zlib,
    symtable_gles2,
    NULL
};

so_module *libyoyo = NULL;

int RunnerJNILib_MoveTaskToBackCalled = 0;

/* The GameMaker runner assembles "<save_dir><filename>" into a fixed ~34-byte
 * heap buffer. It is compiled with _FORTIFY_SOURCE, so it passes that size to
 * __strcpy_chk — but gmloader's fortify layer stubs the bounds check out
 * (thunks/libc/fortify.cpp: __check_buffer_access / __fortify_fatal are no-ops),
 * so the copy runs unbounded and silently smashes the next heap chunk. That is
 * the corruption behind the intermittent glibc "malloc(): invalid size" abort.
 *
 * Measured on device with ASan: a 3-char save path is clean and reaches the
 * render loop; 14, 19, 32 and 60 chars all overflow. A real save directory
 * cannot be that short ("/media/fat/" is 11 before you name anything), so hand
 * the runner a short SYMLINK and leave the actual data where it belongs.
 *
 * The link lives on the RAM-overlay rootfs, so it is (re)created every launch.
 * Any failure is non-fatal: fall back to the real path and warn. */
static const char *kSaveDirAlias  = "/s";
static const size_t kSaveDirMaxLen = 8;   /* conservative: 14 already overflows */

static fs::path alias_save_dir(const fs::path &save_dir)
{
    /* save_dir is built with a trailing separator (`/ ""`); symlink() wants the
     * directory itself. */
    fs::path target = save_dir;
    if (target.filename().empty()) target = target.parent_path();

    struct stat st;
    if (lstat(kSaveDirAlias, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            warning("save_dir: %s exists and is not a symlink; using the long path "
                    "(the runner may overflow its path buffer)\n", kSaveDirAlias);
            return save_dir;
        }
        /* An existing link that already points where we want is the common case
         * on MiSTer, where / is mounted READ-ONLY: unlink() and symlink() both
         * fail there, so recreating it unconditionally made this fall back to
         * the long path — silently reinstating the heap corruption this alias
         * exists to prevent. Reuse it instead. */
        char cur[512];
        ssize_t n = readlink(kSaveDirAlias, cur, sizeof(cur) - 1);
        if (n > 0) {
            cur[n] = '\0';
            if (!strcmp(cur, target.c_str())) {
                warning("save_dir: reusing existing %s -> %s\n", kSaveDirAlias, cur);
                return fs::path(kSaveDirAlias) / "";
            }
        }
    }
    unlink(kSaveDirAlias);   /* stale link pointing elsewhere; ENOENT is fine */
    if (symlink(target.c_str(), kSaveDirAlias) != 0) {
        warning("save_dir: could not link %s -> %s (%s); using the long path\n",
                kSaveDirAlias, target.c_str(), strerror(errno));
        return save_dir;
    }
    warning("save_dir: %s -> %s (short path for the runner's fixed path buffer)\n",
            kSaveDirAlias, target.c_str());
    return fs::path(kSaveDirAlias) / "";   /* keep the trailing-separator convention */
}

static fs::path get_absolute_path(const char* path, fs::path work_dir){

    fs::path fs_path = fs::path(path);
    
    if ( fs_path.is_relative() ){
        fs_path = work_dir / fs_path;
    }

    return fs_path;
}

/* PIE load base, cached at startup. The engine is deliberately NOT linked
 * -rdynamic (exporting every symbol would let the executable interpose over the
 * dlopen'd Mesa/GLES libs), so backtrace_symbols_fd prints bare addresses.
 * Resolve them offline against the unstripped ELF:
 *     arm-linux-gnueabihf-addr2line -f -C -e gmloadernext.armhf <addr - base>  */
static void *g_exe_base = NULL;
static uintptr_t g_text_lo = 0, g_text_hi = 0;   /* our own executable text range */

/* Cache the main executable's text range from /proc/self/maps. Done at startup
 * (where allocating and using stdio is safe), used by the signal handler. */
static void cache_text_range(void)
{
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char perms[8], path[512];
        path[0] = '\0';
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &lo, &hi, perms, path) < 3) continue;
        if (perms[2] != 'x') continue;
        char *p = path;
        while (*p == ' ') p++;
        if (strcmp(p, exe) != 0) continue;
        if (!g_text_lo || lo < g_text_lo) g_text_lo = (uintptr_t)lo;
        if ((uintptr_t)hi > g_text_hi)    g_text_hi = (uintptr_t)hi;
    }
    fclose(f);
}

/* Async-signal-safe: backtrace_symbols_fd does not allocate, and backtrace()
 * itself is pre-warmed in main() so it never has to dlopen/malloc here — which
 * matters because the case we are chasing reaches this path with a corrupt heap. */
static void crash_backtrace(void)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    static const char hdr[] = "  --- backtrace (exe frames: addr2line on exe+0x...) ---\n";
    (void)!write(STDERR_FILENO, hdr, sizeof(hdr) - 1);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

/* glibc has no .ARM.exidx unwind tables, so backtrace() stops dead at the first
 * libc frame. A heap abort is raised from inside malloc/free, so the unwound
 * trace shows ONLY libc — never the engine code that owns the bad chunk, which
 * is the entire thing we need. Scanning the stack for words pointing into our
 * own text recovers those frames. It is heuristic: stale slots yield false
 * positives, so read the list as candidates, newest (lowest sp) first. */
static void crash_stack_scan(uintptr_t sp)
{
    static const char hdr[] = "  --- stack scan: candidate exe return addrs (heuristic) ---\n";
    (void)!write(STDERR_FILENO, hdr, sizeof(hdr) - 1);
    if (!g_text_lo || !sp) return;

    const uintptr_t *p = (const uintptr_t *)(sp & ~(uintptr_t)3);
    char buf[128];
    int printed = 0;
    /* 2048 words = 8 KB upward (toward older frames); the main stack is far
     * larger, so this stays inside the mapping. */
    for (int i = 0; i < 2048 && printed < 40; i++) {
        uintptr_t v = p[i];
        if (v < g_text_lo || v >= g_text_hi) continue;
        int len = snprintf(buf, sizeof(buf), "    [sp+%04x] exe+0x%lx\n",
                           (unsigned)(i * 4),
                           (unsigned long)(v - (uintptr_t)g_exe_base));
        if (len > 0) (void)!write(STDERR_FILENO, buf, (size_t)len);
        printed++;
    }
}

/* Deliberate heap overflow, then free — reproduces the exact topology of the
 * bug we are hunting (glibc aborts from inside free(), called by engine code)
 * so the handler can be proven to surface the engine frame. */
static __attribute__((noinline)) void selftest_heap_corrupt(void)
{
    unsigned char *p = (unsigned char *)malloc(64);
    volatile unsigned char *q = p;
    for (int i = 0; i < 96; i++) q[i] = 0xAB;   /* overflow past the chunk */
    free(p);
}

static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    /* One-shot: if dumping state itself faults, die rather than loop. */
    static volatile sig_atomic_t in_handler = 0;
    if (in_handler) { signal(sig, SIG_DFL); raise(sig); return; }
    in_handler = 1;

    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t fault_addr = (uintptr_t)info->si_addr;
    const char *name = sig == SIGSEGV ? "SIGSEGV" :
                       sig == SIGILL  ? "SIGILL"  :
                       sig == SIGBUS  ? "SIGBUS"  :
                       sig == SIGABRT ? "SIGABRT" :
                       sig == SIGFPE  ? "SIGFPE"  : "SIGNAL";
#if defined(__arm__)
    uintptr_t pc = uc->uc_mcontext.arm_pc;
    uintptr_t lr = uc->uc_mcontext.arm_lr;
    fprintf(stderr, "%s: fault_addr=%p PC=%p LR=%p\n", name,
            (void*)fault_addr, (void*)pc, (void*)lr);
    fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
            uc->uc_mcontext.arm_r0, uc->uc_mcontext.arm_r1,
            uc->uc_mcontext.arm_r2, uc->uc_mcontext.arm_r3);
    fprintf(stderr, "  r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
            uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
            uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7);
    fprintf(stderr, "  sp=%08lx ip=%08lx fp=%08lx\n",
            uc->uc_mcontext.arm_sp, uc->uc_mcontext.arm_ip,
            uc->uc_mcontext.arm_fp);
    // Dump the instruction at PC so we can see what faulted
    fprintf(stderr, "  insn@PC: %08x\n", *(uint32_t*)(pc & ~3));
#else
    fprintf(stderr, "%s: fault_addr=%p\n", name, (void*)fault_addr);
#endif
    fprintf(stderr, "  exe_load_base=%p text=[%08lx,%08lx)\n", g_exe_base,
            (unsigned long)g_text_lo, (unsigned long)g_text_hi);
    fflush(stderr);
    crash_backtrace();
#if defined(__arm__)
    crash_stack_scan(uc->uc_mcontext.arm_sp);
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char *argv[])
{
    // Pre-warm the unwinder and cache the PIE load base BEFORE anything can
    // crash. backtrace() lazily dlopen()s libgcc's unwinder and allocates on
    // first use — exactly what we cannot afford inside a heap-corruption abort,
    // which is the case this handler exists to capture.
    {
        void *warm[4];
        (void)backtrace(warm, 4);
        Dl_info di;
        if (dladdr((void *)&crash_handler, &di)) g_exe_base = di.dli_fbase;
        cache_text_range();
    }

    struct sigaction sa = {};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    // SIGABRT is what glibc raises from malloc_printerr ("malloc(): invalid
    // size", "corrupted double-linked list"). Without it a heap abort took the
    // default action and died with no diagnostic at all.
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    // Verification hook: forces the exact failure mode we are trying to capture,
    // so the handler can be proven end-to-end instead of first being exercised
    // by a real, intermittent crash.
    if (getenv("GMLOADER_SELFTEST_ABORT")) abort();
    if (getenv("GMLOADER_SELFTEST_HEAP")) selftest_heap_corrupt();

    // Store the program name from argv[0]
    if (argc > 0 && argv[0]) {
        program_name = argv[0];
    } else 
    {
        fatal_error("Main: Could not determine program name from argv[0]\n");
        return -1;
    }

    gmloader_config.init_defaults();

    fs::path work_dir, config_file_path, save_dir, apk_path;
    work_dir = fs::canonical(fs::current_path()) / "";

    // Check for -a (apk_path override) and -c (config file)
    std::string override_apk_path;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            override_apk_path = argv[i + 1];
            warning("Main: Using apk_path override from args: '%s'\n", override_apk_path.c_str());
            i++; // Skip the value
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file_path = work_dir / argv[i + 1];
            if (gmloader_config.parse_file(config_file_path.c_str()) < 0) {
                warning("Error while loading the config file\n");
            }
        }
    }

    // Apply apk_path override if provided
    if (!override_apk_path.empty()) {
        gmloader_config.apk_path = override_apk_path;
        override_apk = true;
    }

    char platform_ov[32];
    strncpy(platform_ov, gmloader_config.force_platform.c_str(), sizeof(platform_ov) - 1);
    platform_ov[sizeof(platform_ov) - 1] = '\0';

    std::unordered_map<std::string, int> platform_map = {
        {"os_unknown", os_unknown},
        {"os_windows", os_windows},
        {"os_macosx", os_macosx},
        {"os_ios", os_ios},
        {"os_android", os_android},
        {"os_linux", os_linux},
        {"os_psvita", os_psvita},
        {"os_ps4", os_ps4},
        {"os_xboxone", os_xboxone},
        {"os_tvos", os_tvos},
        {"os_switch", os_switch}
    };

    std::string platform_str(platform_ov);
    std::transform(platform_str.begin(), platform_str.end(), platform_str.begin(), ::tolower);
    auto it = platform_map.find(platform_str);
    if (it != platform_map.end()) {
        FORCE_PLATFORM = it->second;
    } else {
        warning("Unexpected platform '%s'.\n", platform_ov);
        strcpy(platform_ov, "os_unknown");
        FORCE_PLATFORM = os_unknown;
    }

    save_dir = get_absolute_path(gmloader_config.save_dir.c_str(), work_dir) / "";
    /* Keep the path the runner sees inside its fixed buffer (see alias_save_dir).
     * Everything downstream uses the alias, which resolves to the same directory,
     * so saves still land in the configured location. */
    if (strlen(save_dir.c_str()) > kSaveDirMaxLen)
        save_dir = alias_save_dir(save_dir);
    apk_path = get_absolute_path(gmloader_config.apk_path.c_str(), work_dir);

    int err;
    zip_t *apk = zip_open(apk_path.c_str(), ZIP_RDONLY, &err);
    if (apk == NULL) {
        zip_error_t zerror;
        zip_error_init_with_code(&zerror, err);
        fatal_error("Failure opening APK '%s': %s\n", apk_path.c_str(), zip_error_strerror(&zerror));
        zip_error_fini(&zerror);
        return -1;
    }

    // Create the Fake JavaVM environment
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK) {
        fatal_error("Error initializing JNI Interface!\n");
        return -1;
    }

    const char *dumpdir = getenv("GMLOADER_DUMP_DIR");
    const char *alt_searchpath = getenv("GMLOADER_LIB_PATH");
    so_set_options(dumpdir, alt_searchpath);

    warning("Loading images...\n");
    libyoyo = so_load_module("libyoyo.so", apk, (void*)vm);
    if (libyoyo == NULL) {
        fatal_error("Could not load libyoyo.so!\n");
        return -1;
    }
    warning("DBG: so_load_module done\n");

    patch_libyoyo(libyoyo);
    warning("DBG: patch_libyoyo done\n");
    if(gmloader_config.disable_depth == 1) {
        disable_depth();
    }
    patch_input(libyoyo);
    patch_gamepad(libyoyo);
    patch_mouse(libyoyo);
    patch_fmod(libyoyo);
    patch_display_mouse_lock(libyoyo);
    patch_gameframe(libyoyo);
    patch_psn(libyoyo);
    patch_steam(libyoyo);
    patch_texture(libyoyo);
    patch_lua(libyoyo);
#ifdef MISTER_NATIVE_VIDEO
    patch_bench_godmode(libyoyo);
#endif
    warning("DBG: all patches done\n");

    warning("DBG: before NewStringUTF apk_path\n");
    String *apk_path_arg = (String *)env->NewStringUTF(apk_path.c_str());
    warning("DBG: before NewStringUTF save_dir\n");
    String *save_dir_arg = (String *)env->NewStringUTF(save_dir.c_str());
    warning("DBG: before NewStringUTF pkg_dir\n");
    String *pkg_dir_arg = (String *)env->NewStringUTF("com.johnny.loader");
    warning("DBG: after NewStringUTF calls\n");
    printf("apk_path %s save_dir %s pkg_dir %s\n", apk_path_arg->str, save_dir_arg->str, pkg_dir_arg->str);

#ifdef MISTER_NATIVE_VIDEO
    // Build path to bundled libGLES_sw.so relative to the binary (same dir as argv[0])
    {
        warning("DBG: entering MISTER_NATIVE_VIDEO block\n");
        char argv0_buf[4096];
        strncpy(argv0_buf, argv[0], sizeof(argv0_buf) - 1);
        argv0_buf[sizeof(argv0_buf) - 1] = '\0';
        char *bin_dir = dirname(argv0_buf);
        char lib_path[4096];
        char mesa_dir[4096];
        snprintf(mesa_dir, sizeof(mesa_dir), "%s/mesa", bin_dir);

        // Standalone Mesa 21.3 (no GLVND, no X11/Wayland/GBM, softpipe/no-LLVM).
        // EGL_PLATFORM=surfaceless: use the Mesa surfaceless headless backend.
        // LIBGL_DRIVERS_PATH: DRI loader finds swrast_dri.so here.
        setenv("EGL_PLATFORM", "surfaceless", 0);
        setenv("LIBGL_DRIVERS_PATH", mesa_dir, 0);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
        warning("DBG: set EGL_PLATFORM=surfaceless, LIBGL_DRIVERS_PATH=%s\n", mesa_dir);

        // Pre-load Mesa runtime libs from mesa/ subdir so the dynamic linker finds
        // them before attempting system paths (MiSTer has no system Mesa).
        static const char* const preload_libs[] = {
            "libdrm.so.2", "libglapi.so.0", "libEGL.so.1", NULL
        };
        for (int i = 0; preload_libs[i]; i++) {
            // Try mesa/ subdir first
            snprintf(lib_path, sizeof(lib_path), "%s/%s", mesa_dir, preload_libs[i]);
            void *h = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
            if (h) {
                warning("DBG: preloaded mesa/%s ok\n", preload_libs[i]);
            } else {
                warning("DBG: mesa/%s fail: %s\n", preload_libs[i], dlerror());
                // Fall back to bin_dir
                snprintf(lib_path, sizeof(lib_path), "%s/%s", bin_dir, preload_libs[i]);
                h = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
                warning("DBG: bin_dir/%s %s\n", preload_libs[i], h ? "ok" : "fail");
            }
        }
        snprintf(lib_path, sizeof(lib_path), "%s/libGLES_sw.so", bin_dir);
        warning("DBG: dlopen libGLES_sw.so at %s\n", lib_path);
        g_gles_handle = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
        if (!g_gles_handle) {
            fatal_error("Cannot load libGLES_sw.so: %s\n", dlerror());
            return -1;
        }
        warning("Loaded bundled GLES library: %s\n", lib_path);
    }
#endif

    // In MISTER_NATIVE_VIDEO mode the real display is the DDR framebuffer; use SDL's
    // dummy video driver so SDL_Init doesn't fail looking for a display device.
    // Audio and joystick still go through real drivers.
#ifdef MISTER_NATIVE_VIDEO
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    setenv("SDL_AUDIODRIVER", "dummy", 0);
    warning("DBG: SDL_VIDEODRIVER=dummy for MISTER_NATIVE_VIDEO\n");
#endif

#ifdef MISTER_NATIVE_VIDEO
    // The FPGA owns the audio path; MiSTer's Linux has no sound card. Pin SDL
    // to the dummy driver so SDL_Init can neither fail for want of a device
    // nor grab one, then bring the DDR ring up.
    setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif

    // Initialize SDL with video, audio, joystick, and controller support
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        fatal_error("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

#ifdef MISTER_NATIVE_VIDEO
    fprintf(stderr, "SDL audio driver: %s\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
    if (!MisterAudio_Init())
        warning("MisterAudio: /dev/mem unavailable, using SDL fallback\n");
#endif

    // Load an SDL controller-mapping DB so pads not in SDL's built-in list are
    // recognized as GameControllers (update_inputs only handles the GameController
    // API — an unmapped pad would never raise CONTROLLERDEVICEADDED). Try the
    // configured path first, then conventional locations next to the binary and in
    // save_dir. SDL also auto-applies the SDL_GAMECONTROLLERCONFIG env var on init.
    {
        std::string candidates[] = {
            gmloader_config.controller_db,
            "gamecontrollerdb.txt",
            (fs::path(gmloader_config.save_dir) / "gamecontrollerdb.txt").string(),
        };
        for (const std::string &p : candidates) {
            if (p.empty() || !fs::exists(p)) continue;
            int n = SDL_GameControllerAddMappingsFromFile(p.c_str());
            if (n < 0) {
                warning("Controller DB '%s' failed to load: %s\n", p.c_str(), SDL_GetError());
            } else {
                warning("Loaded %d controller mapping(s) from %s\n", n, p.c_str());
            }
            break;   // first existing file wins
        }
    }

    if(gmloader_config.show_cursor == 0) {
        if (SDL_ShowCursor(SDL_DISABLE) < 0) {
            warning("Cannot disable cursor: %s\n", SDL_GetError());
        } else {
            printf("Cursor disabled\n");
        }
    }

    SDL_Window *sdl_win;
#ifndef MISTER_NATIVE_VIDEO
    SDL_GLContext sdl_ctx;
#endif

    SDL_DisplayMode mode = {};
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0) {
        warning("Unable to query display mode, using defaults.");
        mode.w = 640;
        mode.h = 480;
    }

#ifdef MISTER_NATIVE_VIDEO
    // Under MISTER_NATIVE_VIDEO the SDL window is for input/audio only — no OpenGL flag
    sdl_win = SDL_CreateWindow("GMLoader", 0, 0, mode.w, mode.h,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
#else
    sdl_win = SDL_CreateWindow("GMLoader", 0, 0, mode.w, mode.h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
#endif
    if (sdl_win == NULL) {
        fatal_error("Failed to create SDL Window: %s\n", SDL_GetError());
        return -1;
    }

#ifndef MISTER_NATIVE_VIDEO
    // Basic OpenGL ES 2.x setup — skip entirely under MISTER_NATIVE_VIDEO to
    // avoid SDL warnings about setting GL attributes on a non-GL window
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    sdl_ctx = SDL_GL_CreateContext(sdl_win);
    if (sdl_ctx == NULL) {
        fatal_error("Failed to create OpenGL Context: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
#else
    // EGL headless pbuffer context via bundled libGLES_sw.so
    {
        // Resolve core EGL entry points. libGLES_sw.so is a GLES2 dispatch wrapper
        // and does not export EGL symbols; those live in the preloaded libEGL.so.1
        // (loaded with RTLD_GLOBAL above).  Use RTLD_DEFAULT to search the global
        // symbol namespace rather than a specific handle.
        auto pfnGetDisplay   = (EGLDisplay(*)(EGLNativeDisplayType))dlsym(RTLD_DEFAULT, "eglGetDisplay");
        auto pfnInitialize   = (EGLBoolean(*)(EGLDisplay, EGLint*, EGLint*))dlsym(RTLD_DEFAULT, "eglInitialize");
        auto pfnChooseConfig = (EGLBoolean(*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))dlsym(RTLD_DEFAULT, "eglChooseConfig");
        auto pfnCreateCtx    = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*))dlsym(RTLD_DEFAULT, "eglCreateContext");
        auto pfnCreatePbuf   = (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint*))dlsym(RTLD_DEFAULT, "eglCreatePbufferSurface");
        auto pfnMakeCurrent  = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(RTLD_DEFAULT, "eglMakeCurrent");
        auto pfnBindAPI      = (EGLBoolean(*)(EGLenum))dlsym(RTLD_DEFAULT, "eglBindAPI");
        warning("DBG: EGL syms: GetDisplay=%p Init=%p Choose=%p\n",
                (void*)pfnGetDisplay, (void*)pfnInitialize, (void*)pfnChooseConfig);

        if (!pfnGetDisplay || !pfnInitialize || !pfnChooseConfig ||
            !pfnCreateCtx  || !pfnCreatePbuf  || !pfnMakeCurrent) {
            fatal_error("Failed to resolve core EGL symbols from libGLES_sw.so\n");
            return -1;
        }

        // This Mesa has EGL_EXT_platform_device but not surfaceless.
        // Extension function pointers must be fetched via eglGetProcAddress (not dlsym).
        // Use eglQueryDevicesEXT + eglGetPlatformDisplayEXT(DEVICE_EXT) for headless.
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
        auto pfnGetProcAddr = (void*(*)(const char*))dlsym(RTLD_DEFAULT, "eglGetProcAddress");
        auto pfnQueryDevicesEXT       = pfnGetProcAddr ?
            (EGLBoolean(*)(EGLint, void**, EGLint*))pfnGetProcAddr("eglQueryDevicesEXT") : nullptr;
        auto pfnGetPlatformDisplayEXT = pfnGetProcAddr ?
            (EGLDisplay(*)(EGLenum, void*, const EGLint*))pfnGetProcAddr("eglGetPlatformDisplayEXT") : nullptr;
        warning("DBG: eglGetProcAddress=%p QueryDevices=%p GetPlatformDisplay=%p\n",
                (void*)pfnGetProcAddr, (void*)pfnQueryDevicesEXT, (void*)pfnGetPlatformDisplayEXT);

        EGLDisplay egl_dpy = EGL_NO_DISPLAY;
        if (pfnQueryDevicesEXT && pfnGetPlatformDisplayEXT) {
            void* egl_devices[4] = {};
            EGLint num_devices = 0;
            if (pfnQueryDevicesEXT(4, egl_devices, &num_devices) && num_devices > 0) {
                warning("DBG: EGL_PLATFORM_DEVICE: %d device(s)\n", num_devices);
                egl_dpy = pfnGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, egl_devices[0], NULL);
                warning("DBG: eglGetPlatformDisplayEXT(DEVICE_0) = %p\n", (void*)egl_dpy);
            } else {
                warning("DBG: eglQueryDevicesEXT returned %d devices\n", num_devices);
            }
        }
        auto pfnGetError = (EGLint(*)(void))dlsym(RTLD_DEFAULT, "eglGetError");
        #ifndef EGL_PLATFORM_SURFACELESS_MESA
        #define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
        #endif
        // Preferred headless path for a software Mesa with no /dev/dri:
        // the surfaceless platform via eglGetPlatformDisplay.
        if (egl_dpy == EGL_NO_DISPLAY && pfnGetPlatformDisplayEXT) {
            egl_dpy = pfnGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
            warning("DBG: eglGetPlatformDisplay(SURFACELESS_MESA) = %p\n", (void*)egl_dpy);
        }
        if (egl_dpy == EGL_NO_DISPLAY) {
            warning("DBG: falling back to eglGetDisplay(DEFAULT)\n");
            egl_dpy = pfnGetDisplay(EGL_DEFAULT_DISPLAY);
            warning("DBG: eglGetDisplay(DEFAULT) = %p\n", (void*)egl_dpy);
        }

        EGLint egl_major = 0, egl_minor = 0;
        // NOTE: fatal_error() only prints — it does NOT exit. Without an explicit
        // return, a failed eglInitialize cascades through every following EGL call
        // and ends in a null GL context -> SIGSEGV deep inside Mesa. Bail out here.
        if (!pfnInitialize(egl_dpy, &egl_major, &egl_minor)) {
            EGLint e = pfnGetError ? pfnGetError() : 0;
            fatal_error("eglInitialize failed (dpy=%p eglGetError=0x%04x)\n", (void*)egl_dpy, e);
            return -1;
        }
        if (pfnBindAPI) pfnBindAPI(EGL_OPENGL_ES_API);

        static const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,   8,  EGL_GREEN_SIZE,  8,
            EGL_BLUE_SIZE,  8,  EGL_ALPHA_SIZE,  8,
            EGL_DEPTH_SIZE, 16, EGL_NONE
        };
        EGLConfig egl_cfg; EGLint ncfg = 0;
        pfnChooseConfig(egl_dpy, cfg_attribs, &egl_cfg, 1, &ncfg);
        if (ncfg == 0) { fatal_error("eglChooseConfig: no matching config\n"); return -1; }

        static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext egl_ctx = pfnCreateCtx(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (egl_ctx == EGL_NO_CONTEXT) { fatal_error("eglCreateContext failed\n"); return -1; }

        // Render size: 320x240 normally, or GMLOADER_RENDER_W/H when the blitter
        // owns rendering (so the game's native size can be presented 1:1 +
        // letterboxed into the fixed 320x240 DDR buffer — no upscale artifacts).
        int pbw = MISTER_WIDTH, pbh = MISTER_HEIGHT;
        {
            const char *bl = getenv("GMLOADER_BLITTER"); int lvl = bl ? atoi(bl) : 0;
            if (lvl >= 2) {
                const char *rw = getenv("GMLOADER_RENDER_W"), *rh = getenv("GMLOADER_RENDER_H");
                if (rw) pbw = atoi(rw);
                if (rh) pbh = atoi(rh);
                if (pbw <= 0 || pbw > MISTER_WIDTH)  pbw = MISTER_WIDTH;
                if (pbh <= 0 || pbh > MISTER_HEIGHT) pbh = MISTER_HEIGHT;
            }
        }
        EGLint pbuf_attribs[] = { EGL_WIDTH, pbw, EGL_HEIGHT, pbh, EGL_NONE };
        EGLSurface egl_surf = pfnCreatePbuf(egl_dpy, egl_cfg, pbuf_attribs);
        if (egl_surf == EGL_NO_SURFACE) { fatal_error("eglCreatePbufferSurface failed\n"); return -1; }

        if (!pfnMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) {
            fatal_error("eglMakeCurrent failed\n");
            return -1;
        }

        warning("EGL headless context created: %d.%d, pbuffer %dx%d\n",
                egl_major, egl_minor, pbw, pbh);
    }

    if (!NativeVideoWriter_Init()) {
        warning("NativeVideoWriter: /dev/mem unavailable, using SDL fallback\n");
    }
    SDL_Renderer* sdl_renderer = nullptr;
    SDL_Texture*  sdl_texture  = nullptr;
    if (!NativeVideoWriter_IsActive()) {
        sdl_renderer = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE);
        if (!sdl_renderer)
            fatal_error("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        sdl_texture = SDL_CreateTexture(sdl_renderer,
                                        SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        MISTER_WIDTH, MISTER_HEIGHT);
        if (!sdl_texture)
            fatal_error("SDL_CreateTexture failed: %s\n", SDL_GetError());
    }
#endif

    // The libraries are loaded by SDL2.0, and the API entry points by the following
    // functions using the GLAD generated headers.
    load_egl_funcs();
    load_gles2_funcs();
    const char *dump_shaders = getenv("GMLOADER_DUMP_SHADERS");
    set_gles2_shader_override_dir(gmloader_config.shader_dir.c_str(), dump_shaders && *dump_shaders != '\0');

    int cont = 1;
    int w, h;

    #ifdef VIDEO_SUPPORT
    if (video_init(sdl_win, save_dir.c_str()) != 0)
    {
        fatal_error("Could not initialize Video Playback.\n");
        return -1;
    }
    #endif

    int sw, sh;
    SDL_GetWindowSize(sdl_win, &sw, &sh);
    splash_render(apk, sw, sh, sdl_win);

    RunnerJNILib::Startup(env, 0, apk_path_arg, save_dir_arg, pkg_dir_arg, 4, 0);
    setup_ended = 1;

#ifdef MISTER_NATIVE_VIDEO
    {
        int init_w, init_h;
        SDL_GetWindowSize(sdl_win, &init_w, &init_h);
        FrameCapture_Init(init_w, init_h);
    }
    DrawTrace_Init();
    Blitter_Init();
#endif

    while (cont != 0 && cont != 2 && RunnerJNILib_MoveTaskToBackCalled == 0 && relaunch_flag == 0) {
        #ifdef VIDEO_SUPPORT
        video_process();
        #endif
        if (update_inputs(sdl_win) != 1)
            break;
        SDL_GetWindowSize(sdl_win, &w, &h);
#ifdef MISTER_NATIVE_VIDEO
        uint64_t _dt_p0 = DrawTrace_NowNs();
        cont = RunnerJNILib::Process(env, 0, Blitter_RenderW(), Blitter_RenderH(), 0, 0, 0, 0, 0,
                                     fcap_refresh_hz());
        uint64_t _dt_p1 = DrawTrace_NowNs();
        if (RunnerJNILib::canFlip(env, 0)) {
          const uint8_t* _blit = Blitter_PresentDefault();
          if (_blit && NativeVideoWriter_IsActive()) {
            if (RasterBackend_Select() == &backend_mfgpu) {
              // Fabric-offload (FO Task 4): backend_mfgpu's present() already
              // submitted this frame's ring to the FPGA fabric (publish + doorbell +
              // poll C_DONE), and the Maldita core composites it into on-chip BRAM
              // and scans itself out. So there is NOTHING to hand to
              // NativeVideoWriter here — the 0x3A DDR writer is the *software*
              // producer, unused on the fabric path (no blt_execute, no g_fb565).
              (void)0;
            } else {
              // Blitter owns the frame: convert (with row flip) straight to DDR,
              // skipping glReadPixels entirely.
              static uint16_t rgb565_blit[MISTER_WIDTH * MISTER_HEIGHT];
              Blitter_ToRGB565(_blit, rgb565_blit);
              NativeVideoWriter_WriteFrame(rgb565_blit, MISTER_WIDTH, MISTER_HEIGHT, MISTER_WIDTH * 2);
            }
          } else {
            FrameCapture_ReadFrame();

            // glReadPixels returns bottom-row-first; flip to top-row-first for FPGA/SDL
            {
                static uint8_t flip_row[MISTER_WIDTH * 4];
                uint8_t* buf = const_cast<uint8_t*>(FrameCapture_GetRGBA());
                for (int top = 0, bot = MISTER_HEIGHT - 1; top < bot; top++, bot--) {
                    uint8_t* row_top = buf + top * MISTER_WIDTH * 4;
                    uint8_t* row_bot = buf + bot * MISTER_WIDTH * 4;
                    memcpy(flip_row, row_top, MISTER_WIDTH * 4);
                    memcpy(row_top, row_bot, MISTER_WIDTH * 4);
                    memcpy(row_bot, flip_row, MISTER_WIDTH * 4);
                }
            }

            if (NativeVideoWriter_IsActive()) {
                static uint16_t rgb565_buf[MISTER_WIDTH * MISTER_HEIGHT];
                FrameCapture_ConvertToRGB565(rgb565_buf);
                NativeVideoWriter_WriteFrame(rgb565_buf, MISTER_WIDTH, MISTER_HEIGHT,
                                            MISTER_WIDTH * 2);
            } else {
                SDL_UpdateTexture(sdl_texture, NULL,
                                  FrameCapture_GetRGBA(), MISTER_WIDTH * 4);
                SDL_RenderClear(sdl_renderer);
                SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
                SDL_RenderPresent(sdl_renderer);
            }
          }
        }
        DrawTrace_FrameEnd(_dt_p1 - _dt_p0, DrawTrace_NowNs() - _dt_p1);
        Blitter_ProfFrameEnd(_dt_p1 - _dt_p0);
        // Frame-rate cap paced by the fabric's scanout counter, not the wall
        // clock. Design, fallbacks and GMLOADER_FPS semantics: see the
        // [Phase 3 Stage B] block near the top of this file.
        fcap_wait();
#else
        cont = RunnerJNILib::Process(env, 0, w, h, 0, 0, 0, 0, 0, 60);
        if (RunnerJNILib::canFlip(env, 0))
            SDL_GL_SwapWindow(sdl_win);
#endif
    }

#ifdef MISTER_NATIVE_VIDEO
    NativeVideoWriter_Shutdown();
    if (sdl_texture)  SDL_DestroyTexture(sdl_texture);
    if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
#endif
#ifndef MISTER_NATIVE_VIDEO
    SDL_GL_DeleteContext(sdl_ctx);
#endif
    SDL_DestroyWindow(sdl_win);
#ifdef MISTER_NATIVE_VIDEO
    MisterAudio_Shutdown();
#endif
    SDL_Quit();

    // Check for relaunch
    if (relaunch_flag && gc_workdir) {
        warning("Main: Relaunch triggered. workdir='%s'\n", gc_workdir);

        // Extract subfolder prefix from original apk_path (e.g., "assets/")
        std::string orig_apk_path = gmloader_config.apk_path;
        std::string prefix;
        size_t last_slash = orig_apk_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            prefix = orig_apk_path.substr(0, last_slash + 1); // Include the slash
        }

        // Remove leading and trailing slashes from gc_workdir (may or may not have any)
        std::string workdir_clean = gc_workdir;
        if (!workdir_clean.empty() && workdir_clean.front() == '/')
            workdir_clean = workdir_clean.substr(1);
        if (!workdir_clean.empty() && workdir_clean.back() == '/')
            workdir_clean = workdir_clean.substr(0, workdir_clean.length() - 1);
        std::string new_apk_path = prefix + workdir_clean;

        // Check if the override is valid
        bool use_override = (new_apk_path.find("..") == std::string::npos && !workdir_clean.empty());
        if (use_override) {
            gmloader_config.apk_path = new_apk_path;
            warning("Main: Updated config: apk_path='%s', save_dir='%s'\n", 
                    gmloader_config.apk_path.c_str(), gmloader_config.save_dir.c_str());
        } else {
            warning("Main: Ignoring override='%s' (workdir='%s'), using original apk_path='%s'\n", 
                    new_apk_path.c_str(), gc_workdir, gmloader_config.apk_path.c_str());
        }

        // Relaunch: use -a only if override is valid, otherwise just -c
        char apk_path_arg[1024];
        char config_path_arg[1024];
        char *argv_relaunch[6];
        int arg_count = 0;
        argv_relaunch[arg_count++] = program_name;
        argv_relaunch[arg_count++] = (char*)"-c";
        snprintf(config_path_arg, sizeof(config_path_arg), "%s", config_file_path.c_str());
        argv_relaunch[arg_count++] = config_path_arg;
        if (use_override) {
            snprintf(apk_path_arg, sizeof(apk_path_arg), "%s", gmloader_config.apk_path.c_str());
            argv_relaunch[arg_count++] = (char*)"-a";
            argv_relaunch[arg_count++] = apk_path_arg;
        }
        argv_relaunch[arg_count] = nullptr;

        warning("Main: Relaunching '%s' with apk_path='%s', save_dir='%s'\n", 
                program_name, gmloader_config.apk_path.c_str(), gmloader_config.save_dir.c_str());
        fflush(stdout);
        fflush(stderr);
        if (execv(program_name, argv_relaunch) == -1) {
            fatal_error("Main: Failed to relaunch '%s': %s\n", program_name, strerror(errno));
            return -1;
        }
    }

    return 0;
}
