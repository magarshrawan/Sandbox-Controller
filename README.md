# Sandbox Controller

A process-isolation sandbox controller written in C. It launches an
untrusted binary as a separate OS process and supervises it entirely from
the outside — enforcing wall-clock time, CPU-time, and memory limits
without any cooperation from the sandboxed process itself.

## Files

| File | Purpose |
|---|---|
| `sandbox.c` | The controller: fork/execve isolation, 3-thread supervision, signal-based enforcement |
| `test_normal.c` | Well-behaved test process — exits cleanly on its own |
| `test_infinite_loop.c` | Runaway process that ignores `SIGTERM`, to prove `SIGKILL` escalation works |
| `test_memory_hog.c` | Steadily allocates memory to trigger the memory limit |
| `test_cpu_hog.c` | Bursts CPU work between sleeps, to trigger the CPU-time limit independently of wall-clock |
| `logs_kali/` | Captured output from real runs of each scenario, generated on the target machine |
| `EDGE_CASES.md` | Documents invalid-input handling and concurrent-instance testing |
| `BUILD.md` | Build and usage instructions |

## Design summary

Three pthreads independently supervise the child process:

1. **Reaper thread** — blocks on `waitpid()`, detects natural exit.
2. **Timer thread** — polls wall-clock elapsed time via `clock_gettime()`.
3. **Resource thread** — polls `/proc/<pid>/status` (memory) and
   `/proc/<pid>/stat` (CPU time) for the child.

All shared state lives in one struct. The single most contended flag,
`kill_issued`, is a lock-free `atomic_int` synchronized with
`atomic_compare_exchange_strong()`, guaranteeing exactly one thread ever
starts the termination sequence even if two threads detect a violation at
the same instant. The remaining fields (exit status, reason string, etc.)
are protected by a mutex, since they must be updated together as a
logical group.

Enforcement escalates `SIGTERM` → grace period → `SIGKILL`, sent to the
child's entire process group (`killpg()`), so any subprocesses it spawns
are also reached. The child never receives a signal handler installed by
the sandbox and is never consulted about its own termination — all
authority stays with the supervising parent process.

## Build

```bash
gcc -Wall -Wextra -O2 -pthread -o sandbox sandbox.c
gcc -Wall -O2 -o test_normal test_normal.c
gcc -Wall -O2 -o test_infinite_loop test_infinite_loop.c
gcc -Wall -O2 -o test_memory_hog test_memory_hog.c
gcc -Wall -O2 -o test_cpu_hog test_cpu_hog.c
```

## Usage

```bash
./sandbox <wall_seconds> <cpu_seconds> <mem_kb> <path_to_binary> [args...]
```

Pass `0` for any limit to disable checking it. See `BUILD.md` for worked
examples of each test scenario.

## Verified scenarios

- Normal exit: sandbox reports exit code, no violation.
- Wall-clock violation against a `SIGTERM`-resistant process: escalates to
  `SIGKILL`.
- Memory-limit violation: terminated via `SIGTERM` (this test process
  cooperates, so no escalation needed).
- CPU-time violation, distinct from wall-clock: proven by a process that
  bursts CPU work between sleeps, so wall-clock time and CPU time diverge
  significantly.
- Invalid binary path, non-executable file, and negative limits: all
  rejected before any process is forked (see `EDGE_CASES.md`).
- Two sandbox instances running concurrently: verified no cross-instance
  interference (see `EDGE_CASES.md`).

Logs for all of the above are in `logs_kali/`.
