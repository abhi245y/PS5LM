/* PS5LM memprobe: the Phase 0 probe.
 *
 * Measures what a payload process on a jailbroken PS5 can use for LLM
 * inference: the console and CPU, the direct, flexible and malloc memory
 * ceilings, the largest single allocation (llama.cpp's CPU backend puts all
 * weights in one), memory bandwidth on malloc and on direct memory, AVX2 FMA
 * throughput, and storage read and mmap speed on a GGUF file if one is present.
 *
 * Every line goes to stdout (elfldr streams it back to scripts/send.sh) and to
 * /data/PS5LM/memprobe.txt, synced line by line, so a probe the system kills
 * still leaves its last good step on disk.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cpuid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define MiB (1024ULL * 1024ULL)
#define GiB (1024ULL * MiB)

#define OUT_DIR   "/data/PS5LM"
#define OUT_FILE  OUT_DIR "/memprobe.txt"
#define MODEL_DIR OUT_DIR "/models"

#define CHUNK      (256 * MiB)
#define MAX_CHUNKS 64 /* 16 GiB: more than the console has */
#define TOUCH_STEP 4096

/* libkernel, not declared by the SDK's headers. */
int    sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len,
                                     size_t alignment, int memory_type, off_t *phys_out);
int    sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                                off_t direct_start, size_t alignment);
int    sceKernelReleaseDirectMemory(off_t start, size_t len);
int    sceKernelMunmap(void *addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int    sceKernelAvailableDirectMemorySize(off_t search_start, off_t search_end, size_t alignment,
                                         off_t *phys_out, size_t *size_out);
int    sceKernelConfiguredFlexibleMemorySize(size_t *size_out);
int    sceKernelAvailableFlexibleMemorySize(size_t *size_out);
int    sceKernelGetHwModelName(char *name);
long   sceKernelGetCpuFrequency(void);
int    sceKernelGetCpuTemperature(int *celsius);

typedef struct {
    char unused[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

#define PROT_CPU_RW 0x03
/* PS5SX2's direct memory type (pcsx2/Memory.cpp and the Vulkan driver); 0 as a fallback. */
static const int k_mem_types[] = {12, 0};

static FILE *g_log;

static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
        fsync(fileno(g_log));
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double gib(unsigned long long bytes) { return (double)bytes / (double)GiB; }

static void touch(void *p, size_t len) {
    volatile unsigned char *b = p;
    for (size_t i = 0; i < len; i += TOUCH_STEP)
        b[i] = (unsigned char)i;
}

/* Results that go into the closing notification. */
static struct {
    unsigned long long direct_max;
    unsigned long long malloc_max;
    unsigned long long single_max;
    double bw_best;
    double gflops_best;
} g_sum;

/* ---- console and CPU --------------------------------------------------- */

static void probe_system(int *ncpu_out) {
    char model[256] = {0};
    int temp = 0;

    out("== system\n");
    out("firmware      0x%08x\n", kernel_get_fw_version());
    if (sceKernelGetHwModelName(model) == 0)
        out("model         %s\n", model);
    out("cpu freq      %ld MHz\n", sceKernelGetCpuFrequency() / 1000000);
    if (sceKernelGetCpuTemperature(&temp) == 0)
        out("cpu temp      %d C\n", temp);
    out("pid/uid       %d/%d\n", (int)getpid(), (int)getuid());

    unsigned a, b, c, d;
    char vendor[13] = {0};
    char brand[49] = {0};
    __get_cpuid(0, &a, &b, &c, &d);
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    for (unsigned leaf = 0; leaf < 3; leaf++) {
        __get_cpuid(0x80000002 + leaf, &a, &b, &c, &d);
        memcpy(brand + leaf * 16 + 0, &a, 4);
        memcpy(brand + leaf * 16 + 4, &b, 4);
        memcpy(brand + leaf * 16 + 8, &c, 4);
        memcpy(brand + leaf * 16 + 12, &d, 4);
    }
    out("cpu           %s (%s)\n", brand, vendor);

    __get_cpuid(1, &a, &b, &c, &d);
    const int sse42 = (c >> 20) & 1, avx = (c >> 28) & 1, fma = (c >> 12) & 1, f16c = (c >> 29) & 1;
    __get_cpuid_count(7, 0, &a, &b, &c, &d);
    const int avx2 = (b >> 5) & 1, bmi2 = (b >> 8) & 1, avx512f = (b >> 16) & 1;
    out("features      sse4.2=%d avx=%d avx2=%d fma=%d f16c=%d bmi2=%d avx512f=%d\n",
        sse42, avx, avx2, fma, f16c, bmi2, avx512f);

    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    cpuset_t mask;
    int allowed = -1;
    CPU_ZERO(&mask);
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(mask), &mask) == 0) {
        allowed = 0;
        for (int i = 0; i < CPU_SETSIZE; i++)
            allowed += CPU_ISSET(i, &mask) ? 1 : 0;
    }
    out("cpus          online=%ld allowed=%d\n", online, allowed);
    *ncpu_out = allowed > 0 ? allowed : (online > 0 ? (int)online : 1);
}

/* ---- memory pools ------------------------------------------------------ */

static void probe_pools(void) {
    size_t flex_cfg = 0, flex_avail = 0, avail = 0;
    off_t phys = 0;

    out("== memory pools\n");
    const size_t direct_total = sceKernelGetDirectMemorySize();
    out("direct total  %.2f GiB\n", gib(direct_total));
    int rc = sceKernelAvailableDirectMemorySize(0, (off_t)direct_total, 0, &phys, &avail);
    out("direct avail  %.2f GiB largest free block (rc=0x%x)\n", gib(avail), rc);
    rc = sceKernelConfiguredFlexibleMemorySize(&flex_cfg);
    out("flexible cfg  %.2f GiB (rc=0x%x)\n", gib(flex_cfg), rc);
    rc = sceKernelAvailableFlexibleMemorySize(&flex_avail);
    out("flexible free %.2f GiB (rc=0x%x)\n", gib(flex_avail), rc);
}

/* Allocates and maps direct memory in CHUNK steps until the kernel says no. */
static void probe_direct(void) {
    static off_t phys[MAX_CHUNKS];
    static void *addr[MAX_CHUNKS];
    const size_t direct_total = sceKernelGetDirectMemorySize();
    int n = 0, type = -1;

    out("== direct memory ladder (%llu MiB steps)\n", CHUNK / MiB);
    for (; n < MAX_CHUNKS; n++) {
        int rc = -1;
        if (type < 0) {
            for (size_t t = 0; t < sizeof(k_mem_types) / sizeof(k_mem_types[0]); t++) {
                rc = sceKernelAllocateDirectMemory(0, (off_t)direct_total, CHUNK, 2 * MiB,
                                                   k_mem_types[t], &phys[n]);
                out("  type %d: allocate rc=0x%x\n", k_mem_types[t], rc);
                if (rc == 0) {
                    type = k_mem_types[t];
                    break;
                }
            }
        } else {
            rc = sceKernelAllocateDirectMemory(0, (off_t)direct_total, CHUNK, 2 * MiB, type, &phys[n]);
        }
        if (rc != 0) {
            out("  stop at %.2f GiB: allocate rc=0x%x\n", gib((unsigned long long)n * CHUNK), rc);
            break;
        }
        addr[n] = NULL;
        rc = sceKernelMapDirectMemory(&addr[n], CHUNK, PROT_CPU_RW, 0, phys[n], 2 * MiB);
        if (rc != 0) {
            out("  stop at %.2f GiB: map rc=0x%x\n", gib((unsigned long long)n * CHUNK), rc);
            sceKernelReleaseDirectMemory(phys[n], CHUNK);
            break;
        }
        touch(addr[n], CHUNK);
        if ((n + 1) % 4 == 0)
            out("  %.2f GiB mapped and touched\n", gib((unsigned long long)(n + 1) * CHUNK));
    }
    g_sum.direct_max = (unsigned long long)n * CHUNK;
    out("direct max    %.2f GiB (type %d)\n", gib(g_sum.direct_max), type);

    for (int i = 0; i < n; i++) {
        sceKernelMunmap(addr[i], CHUNK);
        sceKernelReleaseDirectMemory(phys[i], CHUNK);
    }
}

/* malloc in CHUNK steps, every page touched, then the largest single block. */
static void probe_malloc(void) {
    static void *chunk[MAX_CHUNKS];
    int n = 0;

    out("== malloc ladder (%llu MiB steps, pages touched)\n", CHUNK / MiB);
    for (; n < MAX_CHUNKS; n++) {
        chunk[n] = malloc(CHUNK);
        if (!chunk[n]) {
            out("  stop at %.2f GiB: malloc errno=%d\n", gib((unsigned long long)n * CHUNK), errno);
            break;
        }
        touch(chunk[n], CHUNK);
        if ((n + 1) % 4 == 0)
            out("  %.2f GiB allocated and touched\n", gib((unsigned long long)(n + 1) * CHUNK));
    }
    g_sum.malloc_max = (unsigned long long)n * CHUNK;
    out("malloc max    %.2f GiB\n", gib(g_sum.malloc_max));
    for (int i = 0; i < n; i++)
        free(chunk[i]);

    out("== largest single allocation (posix_memalign, 64-byte aligned)\n");
    for (unsigned long long size = g_sum.malloc_max; size >= CHUNK; size -= CHUNK) {
        void *p = NULL;
        if (posix_memalign(&p, 64, size) == 0 && p) {
            touch(p, size);
            g_sum.single_max = size;
            free(p);
            break;
        }
    }
    out("single max    %.2f GiB\n", gib(g_sum.single_max));
}

/* ---- bandwidth and compute --------------------------------------------- */

struct bw_job {
    const unsigned char *base;
    size_t len;
    int passes;
    uint64_t sink;
};

static void *bw_worker(void *arg) {
    struct bw_job *job = arg;
    __m256i acc = _mm256_setzero_si256();
    for (int p = 0; p < job->passes; p++) {
        const __m256i *v = (const __m256i *)job->base;
        const size_t count = job->len / sizeof(__m256i);
        for (size_t i = 0; i < count; i += 4) {
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i));
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i + 1));
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i + 2));
            acc = _mm256_add_epi64(acc, _mm256_load_si256(v + i + 3));
        }
    }
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i *)lanes, acc);
    job->sink = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    return NULL;
}

