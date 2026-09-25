/* [fps-dip] See cpu_isolate.h. */
#define _GNU_SOURCE
#include "cpu_isolate.h"
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { CPUISO_PERIOD = 600, CPUISO_RENDER_CPU = 0, CPUISO_OTHER_CPU = 1 };

static int set_cpu(pid_t tid, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(tid, sizeof set, &set);
}

void CpuIsolate_Sweep(void) {
    static int enabled = -1;
    static unsigned calls = 0;
    if (enabled < 0) {
        const char *e = getenv("GMLOADER_CPUISOLATE");
        enabled = !(e && e[0] == '0');
        if (sysconf(_SC_NPROCESSORS_ONLN) < 2) enabled = 0;
    }
    if (!enabled || (calls++ % CPUISO_PERIOD) != 0) return;

    const pid_t self = (pid_t)syscall(SYS_gettid);
    set_cpu(self, CPUISO_RENDER_CPU);
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    int moved = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const pid_t tid = (pid_t)atoi(de->d_name);
        if (tid <= 0 || tid == self) continue;
        cpu_set_t cur;
        if (sched_getaffinity(tid, sizeof cur, &cur) == 0 &&
            CPU_COUNT(&cur) == 1 && CPU_ISSET(CPUISO_OTHER_CPU, &cur))
            continue;
        if (set_cpu(tid, CPUISO_OTHER_CPU) == 0) moved++;
    }
    closedir(d);
    if (moved && calls == 1)
        fprintf(stderr, "cpu-isolate: render thread %d on CPU%d, %d engine thread(s) moved to CPU%d\n",
                (int)self, CPUISO_RENDER_CPU, moved, CPUISO_OTHER_CPU);
}
