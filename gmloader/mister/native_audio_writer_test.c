// Host test for the DDR3 audio ring writer. GMLOADER_AUDIO_DDR points the
// writer at a regular file standing in for the /dev/mem region, the same
// seam joy_ddr_reader.cpp uses. Needs no root and no FPGA.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "native_audio_writer.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)){printf("FAIL %s (line %d)\n",#c,__LINE__);fails++;} } while(0)

#define REGION_SIZE 0x00100000u
#define WR_OFF      0x00000030u
#define RD_OFF      0x00000038u
#define RING_OFF    0x000D0000u
#define RING_BYTES  0x00010000u

static const char *PATH = "/tmp/gmloader-audio-ddr-test";
static volatile uint8_t *g_map = NULL;

// Create a 1 MiB file and map it so the test can act as the FPGA: read the
// write pointer, and advance the read pointer to simulate draining.
static void make_region(void) {
    int fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    if (ftruncate(fd, REGION_SIZE) != 0) { perror("ftruncate"); exit(1); }
    void *p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    g_map = (volatile uint8_t *)p;
    setenv("GMLOADER_AUDIO_DDR", PATH, 1);
}

static uint32_t wr_ptr(void) { return *(volatile uint32_t *)(g_map + WR_OFF); }
static void set_rd_ptr(uint32_t v) { *(volatile uint32_t *)(g_map + RD_OFF) = v; }

int main(void) {
    make_region();
    CHECK(NativeAudioWriter_Init());
    CHECK(NativeAudioWriter_IsActive());

    // Capacity reserves one frame so wr == rd unambiguously means empty.
    const size_t cap = NativeAudioWriter_CapacityFrames();
    CHECK(cap == (RING_BYTES - 4u) / 4u);          // 16383
    CHECK(NativeAudioWriter_FreeFrames() == cap);  // empty at init

    // A small submit lands at offset 0 and advances wr_ptr by frames*4.
    int16_t buf[8];
    for (int i = 0; i < 8; ++i) buf[i] = (int16_t)(100 + i);
    CHECK(NativeAudioWriter_Submit(buf, 4) == 4);
    CHECK(wr_ptr() == 16);
    CHECK(memcmp((const void *)(g_map + RING_OFF), buf, 16) == 0);
    CHECK(NativeAudioWriter_FreeFrames() == cap - 4);

    // The FPGA drains everything: free space returns to capacity.
    set_rd_ptr(16);
    CHECK(NativeAudioWriter_FreeFrames() == cap);

    // Overflow drops the tail instead of overwriting unread samples.
    static int16_t big[20000 * 2];
    memset(big, 0x5A, sizeof(big));
    size_t wrote = NativeAudioWriter_Submit(big, 20000);
    CHECK(wrote == cap);                 // clamped to free space, not 20000
    CHECK(NativeAudioWriter_FreeFrames() == 0);

    // Wrap-around: park the write pointer 8 bytes shy of the ring end, then
    // submit 4 frames (16 bytes) so the copy must split 8/8.
    set_rd_ptr(wr_ptr());
    CHECK(NativeAudioWriter_FreeFrames() == cap);
    const uint32_t start = wr_ptr();                      // 12 after the above
    const uint32_t to_edge = (RING_BYTES - 8u) - start;   // 65516 bytes
    CHECK(to_edge % 4 == 0);
    static int16_t pad[16384 * 2];
    memset(pad, 0, sizeof(pad));
    CHECK(NativeAudioWriter_Submit(pad, to_edge / 4) == to_edge / 4);
    CHECK(wr_ptr() == RING_BYTES - 8u);
    set_rd_ptr(wr_ptr());

    int16_t wrapbuf[8];
    for (int i = 0; i < 8; ++i) wrapbuf[i] = (int16_t)(-1 - i);
    CHECK(NativeAudioWriter_Submit(wrapbuf, 4) == 4);
    CHECK(memcmp((const void *)(g_map + RING_OFF + RING_BYTES - 8),
                 wrapbuf, 8) == 0);
    CHECK(memcmp((const void *)(g_map + RING_OFF),
                 (const uint8_t *)wrapbuf + 8, 8) == 0);
    CHECK(wr_ptr() == 8u);

    // Degenerate inputs are no-ops, not crashes.
    CHECK(NativeAudioWriter_Submit(NULL, 4) == 0);
    CHECK(NativeAudioWriter_Submit(buf, 0) == 0);

    // Shutdown clears the write pointer and deactivates.
    NativeAudioWriter_Shutdown();
    CHECK(!NativeAudioWriter_IsActive());
    CHECK(wr_ptr() == 0);
    CHECK(NativeAudioWriter_Submit(buf, 4) == 0);   // inactive: no write

    munmap((void *)g_map, REGION_SIZE);
    unlink(PATH);
    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", fails);
    return fails ? 1 : 0;
}
