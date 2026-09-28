// Host unit test for prim_assemble.h -- the fan/strip -> triangle-list
// expansion that handle_draw runs before rasterizing. See the header for why
// this exists (every GameMaker fan was being dropped).
#include "prim_assemble.h"
#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

// A vertex stand-in: the expansion is index arithmetic, so an int id is enough
// to assert exactly which source vertex landed in which slot.
struct V { int id; };
static bool operator==(const V &a, const V &b) { return a.id == b.id; }

static std::vector<int> ids(const std::vector<V> &v) {
    std::vector<int> o; for (const V &x : v) o.push_back(x.id); return o;
}
static bool eq(const std::vector<V> &got, const std::vector<int> &want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); i++) if (got[i].id != want[i]) return false;
    return true;
}
static void dump(const char *what, const std::vector<V> &got) {
    fprintf(stderr, "  %s got:", what);
    for (int i : ids(got)) fprintf(stderr, " %d", i);
    fprintf(stderr, "\n");
}

int main() {
    std::vector<V> out;
    V v[8]; for (int i = 0; i < 8; i++) v[i].id = i;

    // --- mode predicate ---
    CHECK(prim_needs_assembly(PRIM_GL_TRIANGLE_FAN));
    CHECK(prim_needs_assembly(PRIM_GL_TRIANGLE_STRIP));
    CHECK(!prim_needs_assembly(PRIM_GL_TRIANGLES));   // already a list; must pass through
    CHECK(!prim_needs_assembly(0x0000u));             // GL_POINTS
    CHECK(!prim_needs_assembly(0x0002u));             // GL_LINE_LOOP

    // --- a triangle list is NOT expanded (0 = "use the input as-is") ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLES, v, 6, out) == 0);
    CHECK(out.empty());

    // --- fan: the measured Cursed Castilla EX case, a 5-vertex fan = 3 tris ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN, v, 5, out) == 9);
    if (!eq(out, {0,1,2, 0,2,3, 0,3,4})) { dump("fan5", out); fails++; }

    // --- fan: a quad, the common GameMaker sprite ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN, v, 4, out) == 6);
    if (!eq(out, {0,1,2, 0,2,3})) { dump("fan4", out); fails++; }

    // --- fan: the minimum, one triangle ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN, v, 3, out) == 3);
    if (!eq(out, {0,1,2})) { dump("fan3", out); fails++; }

    // --- strip: GL flips the winding on every odd triangle ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_STRIP, v, 5, out) == 9);
    if (!eq(out, {0,1,2, 2,1,3, 2,3,4})) { dump("strip5", out); fails++; }

    CHECK(prim_assemble(PRIM_GL_TRIANGLE_STRIP, v, 4, out) == 6);
    if (!eq(out, {0,1,2, 2,1,3})) { dump("strip4", out); fails++; }

    // Every strip triangle must face the same way. With ids standing in for
    // positions we can't measure area, but we CAN assert the swap happened on
    // exactly the odd triangles -- which is the whole content of the rule.
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_STRIP, v, 6, out) == 12);
    if (!eq(out, {0,1,2, 2,1,3, 2,3,4, 4,3,5})) { dump("strip6", out); fails++; }

    // --- degenerate inputs produce nothing, and never a partial triangle ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN,   v, 2, out) == 0);
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_STRIP, v, 2, out) == 0);
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN,   v, 0, out) == 0);
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN,   v, -1, out) == 0);
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN, (const V *)nullptr, 5, out) == 0);

    // --- the output is always a whole number of triangles ---
    for (int n = 3; n <= 8; n++) {
        CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN,   v, n, out) == (n - 2) * 3);
        CHECK(out.size() % 3 == 0);
        CHECK(prim_assemble(PRIM_GL_TRIANGLE_STRIP, v, n, out) == (n - 2) * 3);
        CHECK(out.size() % 3 == 0);
    }

    // --- out is cleared on a rejected call, so a caller can't reuse stale data ---
    CHECK(prim_assemble(PRIM_GL_TRIANGLE_FAN, v, 5, out) == 9);
    CHECK(prim_assemble(PRIM_GL_TRIANGLES,    v, 6, out) == 0);
    CHECK(out.empty());

    if (fails) { fprintf(stderr, "prim_assemble: %d FAILURE(S)\n", fails); return 1; }
    printf("prim_assemble: all checks passed\n");
    return 0;
}
