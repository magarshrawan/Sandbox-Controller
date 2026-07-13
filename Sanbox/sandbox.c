/*
 * sandbox.c
 * ---------
 * A process-isolation sandbox controller.
 *
 * Usage:
 *   ./sandbox <wall_seconds> <cpu_seconds> <mem_kb> <path_to_binary> [args...]
 *
 * Design summary:
 *   - fork()+execve() launches the untrusted binary as a completely separate
 *     process (own address space, own memory, own fault domain).
 *   - The child is placed in its own process group (setpgid) so signals sent
 *     with killpg() reach it and any children it spawns.
 *   - Three pthreads independently supervise the child from OUTSIDE it:
 *       1) reaper_thread   - blocking waitpid(), detects natural exit
 *       2) timer_thread    - polls wall-clock time via clock_gettime()
 *       3) resource_thread - polls /proc/<pid>/status and /proc/<pid>/stat
 *   - All shared state (child_exited, terminated flag, kill_issued flag,
 *     exit_status, reason string) lives in one struct guarded by a single
 *     mutex, so only one thread can ever act on a violation
 *     (race-free termination).
 *   - Enforcement escalates SIGTERM -> grace period -> SIGKILL.
 *   - The untrusted child never receives a custom signal handler and never
 *     participates in deciding when it dies; all authority lives in the
 *     parent (principle of external, non-cooperative enforcement).
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -pthread -o sandbox sandbox.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

/* ---------- Tunables ---------- */
#define POLL_INTERVAL_US   200000   /* 200 ms poll interval for monitors   */
#define GRACE_PERIOD_SEC   2        /* time given to child after SIGTERM  */
#define REASON_LEN         256

/* ---------- Shared, mutex-protected sandbox state ---------- */
typedef struct {
    pthread_mutex_t lock;
    pid_t   child_pid;
    struct  timespec start_time;

    long    wall_limit_sec;
    long    cpu_limit_sec;
    long    mem_limit_kb;

    int     child_exited;      /* set by reaper thread when waitpid returns */
    int     exit_status;       /* raw status from waitpid                   */

    int     terminated_by_sandbox; /* a violation was detected               */
    int     kill_issued;           /* enforcement sequence already started   */
    char    reason[REASON_LEN];
} sandbox_state_t;

static sandbox_state_t g;

/* ---------- Logging helper: timestamped, thread-safe (stdio is safe) ---------- */
static void log_msg(const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmv);

    fprintf(stdout, "[%s.%03ld] ", timebuf, ts.tv_nsec / 1000000);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}

/* ---------- Elapsed wall-clock seconds since sandbox launch ---------- */
static double elapsed_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g.start_time.tv_sec) +
           (now.tv_nsec - g.start_time.tv_nsec) / 1e9;
}

/*
 * enforce_termination()
 * ----------------------
 * Called by whichever monitor thread first detects a violation.
 * The mutex + kill_issued flag guarantee this body runs at most once,
 * even if two threads detect a violation at nearly the same instant.
 */
static void enforce_termination(const char *reason) {
    pthread_mutex_lock(&g.lock);
    if (g.kill_issued || g.child_exited) {
        /* Someone else already handled it, or the child is already gone. */
        pthread_mutex_unlock(&g.lock);
        return;
    }
    g.kill_issued = 1;
    g.terminated_by_sandbox = 1;
    strncpy(g.reason, reason, REASON_LEN - 1);
    pid_t pid = g.child_pid;
    pthread_mutex_unlock(&g.lock);

    log_msg("POLICY VIOLATION: %s", reason);
    log_msg("Sending SIGTERM to process group %d", pid);
    killpg(pid, SIGTERM);

    /* Grace period: poll in small steps so we exit early if it dies quickly */
    for (int i = 0; i < GRACE_PERIOD_SEC * 10; i++) {
        usleep(100000);
        pthread_mutex_lock(&g.lock);
        int exited = g.child_exited;
        pthread_mutex_unlock(&g.lock);
        if (exited) {
            log_msg("Child terminated gracefully after SIGTERM.");
            return;
        }
    }

    pthread_mutex_lock(&g.lock);
    int exited = g.child_exited;
    pthread_mutex_unlock(&g.lock);
    if (!exited) {
        log_msg("Child ignored SIGTERM. Escalating to SIGKILL on group %d", pid);
        killpg(pid, SIGKILL);
    }
}

/* ---------- Thread 1: Reaper ---------- */
static void *reaper_thread(void *arg) {
    (void)arg;
    int status;
    pid_t r = waitpid(g.child_pid, &status, 0);
    pthread_mutex_lock(&g.lock);
    if (r == g.child_pid) {
        g.child_exited = 1;
        g.exit_status = status;
    }
    pthread_mutex_unlock(&g.lock);

    if (r == g.child_pid) {
        if (WIFEXITED(status)) {
            log_msg("Child pid %d exited normally, code=%d", r, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_msg("Child pid %d was killed by signal %d (%s)", r,
                     WTERMSIG(status), strsignal(WTERMSIG(status)));
        }
    }
    return NULL;
}

/* ---------- Thread 2: Wall-clock timer monitor ---------- */
static void *timer_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        int done = g.child_exited || g.kill_issued;
        long limit = g.wall_limit_sec;
        pthread_mutex_unlock(&g.lock);
        if (done) break;

        double el = elapsed_seconds();
        if (limit > 0 && el > (double)limit) {
            char reason[REASON_LEN];
            snprintf(reason, sizeof(reason),
                     "Wall-clock time limit exceeded (%.1fs > %lds)", el, limit);
            enforce_termination(reason);
            break;
        }
        usleep(POLL_INTERVAL_US);
    }
    return NULL;
}

