/* io_pulse.c - I/O bound workload: repeatedly writes and reads a temp file */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define BLOCK_SIZE  4096
#define NUM_BLOCKS  256   /* 1 MiB per iteration */

int main(int argc, char *argv[]) {
    int duration = 30;
    if (argc > 1) duration = atoi(argv[1]);

    printf("io_pulse: running I/O workload for %d seconds\n", duration);
    fflush(stdout);

    char buf[BLOCK_SIZE];
    memset(buf, 0x5A, sizeof(buf));

    time_t start = time(NULL);
    long iterations = 0;

    while ((time(NULL) - start) < duration) {
        FILE *f = fopen("/tmp/io_pulse_tmp", "w");
        if (!f) { perror("fopen write"); break; }
        for (int i = 0; i < NUM_BLOCKS; i++) fwrite(buf, 1, sizeof(buf), f);
        fclose(f);

        f = fopen("/tmp/io_pulse_tmp", "r");
        if (!f) { perror("fopen read"); break; }
        while (fread(buf, 1, sizeof(buf), f) > 0) {}
        fclose(f);

        iterations++;
        if (iterations % 10 == 0) {
            printf("io_pulse: %ld iterations, elapsed=%lds\n",
                   iterations, (long)(time(NULL) - start));
            fflush(stdout);
        }
    }

    remove("/tmp/io_pulse_tmp");
    printf("io_pulse: done. %ld iterations\n", iterations);
    return 0;
}
