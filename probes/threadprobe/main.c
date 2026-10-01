/* PS5LM threadprobe: what a payload's threads look like to the scheduler.
 *
 * Prints the priority and CPU affinity of the main thread and of a new
 * pthread, then runs short busy bursts on 1, 2 and 4 threads (one second
 * each) and reports how long each took, so a thread setup that starves the
 * system shows up as a short stall, not a dead console. Output goes to stdout
 * and /data/PS5LM/threadprobe.txt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int scePthreadGetprio(pthread_t thread, int *prio);
int scePthreadGetaffinity(pthread_t thread, uint64_t *mask);

static FILE *g_log;

static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
        fsync(fileno(g_log));
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void describe(const char *who) {
    int prio = -1;
    uint64_t mask = 0;
    const int rp = scePthreadGetprio(pthread_self(), &prio);
    const int ra = scePthreadGetaffinity(pthread_self(), &mask);
    out("%-8s sce prio %d (rc 0x%x), affinity 0x%04llx (rc 0x%x), nice %d\n", who, prio, rp,
        (unsigned long long)mask, ra, getpriority(PRIO_PROCESS, 0));
}

static void *hello(void *arg) {
    (void)arg;
    describe("thread");
    return NULL;
}

static void *burn(void *arg) {
    const double until = *(const double *)arg;
    volatile uint64_t x = 0;
    while (now_s() < until)
        for (int i = 0; i < 100000; i++)
            x += (uint64_t)i;
    return NULL;
}

int main(void) {
    mkdir("/data/PS5LM", 0777);
    g_log = fopen("/data/PS5LM/threadprobe.txt", "w");

    out("PS5LM threadprobe, pid %d\n", (int)getpid());
    describe("main");

    pthread_t t;
    const int rc = pthread_create(&t, NULL, hello, NULL);
    out("pthread_create rc %d\n", rc);
    if (rc == 0)
        pthread_join(t, NULL);

    const int counts[] = {1, 2, 4};
    for (int k = 0; k < 3; k++) {
        const int n = counts[k];
        pthread_t th[4];
        double until = now_s() + 1.0;
        const double t0 = now_s();
        for (int i = 0; i < n; i++)
            pthread_create(&th[i], NULL, burn, &until);
        for (int i = 0; i < n; i++)
            pthread_join(th[i], NULL);
        out("burst    %d threads x 1 s took %.2f s\n", n, now_s() - t0);
    }

    out("done\n");
    if (g_log)
        fclose(g_log);
    return 0;
}
