/* test_cpu_hog.c
 * ----------------
 * Unlike test_infinite_loop.c (which burns CPU continuously and mainly
 * exercises the WALL-CLOCK limit), this program alternates between short
 * bursts of heavy CPU work and short sleeps. That means wall-clock time
 * passes much faster than CPU time accumulates, so if only a wall-clock
 * limit were enforced this process could run for a long time undetected.
 * This is specifically designed to validate the CPU-time monitoring path
 * (read_cpu_seconds() in sandbox.c) independently of the wall-clock path.
 *
 * It also ignores SIGTERM, like test_infinite_loop.c, to confirm SIGKILL
 * escalation works for CPU-limit violations too, not just wall-clock ones.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

static void ignore_sigterm(int sig) {
    (void)sig;
}

/* Burn CPU for approximately `ms` milliseconds using a busy loop timed
 * against CLOCK_PROCESS_CPUTIME_ID (i.e. actual CPU time, not wall time). */
static void burn_cpu_ms(long ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    volatile unsigned long x = 0;
    for (;;) {
        for (int i = 0; i < 100000; i++) x++;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= ms) break;
    }
}

int main(void) {
    signal(SIGTERM, ignore_sigterm);
    printf("test_cpu_hog: starting, pid=%d (bursty CPU, ignores SIGTERM)\n", getpid());

    for (;;) {
        burn_cpu_ms(400);   /* 400ms of real CPU work            */
        printf("test_cpu_hog: burst done\n");
        fflush(stdout);
        usleep(300000);     /* 300ms sleep: costs wall time, no CPU */
    }
    return 0;
}
