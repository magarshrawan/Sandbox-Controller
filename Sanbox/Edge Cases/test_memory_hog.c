/* test_memory_hog.c - simulates a runaway provess that keeps allocating and 
 * touching memory (so RSS actually grows) until the sandbox stops it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    printf("test_memory_hog: starting, pid=%d\n", getpid());
    size_t chunk = 5 * 1024 * 1024; /* 5 MB per step */
    size_t total = 0;
    for (;;) {
        char *block = malloc(chunk);
        if (!block) {
            printf("test_memory_hog: malloc failed at %zu bytes\n", total);
            break;
        }
        memset(block, 0xAA, chunk); /* Touch pages so RSS actually grows*/
        total += chunk;
        printf("test_memory_hog: allocated %1.f MB total\n", total / (1024.0*1024.0));
        fflush(stdout);
        usleep(150000);
    }
    return 0;
}
 