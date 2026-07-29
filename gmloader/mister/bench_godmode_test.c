/* Host-native test for bench_godmode_core (pattern: fps_overlay_test.c).
 * Build/run: make -f Makefile.gmloader bench-godmode-test */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "bench_godmode_core.h"

/* Fake code node with deliberately different layout from CCode: the core
 * walks via explicit field offsets, so layout must not matter. */
struct fake_code {
    int pad0;
    const char *name;
    double pad1;
    struct fake_code *next;
};

#define NEXT_OFF offsetof(struct fake_code, next)
#define NAME_OFF offsetof(struct fake_code, name)

static void test_name_matches(void)
{
    assert(bench_godmode_name_matches("gml_Object_obj_player_Collision_233") == 1);
    assert(bench_godmode_name_matches("gml_Object_obj_player_Collision_48") == 1);
    /* near-miss names that MUST NOT match */
    assert(bench_godmode_name_matches("gml_Object_obj_player_credits_Collision_0") == 0);
    assert(bench_godmode_name_matches("gml_Object_obj_player_Create_0") == 0);
    assert(bench_godmode_name_matches("gml_Object_obj_player_Alarm_0") == 0);
    assert(bench_godmode_name_matches("gml_Script_scr_damage") == 0);
    assert(bench_godmode_name_matches("") == 0);
    assert(bench_godmode_name_matches(NULL) == 0);
}

static void test_scan_and_is_blocked(void)
{
    struct fake_code n4 = {0, "gml_Object_obj_player_Collision_67",  0.0, NULL};
    struct fake_code n3 = {0, "gml_Object_obj_player_Create_0",      0.0, &n4};
    struct fake_code n2 = {0, "gml_Object_obj_player_Collision_233", 0.0, &n3};
    struct fake_code n1 = {0, "gml_Script_scr_damage",               0.0, &n2};
    struct fake_code n0 = {0, NULL,                                  0.0, &n1}; /* NULL name tolerated */

    const void *out[BENCH_GODMODE_MAX];
    int n = bench_godmode_scan(&n0, NEXT_OFF, NAME_OFF, out, BENCH_GODMODE_MAX);
    assert(n == 2);
    assert(out[0] == (const void *)&n2);
    assert(out[1] == (const void *)&n4);

    assert(bench_godmode_is_blocked(&n2, out, n) == 1);
    assert(bench_godmode_is_blocked(&n4, out, n) == 1);
    assert(bench_godmode_is_blocked(&n3, out, n) == 0);
    assert(bench_godmode_is_blocked(&n1, out, n) == 0);
    assert(bench_godmode_is_blocked(&n2, out, 0) == 0); /* empty set blocks nothing */
}

static void test_scan_edge_cases(void)
{
    const void *out[BENCH_GODMODE_MAX];
    /* empty list */
    assert(bench_godmode_scan(NULL, NEXT_OFF, NAME_OFF, out, BENCH_GODMODE_MAX) == 0);

    /* out_max cap: 3 matching nodes, room for 2 */
    struct fake_code m2 = {0, "gml_Object_obj_player_Collision_3", 0.0, NULL};
    struct fake_code m1 = {0, "gml_Object_obj_player_Collision_2", 0.0, &m2};
    struct fake_code m0 = {0, "gml_Object_obj_player_Collision_1", 0.0, &m1};
    assert(bench_godmode_scan(&m0, NEXT_OFF, NAME_OFF, out, 2) == 2);
}

int main(void)
{
    test_name_matches();
    test_scan_and_is_blocked();
    test_scan_edge_cases();
    printf("bench_godmode_test: ALL PASS\n");
    return 0;
}
