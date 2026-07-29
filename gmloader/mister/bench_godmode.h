/* Env-gated (GMLOADER_GODMODE=1) suppression of obj_player collision events
 * so a tester can hold heavy areas for FPS benchmarking. Off path installs
 * no hooks. Spec: maldita.castilla-mister
 * docs/superpowers/specs/2026-07-29-bench-godmode-design.md */
#ifndef BENCH_GODMODE_H
#define BENCH_GODMODE_H

struct so_module;
void patch_bench_godmode(struct so_module *mod);

#endif /* BENCH_GODMODE_H */