/* Best read bandwidth over a buffer with 1, 2, 4 and ncpu threads. */
static double read_bandwidth(const char *label, unsigned char *buf, size_t len, int ncpu) {
    const int counts[] = {1, 2, 4, ncpu};
    const int passes = 4;
    double best = 0;

    for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]); k++) {
        const int t = counts[k];
        if (t < 1 || (k > 0 && t <= counts[k - 1]))
            continue;
        pthread_t th[64];
        struct bw_job job[64];
        const size_t slice = (len / (size_t)t) & ~(size_t)127;
        const double t0 = now_s();
        for (int i = 0; i < t && i < 64; i++) {
            job[i] = (struct bw_job){buf + (size_t)i * slice, slice, passes, 0};
            pthread_create(&th[i], NULL, bw_worker, &job[i]);
        }
        for (int i = 0; i < t && i < 64; i++)
            pthread_join(th[i], NULL);
        const double dt = now_s() - t0;
        const double gbs = (double)slice * (double)t * passes / dt / 1e9;
        out("%-13s %2d threads: %6.1f GB/s\n", label, t, gbs);
        if (gbs > best)
            best = gbs;
    }
    return best;
}

static void probe_bandwidth(int ncpu) {
    out("== memory read bandwidth\n");
    size_t len = 1 * GiB;
    void *buf = NULL;
    while (len >= 128 * MiB && posix_memalign(&buf, 4096, len) != 0)
        len /= 2;
    if (buf) {
        memset(buf, 1, len);
        g_sum.bw_best = read_bandwidth("malloc", buf, len, ncpu);
        free(buf);
    } else {
        out("malloc        no buffer\n");
    }

    /* Direct memory may be mapped with other cache attributes than malloc's
     * pages: it matters once the GPU reads the same weights. */
    const size_t direct_total = sceKernelGetDirectMemorySize();
    for (size_t t = 0; t < sizeof(k_mem_types) / sizeof(k_mem_types[0]); t++) {
        off_t phys = 0;
        void *addr = NULL;
        if (sceKernelAllocateDirectMemory(0, (off_t)direct_total, 1 * GiB, 2 * MiB, k_mem_types[t], &phys) != 0)
            continue;
        if (sceKernelMapDirectMemory(&addr, 1 * GiB, PROT_CPU_RW, 0, phys, 2 * MiB) == 0) {
            char label[32];
            snprintf(label, sizeof(label), "direct t%d", k_mem_types[t]);
            memset(addr, 1, 1 * GiB);
            const double gbs = read_bandwidth(label, addr, 1 * GiB, ncpu);
            if (gbs > g_sum.bw_best)
                g_sum.bw_best = gbs;
            sceKernelMunmap(addr, 1 * GiB);
        }
        sceKernelReleaseDirectMemory(phys, 1 * GiB);
    }
}

