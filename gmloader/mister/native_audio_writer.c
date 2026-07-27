//
//  Native Audio DDR3 Writer -- gmloader MiSTer
//
//  48 kHz stereo S16 -> DDR3 ring -> FPGA audio reader -> AUDIO_L/R.
//  ARM advances the wr pointer after each submit; the FPGA writes the rd
//  pointer as it drains. Both are 32-bit byte offsets modulo RING_BYTES.
//  Submit() never blocks -- it drops the tail on overflow.
//
//  Lifted from solarus-mister/patches/mister/native_audio_writer.c so all
//  three MiSTer ports share one audio memory map. The only addition is the
//  GMLOADER_AUDIO_DDR test seam (cf. joy_ddr_reader.cpp).
//
//  Copyright (C) 2026 -- GPL-3.0
//

#include "native_audio_writer.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NA_DDR_PHYS_BASE    0x3A000000u
#define NA_DDR_REGION_SIZE  0x00100000u   /* 1 MB -- shared with video writer */
#define NA_WR_PTR_OFFSET    0x00000030u
#define NA_RD_PTR_OFFSET    0x00000038u
#define NA_RING_OFFSET      0x000D0000u
#define NA_RING_BYTES       0x00010000u   /* 64 KiB, must match RTL */
#define NA_RING_MASK        (NA_RING_BYTES - 1u)

static int                 mem_fd = -1;
static volatile uint8_t   *ddr_base = NULL;
static volatile uint32_t  *wr_ptr_reg = NULL;
static volatile uint32_t  *rd_ptr_reg = NULL;
static volatile uint8_t   *ring_base = NULL;
static uint32_t            local_wr_ptr = 0;

bool NativeAudioWriter_Init(void) {
    if (ddr_base) return true;

    /* GMLOADER_AUDIO_DDR points a host test at a regular file standing in for
     * the /dev/mem region; on device the real physical region is mapped. */
    const char *override_path = getenv("GMLOADER_AUDIO_DDR");
    off_t off;
    if (override_path && *override_path) {
        mem_fd = open(override_path, O_RDWR);
        off = 0;
    } else {
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        off = (off_t)NA_DDR_PHYS_BASE;
    }
    if (mem_fd < 0) {
        perror("NativeAudioWriter: open");
        return false;
    }

    ddr_base = (volatile uint8_t *)mmap(NULL, NA_DDR_REGION_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, off);
    if (ddr_base == MAP_FAILED) {
        perror("NativeAudioWriter: mmap");
        ddr_base = NULL;
        close(mem_fd);
        mem_fd = -1;
        return false;
    }

    wr_ptr_reg = (volatile uint32_t *)(ddr_base + NA_WR_PTR_OFFSET);
    rd_ptr_reg = (volatile uint32_t *)(ddr_base + NA_RD_PTR_OFFSET);
    ring_base  = ddr_base + NA_RING_OFFSET;

    /* Start the FPGA at rd == wr == 0. Zeroing the ring is optional but
     * avoids audible garbage if the FPGA drains before the first submit. */
    memset((void *)ring_base, 0, NA_RING_BYTES);
    *wr_ptr_reg = 0;
    *rd_ptr_reg = 0;
    local_wr_ptr = 0;

    fprintf(stderr,
        "NativeAudioWriter: ring %u bytes @ 0x%08X, wr=0x%08X, rd=0x%08X\n",
        NA_RING_BYTES, NA_DDR_PHYS_BASE + NA_RING_OFFSET,
        NA_DDR_PHYS_BASE + NA_WR_PTR_OFFSET,
        NA_DDR_PHYS_BASE + NA_RD_PTR_OFFSET);
    return true;
}

void NativeAudioWriter_Shutdown(void) {
    if (ddr_base) {
        if (wr_ptr_reg) *wr_ptr_reg = 0;
        munmap((void *)ddr_base, NA_DDR_REGION_SIZE);
        ddr_base = NULL;
    }
    wr_ptr_reg = NULL;
    rd_ptr_reg = NULL;
    ring_base  = NULL;
    local_wr_ptr = 0;
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

bool NativeAudioWriter_IsActive(void) {
    return ddr_base != NULL;
}

size_t NativeAudioWriter_FreeFrames(void) {
    if (!ddr_base) return 0;
    uint32_t rd = *rd_ptr_reg;
    uint32_t wr = local_wr_ptr;
    /* Free bytes = RING - 4 - (wr - rd) mod RING (one frame reserved so
     * wr == rd unambiguously means "empty"). */
    uint32_t used = (wr - rd) & NA_RING_MASK;
    uint32_t free_bytes = (NA_RING_BYTES - 4u) - used;
    return free_bytes / NA_BYTES_PER_FRAME;
}

size_t NativeAudioWriter_CapacityFrames(void) {
    return (NA_RING_BYTES - 4u) / NA_BYTES_PER_FRAME;
}

size_t NativeAudioWriter_Submit(const int16_t *frames, size_t frame_count) {
    if (!ddr_base || !frames || frame_count == 0) return 0;

    size_t max_frames = NativeAudioWriter_FreeFrames();
    if (frame_count > max_frames) frame_count = max_frames;
    if (frame_count == 0) return 0;

    uint32_t write_bytes = (uint32_t)(frame_count * NA_BYTES_PER_FRAME);
    uint32_t offset = local_wr_ptr & NA_RING_MASK;
    uint32_t tail_space = NA_RING_BYTES - offset;

    const uint8_t *src = (const uint8_t *)frames;

    if (write_bytes <= tail_space) {
        memcpy((void *)(ring_base + offset), src, write_bytes);
    } else {
        memcpy((void *)(ring_base + offset), src, tail_space);
        memcpy((void *)ring_base, src + tail_space, write_bytes - tail_space);
    }

    /* Ensure ring writes land before the wr_ptr the FPGA polls advances. */
    __sync_synchronize();

    local_wr_ptr = (local_wr_ptr + write_bytes) & NA_RING_MASK;
    *wr_ptr_reg = local_wr_ptr;

    return frame_count;
}
