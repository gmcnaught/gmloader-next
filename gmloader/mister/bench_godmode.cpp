#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "platform.h"
#include "so_util.h"
#include "libyoyo.h"
#include "bench_godmode.h"
#include "bench_godmode_core.h"

static const void *g_blocked[BENCH_GODMODE_MAX];
static int g_blocked_count = -1; /* -1 = code list not scanned yet */

/* Lazy one-time scan: the code list is fully populated during game load,
 * before the first Code_Execute dispatch ever fires. */
static int godmode_check(struct CCode *code)
{
    if (g_blocked_count < 0) {
        g_blocked_count = bench_godmode_scan(
            g_pFirstCode ? (const void *)*g_pFirstCode : NULL,
            offsetof(struct CCode, m_next),
            offsetof(struct CCode, m_name),
            g_blocked, BENCH_GODMODE_MAX);
        warning("bench_godmode: blocking %d obj_player collision events\n",
                g_blocked_count);
    }
    return bench_godmode_is_blocked(code, g_blocked, g_blocked_count);
}

ABI_ATTR static long godmode_code_execute(void *self, void *other,
                                          struct CCode *code, RValue *ret)
{
    if (godmode_check(code))
        return 0; /* skip event; 0 matches the USE_LUA hook's skip path */
    return ExecuteIt(self, other, code, ret);
}

ABI_ATTR static long godmode_code_execute_flags(void *self, void *other,
                                                struct CCode *code, RValue *ret,
                                                int flags)
{
    if (godmode_check(code))
        return 0;
    return ExecuteIt_flags(self, other, code, ret, flags);
}

void patch_bench_godmode(struct so_module *mod)
{
    const char *env = getenv("GMLOADER_GODMODE");
    if (env == NULL || strcmp(env, "1") != 0)
        return; /* off path: no hooks, bit-identical behavior */

    if (ExecuteIt == NULL || ExecuteIt_flags == NULL) {
        warning("bench_godmode: ExecuteIt symbols unresolved; not hooking\n");
        return;
    }

    warning("bench_godmode: enabled (GMLOADER_GODMODE=1)\n");
    /* Same Code_Execute pair the USE_LUA layer hooks (lua.cpp); if USE_LUA is
     * ever compiled into a MiSTer build, whichever patch_* runs last wins the
     * hook (no chaining). USE_LUA is off in MiSTer builds today. */
    hook_symbol(mod, "_Z12Code_ExecuteP9CInstanceS0_P5CCodeP6RValue",
                (uintptr_t)&godmode_code_execute, 1);
    hook_symbol(mod, "_Z12Code_ExecuteP9CInstanceS0_P5CCodeP6RValuei",
                (uintptr_t)&godmode_code_execute_flags, 1);
}
