/*
 * joy_script — deterministic joystick replay for bench runs.
 *
 * Creates and owns the joy-shm file, then walks a script of timed button
 * masks on the monotonic clock. gmloader's input path (input.cpp:307-319)
 * prefers shm over the FPGA's DDR joystick words whenever JoyShm_Init()
 * succeeds, so simply creating a valid file diverts the engine from the
 * physical joystick to this script.
 *
 * ORDERING IS LOAD-BEARING: g_joyshm_ready latches on the engine's FIRST input
 * poll and is never re-evaluated. Start this driver BEFORE loading the core.
 * If the file is not valid by then, the run silently uses the real joystick
 * for its entire life and the capture is meaningless.
 *
 * The magic word is written LAST, after version/generation/masks, so a reader
 * that maps the file mid-initialisation cannot validate a half-built header —
 * the same doorbell-last discipline blitter_top.sv uses for C_DONE.
 */
#define _POSIX_C_SOURCE 200809L
#include "joy_script_parse.h"
#include "../gmloader/mister/mister_joy_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Absolute-deadline sleep: each step fires at base+ms, so per-step scheduling
 * jitter cannot accumulate across a long script. */
static void sleep_until_ms(const struct timespec *base, uint32_t ms) {
    struct timespec d = *base;
    d.tv_sec  += (time_t)(ms / 1000u);
    d.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (d.tv_nsec >= 1000000000L) { d.tv_nsec -= 1000000000L; d.tv_sec += 1; }
    int rc;
    while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL)) == EINTR)
        if (g_stop) return;
    (void)rc;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: joy_script <shm-path> <script-file>\n"
                        "       shm-path is normally %s\n", MALDITA_JOY_SHM_PATH);
        return 2;
    }
    const char *shm_path    = argv[1];
    const char *script_path = argv[2];

    char *text = read_file(script_path);
    if (!text) {
        fprintf(stderr, "joy_script: cannot read %s: %s\n", script_path, strerror(errno));
        return 1;
    }
    JoyScript js;
    int err_line = 0;
    int rc = JoyScript_ParseText(text, &js, &err_line);
    free(text);
    if (rc != 0) {
        fprintf(stderr, "joy_script: parse error %d at %s:%d\n", rc, script_path, err_line);
        return 1;
    }
    if (js.n == 0) {
        fprintf(stderr, "joy_script: %s contains no steps\n", script_path);
        return 1;
    }

    int fd = open(shm_path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr, "joy_script: open %s: %s\n", shm_path, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)sizeof(MalditaJoyShm)) != 0) {
        fprintf(stderr, "joy_script: ftruncate %s: %s\n", shm_path, strerror(errno));
        close(fd);
        return 1;
    }
    MalditaJoyShm *p = (MalditaJoyShm *)mmap(NULL, sizeof(MalditaJoyShm),
                                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "joy_script: mmap %s: %s\n", shm_path, strerror(errno));
        return 1;
    }

    p->joy_mask[0] = 0u;
    p->joy_mask[1] = 0u;
    p->version     = MALDITA_JOY_SHM_VERSION;
    p->generation += 1u;
    __sync_synchronize();
    p->magic = MALDITA_JOY_SHM_MAGIC;      /* doorbell LAST */
    __sync_synchronize();

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    struct timespec base;
    clock_gettime(CLOCK_MONOTONIC, &base);
    printf("JOYSCRIPT start steps=%zu settle_ms=%u shm=%s script=%s\n",
           js.n, js.settle_ms, shm_path, script_path);
    fflush(stdout);

    for (size_t i = 0; i < js.n && !g_stop; i++) {
        sleep_until_ms(&base, js.steps[i].at_ms);
        if (g_stop) break;
        p->joy_mask[0] = js.steps[i].mask;   /* single naturally-aligned word */
        __sync_synchronize();
        printf("JOYSCRIPT step=%zu t=%ums mask=0x%03X\n",
               i, js.steps[i].at_ms, js.steps[i].mask);
        fflush(stdout);
    }

    if (!g_stop) {
        uint32_t settled_at = js.steps[js.n - 1u].at_ms + js.settle_ms;
        sleep_until_ms(&base, settled_at);
        printf("JOYSCRIPT settled t=%ums mask=0x%03X capture-may-begin\n",
               settled_at, js.steps[js.n - 1u].mask);
        fflush(stdout);
    }

    /* Hold the final mask until killed: the scene must not drift while the
     * capture runs. */
    while (!g_stop) sleep(1);

    printf("JOYSCRIPT stop\n");
    fflush(stdout);
    munmap((void *)p, sizeof(MalditaJoyShm));
    return 0;
}
