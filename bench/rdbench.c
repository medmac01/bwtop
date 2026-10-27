// rdbench: multithreaded streaming read (sum) over large buffers; reports read GB/s.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define SZ (128ul << 20)
static int secs = 5;
static double bytes[16];
static volatile uint64_t sink;
static void *run(void *a) {
    long id = (long)a;
    uint64_t *x = malloc(SZ); memset(x, 1, SZ);
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (size_t i = 0; i < SZ / 8; i += 4) { s0 += x[i]; s1 += x[i+1]; s2 += x[i+2]; s3 += x[i+3]; }
        sink += s0 + s1 + s2 + s3; bytes[id] += SZ; clock_gettime(CLOCK_MONOTONIC, &t);
    } while (t.tv_sec - t0.tv_sec < secs);
    return NULL;
}
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 8;
    pthread_t th[16];
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < n; i++) pthread_create(&th[i], NULL, run, (void *)i);
    double tot = 0;
    for (int i = 0; i < n; i++) { pthread_join(th[i], NULL); tot += bytes[i]; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("rdbench: %.1f GB/s (read, %d threads)\n", tot / dt / 1e9, n);
}
