/*
 * ddr_monitor.c — Read NativeVideoWriter control word from DDR3
 *
 * Prints the control word at 0x3A000000 every 100ms.
 * A running gmloader should show an incrementing frame_counter.
 *
 * Build: arm-linux-gnueabihf-gcc -o ddr_monitor ddr_monitor.c
 * Run on MiSTer: ./ddr_monitor
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define DDR_BASE   0x3A000000u
#define DDR_SIZE   0x00001000u  /* 4KB — just the control region */

int main(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    volatile uint32_t *base = mmap(NULL, DDR_SIZE, PROT_READ, MAP_SHARED, fd, DDR_BASE);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    printf("Monitoring DDR control word at 0x%08X (Ctrl-C to stop)\n", DDR_BASE);
    printf("Format: frame_counter | active_buf\n\n");

    uint32_t prev = ~0u;
    int same_count = 0;
    for (;;) {
        uint32_t ctrl = base[0];  /* offset 0 = control word */
        uint32_t frame_cnt = ctrl >> 2;
        uint32_t active_buf = ctrl & 0x3;

        if (ctrl != prev) {
            printf("ctrl=0x%08X  frame=%u  buf=%u\n", ctrl, frame_cnt, active_buf);
            prev = ctrl;
            same_count = 0;
        } else {
            same_count++;
            if (same_count % 10 == 0)
                printf("  (no change for %d00ms, frame=%u)\n", same_count, frame_cnt);
        }
        usleep(100000);  /* 100ms */
    }

    munmap((void*)base, DDR_SIZE);
    close(fd);
    return 0;
}