/* ---------- Helpers to read /proc for the resource monitor ---------- */

/* Returns resident memory in KB from /proc/<pid>/status, or -1 on failure */
static long read_vmrss_kb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

/* Returns total CPU time (user+sys) in seconds from /proc/<pid>/stat, or -1 */
static double read_cpu_seconds(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1.0;

    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1.0; }
    fclose(f);

    /* Fields are space separated; field 2 (comm) may contain spaces inside
     * parentheses, so skip past the last ')' before counting fields. */
    char *p = strrchr(buf, ')');
    if (!p) return -1.0;
    p += 2; /* skip ") " */

    long utime = 0, stime = 0;
    /* From field 3 (state) onward. utime is field 14, stime is field 15
     * relative to the start of the whole line; relative to p (field 3) they
     * are the 12th and 13th tokens. */
    char *tok = strtok(p, " ");
    int idx = 3; /* p starts at field 3 */
    while (tok) {
        if (idx == 14) utime = atol(tok);
        if (idx == 15) { stime = atol(tok); break; }
        tok = strtok(NULL, " ");
        idx++;
    }

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    return (double)(utime + stime) / (double)clk_tck;
}

/* ---------- Thread 3: Resource monitor (memory + CPU time) ---------- */
static void *resource_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        int done = g.child_exited || g.kill_issued;
        long mem_limit = g.mem_limit_kb;
        long cpu_limit = g.cpu_limit_sec;
        pid_t pid = g.child_pid;
        pthread_mutex_unlock(&g.lock);
        if (done) break;

        long rss = read_vmrss_kb(pid);
        double cpu = read_cpu_seconds(pid);

        if (rss >= 0 && cpu >= 0) {
            log_msg("monitor: pid=%d rss=%ldKB cpu=%.2fs", pid, rss, cpu);
        }

        if (mem_limit > 0 && rss > mem_limit) {
            char reason[REASON_LEN];
            snprintf(reason, sizeof(reason),
                     "Memory limit exceeded (%ldKB > %ldKB)", rss, mem_limit);
            enforce_termination(reason);
            break;
        }
        if (cpu_limit > 0 && cpu > (double)cpu_limit) {
            char reason[REASON_LEN];
            snprintf(reason, sizeof(reason),
                     "CPU time limit exceeded (%.2fs > %lds)", cpu, cpu_limit);
            enforce_termination(reason);
            break;
        }
        usleep(POLL_INTERVAL_US);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <wall_seconds> <cpu_seconds> <mem_kb> <binary> [args...]\n"
            "  Pass 0 for any limit to disable it.\n", argv[0]);
        return 2;
    }

    long wall_limit = atol(argv[1]);
    long cpu_limit  = atol(argv[2]);
    long mem_limit  = atol(argv[3]);
    char *target    = argv[4];

    memset(&g, 0, sizeof(g));
    pthread_mutex_init(&g.lock, NULL);
    g.wall_limit_sec = wall_limit;
    g.cpu_limit_sec  = cpu_limit;
    g.mem_limit_kb   = mem_limit;

    log_msg("Sandbox starting. target=%s wall=%lds cpu=%lds mem=%ldKB",
             target, wall_limit, cpu_limit, mem_limit);

    clock_gettime(CLOCK_MONOTONIC, &g.start_time);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* ---- Child: becomes the untrusted process, no special handling ---- */
        /* New process group so the parent can signal it (and any of its own
         * children) as a unit, and so it cannot piggyback on the sandbox's
         * own group / job-control signals. */
        if (setpgid(0, 0) < 0) {
            _exit(126);
        }
        execv(target, &argv[4]);
        /* execv only returns on failure */
        fprintf(stderr, "sandbox: execv failed for %s: %s\n", target, strerror(errno));
        _exit(127);
    }

    /* ---- Parent: the supervisor ---- */
    g.child_pid = pid;
    /* Also set the group from the parent side to close the race where the
     * child hasn't called setpgid yet when we try to signal it. */
    setpgid(pid, pid);

    log_msg("Child launched, pid=%d, pgid=%d", pid, pid);

    pthread_t t_reap, t_timer, t_res;
    pthread_create(&t_reap,  NULL, reaper_thread,   NULL);
    pthread_create(&t_timer, NULL, timer_thread,    NULL);
    pthread_create(&t_res,   NULL, resource_thread, NULL);

    /* Wait for the child to actually be gone (natural exit or enforced kill) */
    pthread_join(t_reap, NULL);

    /* Child is confirmed gone; make sure the monitor threads notice and stop */
    pthread_join(t_timer, NULL);
    pthread_join(t_res, NULL);

    pthread_mutex_lock(&g.lock);
    int terminated_by_us = g.terminated_by_sandbox;
    char reason_copy[REASON_LEN];
    strncpy(reason_copy, g.reason, REASON_LEN);
    int status = g.exit_status;
    pthread_mutex_unlock(&g.lock);

    log_msg("---- Sandbox summary ----");
    log_msg("Total wall time: %.2fs", elapsed_seconds());
    if (terminated_by_us) {
        log_msg("Result: TERMINATED BY SANDBOX (%s)", reason_copy);
    } else if (WIFEXITED(status)) {
        log_msg("Result: child exited normally, code=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_msg("Result: child killed by signal %d", WTERMSIG(status));
    }

    pthread_mutex_destroy(&g.lock);
    return terminated_by_us ? 1 : 0;
}
