/* io_pulse.c - I/O-bound workload for scheduling experiments */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define BUF_SIZE 4096

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    printf("io_pulse: running I/O workload for %d seconds\n", duration);
    fflush(stdout);

    time_t start = time(NULL);
    char   buf[BUF_SIZE];
    memset(buf, 'A', sizeof(buf));
    long long writes = 0;

    FILE *f = tmpfile();
    if (!f) { perror("tmpfile"); return 1; }

    while ((int)(time(NULL) - start) < duration) {
        fwrite(buf, 1, sizeof(buf), f);
        fflush(f);
        rewind(f);
        fread(buf, 1, sizeof(buf), f);
        rewind(f);
        writes++;

        static time_t last = 0;
        time_t now = time(NULL);
        if (now != last) {
            printf("io_pulse: t=%lds writes=%lld\n",
                   (long)(now - start), writes);
            fflush(stdout);
            last = now;
        }
    }

    fclose(f);
    printf("io_pulse: done, total writes=%lld\n", writes);
    return 0;
}
