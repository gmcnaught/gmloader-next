#include "mfgpu.h"
#include <stdio.h>
int main(void){
    static unsigned char ring[4096], heap[4096];
    blt_emitter_t e; blt_emitter_init(&e, ring, sizeof ring, heap, sizeof heap);
    mfgpu_t *m = mfgpu_create(&e);
    if(!m){ printf("FAIL\n"); return 1; }
    mfgpu_destroy(m);
    printf("mfgpu link smoke OK\n"); return 0;
}
