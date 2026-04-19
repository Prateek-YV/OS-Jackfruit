/* memory_hog.c - allocates memory in steps to trigger soft/hard limits */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STEP_MIB   8
#define STEP_SLEEP 2

int main(int argc, char *argv[]) {
    int max_mib = 80;
    if (argc > 1) max_mib = atoi(argv[1]);

    printf("memory_hog: will allocate up to %d MiB in %d MiB steps\n",
           max_mib, STEP_MIB);
    fflush(stdout);

    char *blocks[256];
    int   nblocks = 0;
    int   allocated = 0;

    while (allocated < max_mib && nblocks < 256) {
        size_t sz = STEP_MIB * 1024 * 1024;
        char *p = malloc(sz);
        if (!p) { printf("memory_hog: malloc failed at %d MiB\n", allocated); break; }

        /* Touch every page so it's actually resident */
        memset(p, 0xAB, sz);
        blocks[nblocks++] = p;
        allocated += STEP_MIB;

        printf("memory_hog: allocated %d MiB so far\n", allocated);
        fflush(stdout);
        sleep(STEP_SLEEP);
    }

    printf("memory_hog: holding %d MiB, sleeping...\n", allocated);
    fflush(stdout);
    sleep(60);

    /* Free (may not be reached if killed by monitor) */
    for (int i = 0; i < nblocks; i++) free(blocks[i]);
    return 0;
}
