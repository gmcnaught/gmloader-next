#include "joy_ddr_reader.h"

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

/* The Maldita core's relocated framebuffer block: Maldita.sv FB_QW_BASE
 * (29'h077E8000, a qword address) << 3 = byte 0x3BF40000. The ctrl word,
 * joystick words, and video buffers all moved here as a block — NOT the
 * legacy OpenBOR 0x3A000000 base that native_video_writer.c still uses. */
#define JOYDDR_PHYS_BASE  0x3BF40000u
#define JOYDDR_MAP_SIZE   0x1000u
#define JOYDDR_P1_OFFSET  0x008u
#define JOYDDR_P2_OFFSET  0x018u
#define JOYDDR_MAX_PLAYERS 2

static volatile const uint8_t *g_base = 0;

bool JoyDdr_Init(void) {
    if (g_base) return true;
    /* GMLOADER_JOY_DDR points a host test at a regular file standing in for
     * the /dev/mem region; on device the real physical region is mapped. */
    const char *override_path = getenv("GMLOADER_JOY_DDR");
    int fd;
    off_t off;
    if (override_path && *override_path) {
        fd = open(override_path, O_RDONLY);
        off = 0;
    } else {
        fd = open("/dev/mem", O_RDONLY | O_SYNC);
        off = (off_t)JOYDDR_PHYS_BASE;
    }
    if (fd < 0) return false;
    void *p = mmap(0, JOYDDR_MAP_SIZE, PROT_READ, MAP_SHARED, fd, off);
    close(fd);
    if (p == MAP_FAILED) return false;
    g_base = (volatile const uint8_t *)p;
    return true;
}

bool JoyDdr_IsActive(void) { return g_base != 0; }

uint32_t JoyDdr_ReadMask(int player) {
    if (!g_base || player < 0 || player >= JOYDDR_MAX_PLAYERS) return 0;
    uint32_t off = player ? JOYDDR_P2_OFFSET : JOYDDR_P1_OFFSET;
    return *(volatile const uint32_t *)(g_base + off);   /* aligned 32-bit load */
}

void JoyDdr_Shutdown(void) {
    if (g_base) { munmap((void *)g_base, JOYDDR_MAP_SIZE); g_base = 0; }
}
