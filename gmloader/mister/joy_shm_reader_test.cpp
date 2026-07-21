#include <stdio.h>
#include "joy_shm_reader.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)){printf("FAIL %s (line %d)\n",#c,__LINE__);fails++;} } while(0)

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

    if (fails) { printf("%d checks FAILED\n", fails); return 1; }
    printf("joy_shm_reader mapping OK\n");
    return 0;
}
