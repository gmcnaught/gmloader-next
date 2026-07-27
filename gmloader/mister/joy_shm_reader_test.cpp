#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "joy_shm_reader.h"
#include "mister_joy_shm.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)){printf("FAIL %s (line %d)\n",#c,__LINE__);fails++;} } while(0)

// Write a producer-shaped file the reader can mmap, standing in for the
// MiSTer-side publisher.
static void write_shm_file(const char *path, uint32_t magic, uint32_t version,
                           uint32_t mask0) {
    MalditaJoyShm s;
    memset(&s, 0, sizeof(s));
    s.magic = magic;
    s.version = version;
    s.joy_mask[0] = mask0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    (void)!write(fd, &s, sizeof(s));
    close(fd);
}

int main(void) {
    unsigned char b[16];

    // Directions (MiSTer bits 0..3) map to SDL DPAD slots 15/14/13/12.
    JoyShm_MaskToButtons(1u<<0, b); CHECK(b[15]==1);   // right → DPAD_RIGHT
    JoyShm_MaskToButtons(1u<<1, b); CHECK(b[14]==1);   // left  → DPAD_LEFT
    JoyShm_MaskToButtons(1u<<2, b); CHECK(b[13]==1);   // down  → DPAD_DOWN
    JoyShm_MaskToButtons(1u<<3, b); CHECK(b[12]==1);   // up    → DPAD_UP

    // Buttons (MiSTer bits 4..8) map to SDL face/Start slots.
    JoyShm_MaskToButtons(1u<<4, b); CHECK(b[0]==1);    // Sword  → A
    JoyShm_MaskToButtons(1u<<5, b); CHECK(b[1]==1);    // Action → B
    JoyShm_MaskToButtons(1u<<6, b); CHECK(b[2]==1);    // Item1  → X
    JoyShm_MaskToButtons(1u<<7, b); CHECK(b[3]==1);    // Item2  → Y
    JoyShm_MaskToButtons(1u<<8, b); CHECK(b[9]==1);    // Pause  → Start

    // Unpressed bits stay 0.
    JoyShm_MaskToButtons(1u<<4, b); CHECK(b[1]==0 && b[15]==0);

    // --- Path resolution: env override wins; canonical contract path is the
    // fallback, so the reader works no matter which parent spawned the engine
    // (the main= wrapper exports the env; the Master_Daemon handler need not).
    setenv("GMLOADER_JOY_SHM", "/tmp/joyshm-env-override", 1);
    CHECK(strcmp(JoyShm_ResolvePath(), "/tmp/joyshm-env-override") == 0);
    setenv("GMLOADER_JOY_SHM", "", 1);   // empty string == unset
    CHECK(strcmp(JoyShm_ResolvePath(), MALDITA_JOY_SHM_PATH) == 0);
    unsetenv("GMLOADER_JOY_SHM");
    CHECK(strcmp(JoyShm_ResolvePath(), MALDITA_JOY_SHM_PATH) == 0);

    // --- Init end-to-end against a real file (via env; the canonical /dev/shm
    // path is not writable on all test hosts).
    const char *path = "/tmp/joyshm-reader-test";
    setenv("GMLOADER_JOY_SHM", path, 1);

    // Bad magic is rejected.
    write_shm_file(path, 0xDEADBEEFu, MALDITA_JOY_SHM_VERSION, 0);
    CHECK(JoyShm_Init() == false);
    CHECK(JoyShm_IsActive() == false);

    // Valid producer file: Init succeeds and the mask reads through.
    write_shm_file(path, MALDITA_JOY_SHM_MAGIC, MALDITA_JOY_SHM_VERSION, 0x113u);
    CHECK(JoyShm_Init() == true);
    CHECK(JoyShm_IsActive() == true);
    CHECK(JoyShm_ReadMask(0) == 0x113u);
    CHECK(JoyShm_ReadMask(1) == 0u);
    CHECK(JoyShm_ReadMask(-1) == 0u && JoyShm_ReadMask(MALDITA_JOY_MAX_PLAYERS) == 0u);
    JoyShm_Shutdown();
    CHECK(JoyShm_IsActive() == false);
    unlink(path);

    if (fails) { printf("%d checks FAILED\n", fails); return 1; }
    printf("joy_shm_reader mapping OK\n");
    return 0;
}