struct fma_job {
    long iters;
    float sink;
};

/* 12 independent FMA chains: enough to cover Zen 2's two FMA pipes at 5-cycle latency. */
static void *fma_worker(void *arg) {
    struct fma_job *job = arg;
    const __m256 m = _mm256_set1_ps(0.9999999f), c = _mm256_set1_ps(1e-7f);
    __m256 r[12];
    for (int i = 0; i < 12; i++)
        r[i] = _mm256_set1_ps((float)i);
    for (long it = 0; it < job->iters; it++)
        for (int i = 0; i < 12; i++)
            r[i] = _mm256_fmadd_ps(r[i], m, c);
    float lanes[8];
    __m256 acc = r[0];
    for (int i = 1; i < 12; i++)
        acc = _mm256_add_ps(acc, r[i]);
    _mm256_storeu_ps(lanes, acc);
    job->sink = lanes[0];
    return NULL;
}

static void probe_compute(int ncpu) {
    out("== AVX2 FMA throughput (fp32)\n");
    const int counts[] = {1, ncpu};
    const long iters = 50 * 1000 * 1000;
    for (size_t k = 0; k < 2; k++) {
        const int t = counts[k];
        if (k > 0 && t <= counts[0])
            continue;
        pthread_t th[64];
        struct fma_job job[64];
        const double t0 = now_s();
        for (int i = 0; i < t && i < 64; i++) {
            job[i] = (struct fma_job){iters, 0};
            pthread_create(&th[i], NULL, fma_worker, &job[i]);
        }
        for (int i = 0; i < t && i < 64; i++)
            pthread_join(th[i], NULL);
        const double dt = now_s() - t0;
        const double gflops = (double)iters * 12 * 8 * 2 * t / dt / 1e9;
        out("fma           %2d threads: %7.1f GFLOPS\n", t, gflops);
        if (gflops > g_sum.gflops_best)
            g_sum.gflops_best = gflops;
    }
}

