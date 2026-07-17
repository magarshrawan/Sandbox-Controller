/* test_infinite_loop.c - simulates a runaway / malicious process that never
 * exits and never cooperates with any monitor. It also ignores SIGTERM to
 * demonstrate that the sandbox must be able to escalate to SIGKILL. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

static void ignore_sigterm(int sig) {
    (void)sig;
    /* Deliberately do nothing: this process refuses to die gracefully. */
}

int main(void) {
    signal(SIGTERM, ignore_sigterm);
    printf("test_infinite_loop: starting, pid=%d (ignores SIGTERM)\n", getpid());
    volatile unsigned long counter = 0;
    for (;;) {
        counter++;
        if (counter % 200000000UL == 0) {
            printf("test_infinite_loop: still spinning...\n");
            fflush(stdout);
        }
    }
    return 0;
}


