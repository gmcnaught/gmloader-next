#ifndef MF_SEAM_STAT_H
#define MF_SEAM_STAT_H
// Phase 4 Stage A — the submit-seam decomposition accumulator.
//
//   period = host + block + pub
//     host  = doorbell N            -> mf_publish_barrier entry
//     block = barrier entry         -> barrier returns true  (the real wait on the fabric)
//     pub   = barrier return        -> doorbell N+1
//
// Three CONSECUTIVE intervals, so no term is a residual. That shape is the point:
// the 2026-07-30 exposed-cost audit sec 6.2 records BLITPROF's `logic` residual
// silently absorbing a 16.9 ms deferred await, because a residual absorbs whatever
// nobody named.
//
// Pure by construction: no I/O, no globals, no device headers, so all of the
// arithmetic is unit-testable on the host. The device side contributes timestamps
// and nothing else.
#include <stdint.h>
#include <string.h>

#define MF_SEAM_WINDOW   30      /* frames per report; matches MFSUBMIT's window */
#define MF_SEAM_TOL_MS   0.05    /* identity slack for the timestamp reads themselves */

typedef struct {
    unsigned n;
    unsigned suspect;            /* frames whose parts did not sum to their period */
    double   host_sum, block_sum, pub_sum, period_sum;
} mf_seam_acc_t;

typedef struct {
    double   host_ms, block_ms, pub_ms, period_ms;
    unsigned suspect;
} mf_seam_out_t;

static inline void mf_seam_reset(mf_seam_acc_t *a) { memset(a, 0, sizeof *a); }

/* frame_ms is the fabric's own compute counter (C_DONE.hi) for the batch this
   frame's `block` waited on. Unused until Task 2 introduces `notice`; taken now
   so the call sites written in Task 4 never have to change signature. */
static inline void mf_seam_add(mf_seam_acc_t *a, double host_ms, double block_ms,
                               double pub_ms, double period_ms, double frame_ms) {
    (void)frame_ms;
    a->n++;
    a->host_sum   += host_ms;
    a->block_sum  += block_ms;
    a->pub_sum    += pub_ms;
    a->period_sum += period_ms;

    double d = (host_ms + block_ms + pub_ms) - period_ms;
    if (d < 0.0) d = -d;
    if (d >= MF_SEAM_TOL_MS) a->suspect++;
}

static inline int mf_seam_ready(const mf_seam_acc_t *a) {
    return a->n >= MF_SEAM_WINDOW;
}

static inline void mf_seam_derive(const mf_seam_acc_t *a, mf_seam_out_t *o) {
    const double n = a->n ? (double)a->n : 1.0;   /* an empty window reports zeros */
    o->host_ms   = a->host_sum   / n;
    o->block_ms  = a->block_sum  / n;
    o->pub_ms    = a->pub_sum    / n;
    o->period_ms = a->period_sum / n;
    o->suspect   = a->suspect;
}

#endif /* MF_SEAM_STAT_H */
