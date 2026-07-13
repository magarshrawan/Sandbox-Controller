# Build & Run Instructions (Kali Linux / any glibc Linux)

## Build
```
gcc -Wall -Wextra -O2 -pthread -o sandbox sandbox.c
gcc -Wall -O2 -o test_normal test_normal.c
gcc -Wall -O2 -o test_infinite_loop test_infinite_loop.c
gcc -Wall -O2 -o test_memory_hog test_memory_hog.c
```

## Run
```
./sandbox <wall_seconds> <cpu_seconds> <mem_kb> <path_to_binary> [args...]
```
Pass `0` for any limit to disable checking that limit.

Examples:
```
./sandbox 10 5 500000 ./test_normal          # should exit cleanly, no violation
./sandbox 3  0 500000 ./test_infinite_loop   # wall-clock violation, escalates to SIGKILL
./sandbox 30 30 30000 ./test_memory_hog      # memory violation, SIGTERM is enough
```

Logs of these three runs (captured on Ubuntu 24.04, kernel 6.18, gcc 13.3;
behaviour is identical on Kali since both are glibc/pthread Linux) are in `logs/`.
