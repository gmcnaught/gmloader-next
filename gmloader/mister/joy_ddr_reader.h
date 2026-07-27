#ifndef JOY_DDR_READER_H
#define JOY_DDR_READER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* OpenBOR-contract DDR joystick reader: the FPGA core (openbor_video_reader)
 * publishes the MiSTer-normalized joystick words into the native-video DDR
 * region each frame — P1 at byte +0x008, P2 at +0x018 (low 32 bits of the
 * qword). Same mechanism as OpenBOR_7533 / sonic-mania. Bit layout matches
 * mister_joy_shm.h: bit0=right 1=left 2=down 3=up, then the CONF_STR J1
 * buttons (Sword,Action,Item1,Item2,Pause) — JoyShm_MaskToButtons applies. */
bool     JoyDdr_Init(void);
bool     JoyDdr_IsActive(void);
uint32_t JoyDdr_ReadMask(int player);
void     JoyDdr_Shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
