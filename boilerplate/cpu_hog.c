/* cpu_hog.c - burns CPU in a tight loop for scheduling experiments */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[]) {
    int duration = 30; /* seconds */
    if (argc > 1) duration = atoi(argv[1]);

    printf("cpu_hog: running for %d seconds\n", duration);
    fflush(stdout);

    time_t start = time(NULL);
    volatile long counter = 0;
    while ((time(NULL) - start) < duration) {
        counter++;
        if (counter % 100000000L == 0) {
            printf("cpu_hog: iterations=%ld elapsed=%lds\n",
                   counter, (long)(time(NULL) - start));
            fflush(stdout);
        }
    }
    printf("cpu_hog: done. total iterations=%ld\n", counter);
    return 0;
}
