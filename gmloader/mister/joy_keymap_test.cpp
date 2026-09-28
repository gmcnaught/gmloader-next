// joy_keymap_test.cpp -- host test for joy_keymap.h (make -f Makefile.gmloader joy-keymap-test)
#include "joy_keymap.h"
#include <stdio.h>
#include <vector>
#include <utility>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef std::vector<std::pair<int,int>> Ev;
static Ev step(JoyKeymap *m, uint32_t mask) {
    Ev ev; joykey_step(m, mask, [&](int k, int d) { ev.push_back({k, d}); }); return ev;
}

int main() {
    JoyKeymap m;
    // The Cursed Castilla EX map (PortMaster cursedcastilla.gptk equivalent).
    CHECK(joykey_parse(&m, "0=right,1=left,2=down,3=up,4=z,5=x,6=x,7=z,8=esc") == 9);
    CHECK(m.key[0] == JK_RIGHT && m.key[3] == JK_UP && m.key[4] == JK_CHAR_BASE + 'z' && m.key[8] == JK_ESC);

    CHECK(step(&m, 0).empty());
    // UP alone -> only the up arrow; never a face-button key (the defect being fixed).
    Ev e = step(&m, 1u << 3);
    CHECK(e.size() == 1 && e[0].first == JK_UP && e[0].second == 1);
    CHECK(step(&m, 1u << 3).empty());                               // held: no repeat
    e = step(&m, 0);
    CHECK(e.size() == 1 && e[0].first == JK_UP && e[0].second == 0);
    // LEFT alone -> left arrow only.
    e = step(&m, 1u << 1);
    CHECK(e.size() == 1 && e[0].first == JK_LEFT && e[0].second == 1);
    step(&m, 0);
    // A (bit4) and Y (bit7) share 'z': one press, released only when both are up.
    e = step(&m, 1u << 4);
    CHECK(e.size() == 1 && e[0].first == JK_CHAR_BASE + 'z' && e[0].second == 1);
    CHECK(step(&m, (1u << 4) | (1u << 7)).empty());
    CHECK(step(&m, 1u << 7).empty());
    e = step(&m, 0);
    CHECK(e.size() == 1 && e[0].first == JK_CHAR_BASE + 'z' && e[0].second == 0);
    // Unmapped bits do nothing.
    CHECK(step(&m, 1u << 12).empty());

    // Malformed specs disable the whole map.
    CHECK(joykey_parse(&m, "0=right,1=bogus") == 0 && m.n == 0);
    CHECK(joykey_parse(&m, "40=z") == 0);
    CHECK(joykey_parse(&m, "x=z") == 0);
    CHECK(joykey_parse(&m, "") == 0);
    CHECK(joykey_parse(&m, " 5=X , 8=Escape ") == 2 && m.key[0] == JK_CHAR_BASE + 'x' && m.key[1] == JK_ESC);

    printf(fails ? "joy_keymap_test: %d FAILURE(S)\n" : "joy_keymap_test: OK\n", fails);
    return fails ? 1 : 0;
}