/* ---- storage ------------------------------------------------------------ */

static void probe_storage(void) {
    static const char *mounts[] = {"/data", "/mnt/usb0", "/mnt/usb1", "/mnt/ext0"};

    out("== storage\n");
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        struct statfs fs;
        if (statfs(mounts[i], &fs) != 0)
            continue;
        out("%-13s %s, %.1f GiB free of %.1f GiB\n", mounts[i], fs.f_fstypename,
            gib((unsigned long long)fs.f_bavail * fs.f_bsize),
            gib((unsigned long long)fs.f_blocks * fs.f_bsize));
    }

    char path[512] = {0};
    DIR *dir = opendir(MODEL_DIR);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            const size_t n = strlen(e->d_name);
            if (n > 5 && strcmp(e->d_name + n - 5, ".gguf") == 0) {
                snprintf(path, sizeof(path), "%s/%s", MODEL_DIR, e->d_name);
                break;
            }
        }
        closedir(dir);
    }
    if (!path[0]) {
        out("no .gguf in %s: copy one there to measure model load speed\n", MODEL_DIR);
        return;
    }

    struct stat st;
    const int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0) {
        out("%s: open errno=%d\n", path, errno);
        if (fd >= 0)
            close(fd);
        return;
    }
    out("model file    %s, %.2f GiB\n", path, gib((unsigned long long)st.st_size));

    const size_t bufsize = 16 * MiB;
    unsigned char *buf = malloc(bufsize);
    if (buf) {
        const unsigned long long want = (unsigned long long)st.st_size < 2 * GiB ? (unsigned long long)st.st_size : 2 * GiB;
        unsigned long long got = 0;
        const double t0 = now_s();
        while (got < want) {
            const ssize_t r = read(fd, buf, bufsize);
            if (r <= 0)
                break;
            got += (unsigned long long)r;
        }
        const double dt = now_s() - t0;
        out("read          %.2f GiB in %.2f s: %.0f MB/s\n", gib(got), dt, (double)got / dt / 1e6);
        free(buf);
    }

    /* llama.cpp maps the model file by default: does that work here? */
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        out("file mmap     failed, errno=%d (llama.cpp needs --no-mmap here)\n", errno);
    } else {
        const size_t span = (size_t)st.st_size < 1 * GiB ? (size_t)st.st_size : 1 * GiB;
        volatile const unsigned char *p = map;
        unsigned sum = 0;
        const double t0 = now_s();
        for (size_t i = 0; i < span; i += TOUCH_STEP)
            sum += p[i];
        const double dt = now_s() - t0;
        out("file mmap     ok, first %.2f GiB faulted in %.2f s: %.0f MB/s (sum %u)\n", gib(span), dt,
            (double)span / dt / 1e6, sum);
        munmap(map, (size_t)st.st_size);
    }
    close(fd);
}

/* ---- main ---------------------------------------------------------------- */

int main(void) {
    mkdir(OUT_DIR, 0777);
    mkdir(MODEL_DIR, 0777);
    g_log = fopen(OUT_FILE, "w");

    out("PS5LM memprobe\n");
    int ncpu = 1;
    probe_system(&ncpu);
    probe_pools();
    probe_direct();
    probe_malloc();
    probe_bandwidth(ncpu);
    probe_compute(ncpu);
    probe_storage();

    out("== summary\n");
    out("direct %.2f GiB, malloc %.2f GiB, single block %.2f GiB, %.1f GB/s, %.0f GFLOPS\n",
        gib(g_sum.direct_max), gib(g_sum.malloc_max), gib(g_sum.single_max), g_sum.bw_best,
        g_sum.gflops_best);
    out("log: %s\n", g_log ? OUT_FILE : "(not written)");

    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message),
             "PS5LM memprobe\ndirect %.1f GiB, malloc %.1f GiB\n%.0f GB/s, %.0f GFLOPS",
             gib(g_sum.direct_max), gib(g_sum.malloc_max), g_sum.bw_best, g_sum.gflops_best);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

    if (g_log)
        fclose(g_log);
    return 0;
}
