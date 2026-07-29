#include <string.h>
#include "bench_godmode_core.h"

int bench_godmode_name_matches(const char *name)
{
    if (name == NULL)
        return 0;
    return strncmp(name, BENCH_GODMODE_PREFIX,
                   sizeof(BENCH_GODMODE_PREFIX) - 1) == 0;
}

int bench_godmode_scan(const void *first, size_t next_off, size_t name_off,
                       const void **out, int out_max)
{
    int count = 0;
    const char *node = (const char *)first;

    while (node != NULL && count < out_max) {
        const char *name = *(const char *const *)(node + name_off);
        if (bench_godmode_name_matches(name))
            out[count++] = node;
        node = *(const char *const *)(node + next_off);
    }
    return count;
}

int bench_godmode_is_blocked(const void *code, const void *const *blocked, int count)
{
    for (int i = 0; i < count; i++) {
        if (blocked[i] == code)
            return 1;
    }
    return 0;
}
