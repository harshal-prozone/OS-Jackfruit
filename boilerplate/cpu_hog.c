/* cpu_hog.c - CPU-bound workload for scheduling experiments */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    printf("cpu_hog: burning CPU for %d seconds\n", duration);
    fflush(stdout);

    time_t start = time(NULL);
    volatile long long x = 0;

    while ((int)(time(NULL) - start) < duration) {
        for (int i = 0; i < 1000000; i++)
            x += i * i;

        static time_t last = 0;
        time_t now = time(NULL);
        if (now != last) {
            printf("cpu_hog: t=%lds x=%lld\n", (long)(now - start), x);
            fflush(stdout);
            last = now;
        }
    }

    printf("cpu_hog: done, x=%lld\n", x);
    return 0;
}
