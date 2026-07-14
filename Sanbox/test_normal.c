/* test_normal.c - a well-behaved program: does a little work then exits. */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("test_normal: starting, pid=%d\n", getpid());
    for (int i = 0; i < 3; i++) {
        printf("test_normal: working ... %d\n", i);
        sleep(1);
    }
    printf("test_normal: done, exiting cleanly.\n");
    return 0;
}