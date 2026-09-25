/* [fps-dip] Scanout-locked doorbell pacer — decision logic, host-testable.
 *
 * The frame loop used to be paced at its END (main.cpp fcap_wait: a leaky
 * bucket over the scanout counter). That released the loop just after a
 * scanout boundary, the host then spent ~5 ms building the frame, and the
 * fabric's ~12 ms (p50) put the publish ~1 ms AFTER the next boundary. Jitter
 * of a few hundred us then moved publishes to either side of the boundary:
 * two publishes in one scanout period (comp_fb_dma drops the second — it never
 * overwrites a frame the reader has not adopted) and none in the next (the
 * reader shows the old frame again). Measured on .81 2026-09-25 (fps-dip
 * harness, 120 s of play): 7.6 % of scanout frames repeated, 62 of 119 1-s
 * windows below 58 new frames while the engine submitted 59 per second.
 *
 * The pacer gates the DOORBELL instead: it may ring once the scanout counter
 * has moved past its value at the previous doorbell. So at most one batch is
 * rung per scanout period, it is rung right after a boundary whenever the host
 * is early, and the fabric has almost the whole period to publish before the
 * next one. A counter that moves backwards (core reload) re-bases.
 * Same rule as the Solarus core's mister_pace.h / Cash Cow DX's mf_present.
 */
#ifndef MF_PACE_H
#define MF_PACE_H
#include <stdint.h>

typedef struct {
    int      have;   /* a doorbell has been rung under the pacer */
    uint32_t last;   /* scan_frame_cnt at that doorbell */
} mf_pace_t;

/* May the doorbell ring now, with the scanout counter at `cnt`? */
static inline int mf_pace_ready(const mf_pace_t *p, uint32_t cnt) {
    return !p->have || cnt != p->last;
}

/* Record a doorbell rung with the scanout counter at `cnt`. */
static inline void mf_pace_rang(mf_pace_t *p, uint32_t cnt) {
    p->have = 1;
    p->last = cnt;
}
#endif
