# Edge Case Testing (Day 5)

## 1. Nonexistent binary path
Before this fix, an invalid path caused `execv()` to fail *inside the
forked child*, which then exited with code 127. The sandbox reported this
as "child exited normally, code=127" — technically true but misleading,
since a legitimate program could also happen to exit with code 127.

**Fix:** added a pre-flight check in `main()` using `access(target, F_OK)`
and `access(target, X_OK)` *before* forking at all. Invalid paths or
non-executable files are now rejected immediately with a clear error and
exit code 2, with no child process ever created.

Test:
```
./sandbox 10 5 500000 ./nonexistent_binary
# sandbox: target './nonexistent_binary' does not exist: No such file or directory
```

## 2. Non-executable file passed as target
```
touch not_executable.txt
./sandbox 10 5 500000 ./not_executable.txt
# sandbox: target './not_executable.txt' is not executable: Permission denied
```

## 3. Negative resource limits
Negative numbers for wall/cpu/mem limits are nonsensical (0 already means
"disabled"). Added validation to reject them before forking.
```
./sandbox -5 5 500000 ./test_normal
# sandbox: limits must be >= 0 (0 disables a limit)
```

## 4. Two sandbox instances running concurrently
Ran `sandbox` against `test_infinite_loop` (4s wall limit) and
`sandbox` against `test_memory_hog` (30000KB mem limit) at the same time,
as two separate OS processes. Confirmed via logs that:
  - Each instance tracked its own child PID/PGID independently.
  - Each enforced only its own configured limits.
  - Termination of one instance's child had no effect on the other.

This confirms the design is safe to run multiple sandboxes in parallel —
each `sandbox` process has its own private `g` state struct (no shared
memory between separate sandbox instances), so there is no cross-instance
race condition possible at the OS level.
