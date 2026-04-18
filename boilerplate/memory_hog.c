/* memory_hog.c - Memory consumer for soft/hard limit testing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int target_mib = (argc > 1) ? atoi(argv[1]) : 50;
    int step_mib   = (argc > 2) ? atoi(argv[2]) : 5;
    int sleep_sec  = (argc > 3) ? atoi(argv[3]) : 2;

    printf("memory_hog: allocating up to %d MiB in %d MiB steps\n",
           target_mib, step_mib);
    fflush(stdout);

    int    slots  = target_mib / step_mib + 2;
    char **chunks = calloc(slots, sizeof(char *));
    if (!chunks) { perror("calloc"); return 1; }
    int n = 0;

    for (int alloc = 0; alloc < target_mib; alloc += step_mib) {
        size_t sz = (size_t)step_mib * 1024 * 1024;
        char  *p  = malloc(sz);
        if (!p) {
            printf("memory_hog: malloc failed at %d MiB\n", alloc);
            break;
        }
        /* touch every page so RSS actually grows */
        memset(p, 0xAB, sz);
        chunks[n++] = p;
        printf("memory_hog: allocated %d MiB so far\n", alloc + step_mib);
        fflush(stdout);
        sleep(sleep_sec);
    }

    printf("memory_hog: holding allocation, sleeping 60s\n");
    fflush(stdout);
    sleep(60);

    for (int i = 0; i < n; i++) free(chunks[i]);
    free(chunks);
    return 0;
}
