/* PS5LM bwprobe: is a big allocation slow memory?
 *
 * llama.cpp reads every weight once per token, so generation speed is memory
 * read bandwidth. Models over ~1 GB run 15 times slower than a 0.5 GB one on
 * the console. This allocates buffers of growing size two ways, with
 * posix_memalign (Sony's libc heap) and with an anonymous mmap, fills them and
 * measures read bandwidth with 1 and 3 threads. One buffer at a time, at most
 * 1.5 GB, three threads at most: safe next to a running ps5lm.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <immintrin.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define MiB ((size_t)1024 * 1024)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

struct job { const unsigned char *base; size_t len; uint64_t sink; };

static void *reader(void *arg) {
    struct job *j = arg;
    __m256i acc = _mm256_setzero_si256();
    const __m256i *v = (const __m256i *)j->base;
    const size_t n = j->len / sizeof(__m256i);
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < n; i += 4) {
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i));
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i + 1));
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i + 2));
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i + 3));
        }
    uint64_t l[4];
    _mm256_storeu_si256((__m256i *)l, acc);
    j->sink = l[0] + l[1] + l[2] + l[3];
    return NULL;
}

static double bandwidth(unsigned char *buf, size_t len, int threads) {
    pthread_t th[3];
    struct job jobs[3];
    const size_t slice = (len / threads) & ~(size_t)127;
    const double t0 = now_s();
    for (int i = 0; i < threads; i++) {
        jobs[i] = (struct job){buf + i * slice, slice, 0};
        pthread_create(&th[i], NULL, reader, &jobs[i]);
    }
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    return (double)slice * threads * 2 / (now_s() - t0) / 1e9;
}

static void measure(const char *how, unsigned char *buf, size_t len) {
    if (!buf) {
        printf("%-8s %5zu MiB: allocation failed\n", how, len / MiB);
        fflush(stdout);
        return;
    }
    memset(buf, 1, len);
    printf("%-8s %5zu MiB: %6.1f GB/s (1 thread) %6.1f GB/s (3 threads) at %p\n", how, len / MiB,
           bandwidth(buf, len, 1), bandwidth(buf, len, 3), (void *)buf);
    fflush(stdout);
}

int main(void) {
    static const size_t sizes[] = {64 * MiB, 256 * MiB, 512 * MiB, 1024 * MiB, 1536 * MiB};
    printf("PS5LM bwprobe\n");
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        const size_t len = sizes[k];
        void *p = NULL;
        if (posix_memalign(&p, 64, len) != 0)
            p = NULL;
        measure("malloc", p, len);
        free(p);

        void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        measure("mmap", m == MAP_FAILED ? NULL : m, len);
        if (m != MAP_FAILED)
            munmap(m, len);
    }
    printf("done\n");
    return 0;
}
