#include "joy_shm_reader.h"
#include "mister_joy_shm.h"

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static const MalditaJoyShm *g_shm = 0;

/* Hardware-verified mapping: MiSTer joy_mask bit -> SDL controller button slot.
 * SDL slot order: 0=A 1=B 2=X 3=Y 4=LS 5=RS 6=LT 7=RT 8=Back 9=Start
 *                 10=LStick 11=RStick 12=DpadUp 13=DpadDown 14=DpadLeft 15=DpadRight */
void JoyShm_MaskToButtons(uint32_t mask, unsigned char raw16[16]) {
    memset(raw16, 0, 16);
    raw16[15] = (mask >> 0) & 1u;  /* right  -> DPAD_RIGHT */
    raw16[14] = (mask >> 1) & 1u;  /* left   -> DPAD_LEFT  */
    raw16[13] = (mask >> 2) & 1u;  /* down   -> DPAD_DOWN  */
    raw16[12] = (mask >> 3) & 1u;  /* up     -> DPAD_UP    */
    raw16[0]  = (mask >> 4) & 1u;  /* Sword  -> A          */
    raw16[1]  = (mask >> 5) & 1u;  /* Action -> B          */
    raw16[2]  = (mask >> 6) & 1u;  /* Item1  -> X          */
    raw16[3]  = (mask >> 7) & 1u;  /* Item2  -> Y          */
    raw16[9]  = (mask >> 8) & 1u;  /* Pause  -> Start      */
}

bool JoyShm_Init(void) {
    const char *path = getenv("GMLOADER_JOY_SHM");
    if (!path || !*path) return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    const MalditaJoyShm *p = (const MalditaJoyShm*)mmap(0, sizeof(MalditaJoyShm),
                                                        PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    if (p->magic != MALDITA_JOY_SHM_MAGIC || p->version != MALDITA_JOY_SHM_VERSION) {
        munmap((void*)p, sizeof(MalditaJoyShm));
        return false;
    }
    g_shm = p;
    return true;
}

bool JoyShm_IsActive(void) { return g_shm != 0; }

uint32_t JoyShm_ReadMask(int player) {
    if (!g_shm || player < 0 || player >= MALDITA_JOY_MAX_PLAYERS) return 0;
    return g_shm->joy_mask[player];   /* single-word atomic load */
}

void JoyShm_Shutdown(void) {
    if (g_shm) { munmap((void*)g_shm, sizeof(MalditaJoyShm)); g_shm = 0; }
}
