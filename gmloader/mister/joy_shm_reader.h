#ifndef JOY_SHM_READER_H
#define JOY_SHM_READER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool     JoyShm_Init(void);
bool     JoyShm_IsActive(void);
uint32_t JoyShm_ReadMask(int player);
void     JoyShm_MaskToButtons(uint32_t mask, unsigned char raw16[16]);
void     JoyShm_Shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
