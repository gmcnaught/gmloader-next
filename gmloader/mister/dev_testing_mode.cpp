#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "so_util.h"
#include "libyoyo.h"
#include "dev_testing_mode.h"

/* mister_joy_shm.h bit layout: bit6=Item1, bit7=Item2. */
#define CHORD_MASK   ((1u << 6) | (1u << 7))
#define KEY_J        74
#define DEVSKIP_GAP  30     /* steps between automatic J presses */

static int g_enabled;
static int g_var_id = -1;
static int g_skip_from = 4, g_skip_to = -1;
static int g_chord_prev;
static int g_press_now, g_press_prev;
static long g_skip_wait;
static int g_last_room = -2;

static ABI_ATTR int  (*Code_Variable_Find)(const char *name);
static ABI_ATTR void (*Variable_Global_SetVar)(int id, int index, RValue *val);

void patch_dev_testing_mode(struct so_module *mod)
{
    const char *env = getenv("GMLOADER_TESTING_MODE");
    if (env == NULL || strcmp(env, "1") != 0)
        return;

    FIND_SYMBOL(mod, Code_Variable_Find, "_Z18Code_Variable_FindPc");
    FIND_SYMBOL(mod, Variable_Global_SetVar, "_Z22Variable_Global_SetVariiP6RValue");
    if (!Code_Variable_Find || !Variable_Global_SetVar || !Current_Room ||
        !_IO_KeyPressed || !_IO_KeyDown) {
        warning("dev_testing_mode: runner symbols missing; not enabled\n");
        return;
    }

    const char *to = getenv("GMLOADER_DEVSKIP_TO");
    const char *from = getenv("GMLOADER_DEVSKIP_FROM");
    if (to && *to) g_skip_to = atoi(to);
    if (from && *from) g_skip_from = atoi(from);
    g_enabled = 1;
    warning("dev_testing_mode: enabled (Item1+Item2 = next room; devskip %d..%d)\n",
            g_skip_from, g_skip_to);
}

static void assert_testing_mode(void)
{
    /* Resolved lazily: the variable table is filled while the game loads,
     * after patch time. */
    if (g_var_id < 0) {
        g_var_id = Code_Variable_Find("testing_mode");
        if (g_var_id < 0)
            return;
        warning("dev_testing_mode: global.testing_mode id=%d\n", g_var_id);
    }
    RValue v;
    memset(&v, 0, sizeof(v));
    v.kind = VALUE_REAL;
    v.rvalue.val = 1.0;
    /* INT_MIN = scalar (not an array element), per this runner's SET_RValue. */
    Variable_Global_SetVar(g_var_id, (int)0x80000000, &v);
}

void DevTestingMode_Step(uint32_t *mask_p0)
{
    if (!g_enabled)
        return;

    int room = (int)*Current_Room;
    if (room != g_last_room) {
        warning("dev_testing_mode: room %d\n", room);
        g_last_room = room;
    }
    assert_testing_mode();

    /* One J press per chord press; hide the chord's buttons from the game. */
    int chord = (*mask_p0 & CHORD_MASK) == CHORD_MASK;
    int press = chord && !g_chord_prev;
    g_chord_prev = chord;
    if (chord)
        *mask_p0 &= ~CHORD_MASK;

    if (g_skip_to >= 0) {
        if (room >= g_skip_to) {
            warning("dev_testing_mode: devskip reached room %d\n", room);
            g_skip_to = -1;
        } else if (room >= g_skip_from && ++g_skip_wait >= DEVSKIP_GAP) {
            g_skip_wait = 0;
            press = 1;
        }
    }

    /* keyboard_check_pressed reads _IO_KeyPressed directly (IO_Start_Step is
     * hooked out, so nothing else clears it): hold it for exactly one step. */
    g_press_prev = g_press_now;
    g_press_now = press;
    if (g_press_now) {
        _IO_KeyDown[KEY_J] = 1;
        _IO_KeyPressed[KEY_J] = 1;
    } else if (g_press_prev) {
        _IO_KeyDown[KEY_J] = 0;
        _IO_KeyPressed[KEY_J] = 0;
    }
}
