/* bench_godmode core: pure matching/scan logic, host-testable.
 * Suppresses obj_player collision events for FPS benchmarking; see
 * maldita.castilla-mister docs/superpowers/specs/2026-07-29-bench-godmode-design.md */
#ifndef BENCH_GODMODE_CORE_H
#define BENCH_GODMODE_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_GODMODE_PREFIX "gml_Object_obj_player_Collision_"
#define BENCH_GODMODE_MAX 32

/* 1 iff name is non-NULL and starts with BENCH_GODMODE_PREFIX. */
int bench_godmode_name_matches(const char *name);

/* Walk an intrusive singly-linked list starting at `first`, reading the
 * next-pointer at byte offset `next_off` and a `const char *` name at byte
 * offset `name_off` of each node. Store nodes whose name matches into `out`
 * (up to `out_max`); return the count stored. */
int bench_godmode_scan(const void *first, size_t next_off, size_t name_off,
                       const void **out, int out_max);

/* 1 iff `code` is one of the `count` pointers in `blocked`. */
int bench_godmode_is_blocked(const void *code, const void *const *blocked, int count);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_GODMODE_CORE_H */
