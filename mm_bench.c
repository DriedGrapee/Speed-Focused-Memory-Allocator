/*
 * mm_bench.c
 *
 * Replays CS:APP-format traces against every allocator in allocators[] and reports
 * throughput, utilization and internal fragmentation with confidence intervals.
 * README.md (Benchmarking) describes the trace format and the method.
 */
#define _GNU_SOURCE  // sched_setaffinity

#include "mm_alloc.h"
#include "mm_alloc_internal.h"

#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---- harness memory ----

// The harness's own buffers come from mmap: malloc would put them in the glibc
// heap being measured.
static void* mmap_zeroed(size_t bytes) {
    void* p = mmap(nullptr, bytes ? bytes : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void mmap_release(void* p, size_t bytes) {
    if (p)
        munmap(p, bytes ? bytes : 1);
}

// ---- allocators ----

typedef struct allocator allocator;
struct allocator {
    const char* name;
    void (*init)(void);
    void* (*malloc)(size_t size);
    void (*free)(void* ptr);
    void* (*realloc)(void* ptr, size_t size);  // nullptr: replayed as malloc + copy + free
    size_t (*usable_size)(void* ptr);          // counted the way malloc_usable_size counts
    size_t (*heap_size)(void);                 // growth since init
};

// mm grows only through sbrk, and nothing else in the child moves the break.
// Usable size is the block minus its header: the footer slot is payload while allocated.
static uintptr_t mm_heap_base;
static void mm_init(void) { mm_heap_base = (uintptr_t)sbrk(0); }
static size_t mm_usable_size(void* ptr) { return calculate_size(block_of(ptr)) - offsetof(free_list, body); }
static size_t mm_heap_size(void) { return (uintptr_t)sbrk(0) - mm_heap_base; }

// The child inherits the parent's glibc arena, so subtract what the parent still
// uses; its free slack stands in for the padding a fresh process's first sbrk takes.
static size_t glibc_heap_base;
static void glibc_init(void) {
    struct mallinfo2 info = mallinfo2();
    glibc_heap_base = info.uordblks + info.hblkhd;
}
static size_t glibc_usable_size(void* ptr) { return malloc_usable_size(ptr); }
static size_t glibc_heap_size(void) {
    struct mallinfo2 info = mallinfo2();
    size_t now = info.arena + info.hblkhd;
    return now > glibc_heap_base ? now - glibc_heap_base : 0;
}

static const allocator allocators[] = {
    {
        .name        = "mm",
        .init        = mm_init,
        .malloc      = mm_malloc,
        .free        = mm_free,
        .realloc     = nullptr, // TODO: mm_realloc once it is implemented
        .usable_size = mm_usable_size,
        .heap_size   = mm_heap_size,
    },
    {
        .name        = "glibc",
        .init        = glibc_init,
        .malloc      = malloc,
        .free        = free,
        .realloc     = realloc,
        .usable_size = glibc_usable_size,
        .heap_size   = glibc_heap_size,
    },
};
#define N_ALLOCATORS (sizeof allocators / sizeof allocators[0])

// ---- traces ----

typedef enum { OP_ALLOC, OP_FREE, OP_REALLOC } op_kind;

typedef struct {
    op_kind kind;
    size_t id;
    size_t size;
} op;

typedef struct {
    const char* name;
    op* ops;
    size_t n_ops;
    size_t cap;
    size_t n_ids;
} trace;

static void trace_push(trace* t, op_kind kind, size_t id, size_t size) {
    if (t->n_ops == t->cap) {
        size_t cap = t->cap ? 2 * t->cap : 4096;
        op* ops = mmap_zeroed(cap * sizeof *ops);
        if (t->ops)
            memcpy(ops, t->ops, t->n_ops * sizeof *ops);
        mmap_release(t->ops, t->cap * sizeof *t->ops);
        t->ops = ops;
        t->cap = cap;
    }
    t->ops[t->n_ops++] = (op){ .kind = kind, .id = id, .size = size };
    if (id >= t->n_ids)
        t->n_ids = id + 1;
}

// Builders for workload(). An id names one live allocation and can be reused once freed.
static void trace_malloc(trace* t, size_t id, size_t size)  { trace_push(t, OP_ALLOC, id, size); }
static void trace_free(trace* t, size_t id)                 { trace_push(t, OP_FREE, id, 0); }
static void trace_realloc(trace* t, size_t id, size_t size) { trace_push(t, OP_REALLOC, id, size); }

// Synthetic workload, replayed when no trace files are given. Record calls with the
// builders above; every allocator then replays the same sequence, keeping random
// number generation out of the timing. Seed from `seed` (-s) to be reproducible.
static void workload(trace* t, unsigned seed) {
    // TODO
    (void)t; (void)seed; (void)trace_malloc; (void)trace_free; (void)trace_realloc;
}

static bool load_trace(trace* t, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        perror(path);
        return false;
    }

    char* line = nullptr;
    size_t line_cap = 0, line_no = 0;
    bool ok = true;

    while (getline(&line, &line_cap, f) != -1) {
        line_no++;
        char kind;
        size_t id, size;
        char* p = line + strspn(line, " \t");

        // blank, comment, or a number from the CS:APP header
        if (*p == '\0' || *p == '\n' || *p == '#' || (*p >= '0' && *p <= '9'))
            continue;

        int fields = sscanf(p, "%c %zu %zu", &kind, &id, &size);
        if (kind == 'a' && fields == 3)
            trace_malloc(t, id, size);
        else if (kind == 'r' && fields == 3)
            trace_realloc(t, id, size);
        else if (kind == 'f' && fields >= 2)
            trace_free(t, id);
        else {
            fprintf(stderr, "%s:%zu: cannot parse '%.*s'\n", path, line_no, (int)strcspn(p, "\n"), p);
            ok = false;
            break;
        }
    }

    free(line);
    fclose(f);
    return ok;
}

// Rejects traces whose allocs and frees do not pair up, so a bad trace is not
// reported as an allocator crash.
static bool check_trace(const trace* t) {
    bool* live = mmap_zeroed(t->n_ids * sizeof *live);
    bool ok = true;

    for (size_t i = 0; i < t->n_ops && ok; i++) {
        const op* o = &t->ops[i];
        switch (o->kind) {
        case OP_ALLOC:
            if (live[o->id]) {
                fprintf(stderr, "%s: op %zu allocates id %zu, which is already live\n", t->name, i, o->id);
                ok = false;
            }
            live[o->id] = true;
            break;
        case OP_FREE:
            if (!live[o->id]) {
                fprintf(stderr, "%s: op %zu frees id %zu, which is not live\n", t->name, i, o->id);
                ok = false;
            }
            live[o->id] = false;
            break;
        case OP_REALLOC: // of a dead id: realloc(NULL, size), i.e. malloc
            live[o->id] = true;
            break;
        }
    }

    mmap_release(live, t->n_ids * sizeof *live);
    return ok;
}

// ---- replay ----

typedef enum { MODE_THROUGHPUT, MODE_UTILIZATION } bench_mode;

typedef struct {
    bool ok;
    char error[96];
    double seconds;
    size_t peak_live;     // requested bytes
    size_t peak_heap;
    size_t frag_at_peak;  // internal fragmentation when peak_live was reached
} result;

static void* do_realloc(const allocator* a, void* ptr, size_t old_size, size_t size) {
    if (a->realloc)
        return a->realloc(ptr, size);

    void* fresh = a->malloc(size);
    if (fresh && ptr) {
        memcpy(fresh, ptr, old_size < size ? old_size : size);
        a->free(ptr);
    }
    return fresh;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Timed: the calls and the pointer table, nothing else.
static result replay_timed(const allocator* a, const trace* t, void** ptrs, size_t* sizes) {
    result r = { .ok = true };
    double start = now_seconds();

    for (size_t i = 0; i < t->n_ops; i++) {
        const op* o = &t->ops[i];
        switch (o->kind) {
        case OP_ALLOC:
            ptrs[o->id] = a->malloc(o->size);
            sizes[o->id] = o->size;
            break;
        case OP_FREE:
            a->free(ptrs[o->id]);
            ptrs[o->id] = nullptr;
            break;
        case OP_REALLOC:
            ptrs[o->id] = do_realloc(a, ptrs[o->id], sizes[o->id], o->size);
            sizes[o->id] = o->size;
            break;
        }
        if (!ptrs[o->id] && o->kind != OP_FREE && o->size) {
            r.ok = false;
            snprintf(r.error, sizeof r.error, "op %zu: out of memory", i);
            return r;
        }
    }

    r.seconds = now_seconds() - start;
    return r;
}

// Untimed: tracks live bytes, fragmentation and heap growth after every op.
static result replay_measured(const allocator* a, const trace* t, void** ptrs, size_t* sizes) {
    result r = { .ok = true };
    size_t live = 0, frag = 0;

    for (size_t i = 0; i < t->n_ops; i++) {
        const op* o = &t->ops[i];
        void* old = ptrs[o->id];

        if (o->kind != OP_ALLOC && old) {
            live -= sizes[o->id];
            frag -= a->usable_size(old) - sizes[o->id];
        }

        switch (o->kind) {
        case OP_ALLOC:
            ptrs[o->id] = a->malloc(o->size);
            break;
        case OP_FREE:
            a->free(old);
            ptrs[o->id] = nullptr;
            break;
        case OP_REALLOC:
            ptrs[o->id] = do_realloc(a, old, sizes[o->id], o->size);
            break;
        }

        if (o->kind != OP_FREE) {
            void* p = ptrs[o->id];
            if (!p && o->size) {
                r.ok = false;
                snprintf(r.error, sizeof r.error, "op %zu: out of memory", i);
                return r;
            }
            if ((uintptr_t)p % ALIGNMENT) {
                r.ok = false;
                snprintf(r.error, sizeof r.error, "op %zu: %p is not %d-byte aligned", i, p, ALIGNMENT);
                return r;
            }
            sizes[o->id] = o->size;
            if (p) {
                live += o->size;
                frag += a->usable_size(p) - o->size;
            }
        }

        if (live > r.peak_live) {
            r.peak_live = live;
            r.frag_at_peak = frag;
        }
        size_t heap = a->heap_size();
        if (heap > r.peak_heap)
            r.peak_heap = heap;
    }

    return r;
}

// Runs one replay in a forked child: the only way to give mm a fresh heap, and a
// crash becomes a FAILED row instead of ending the benchmark.
static result run_child(const allocator* a, const trace* t, bench_mode mode) {
    result r = { .ok = false };
    int fds[2];

    if (pipe(fds) == -1) {
        snprintf(r.error, sizeof r.error, "pipe: %s", strerror(errno));
        return r;
    }

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == -1) {
        snprintf(r.error, sizeof r.error, "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return r;
    }

    if (pid == 0) {
        close(fds[0]);
        void** ptrs = mmap_zeroed(t->n_ids * sizeof *ptrs);
        size_t* sizes = mmap_zeroed(t->n_ids * sizeof *sizes);

        a->init();
        result out = mode == MODE_THROUGHPUT ? replay_timed(a, t, ptrs, sizes)
                                             : replay_measured(a, t, ptrs, sizes);
        ssize_t written = write(fds[1], &out, sizeof out);
        _exit(written == sizeof out ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    close(fds[1]);
    ssize_t got = read(fds[0], &r, sizeof r);
    close(fds[0]);

    int status;
    waitpid(pid, &status, 0);
    if (got != sizeof r) {
        r = (result){ .ok = false };
        if (WIFSIGNALED(status))
            snprintf(r.error, sizeof r.error, "crashed: %s", strsignal(WTERMSIG(status)));
        else
            snprintf(r.error, sizeof r.error, "exited without a result");
    }
    return r;
}

// ---- statistics ----

// splitmix64 with a fixed seed, so run order and bootstrap draws are reproducible
static uint64_t rng_state = 0x9e3779b97f4a7c15;

static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

static size_t rng_below(size_t n) { return (size_t)(rng_next() % n); }

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

// sorts xs in place
static double median(double* xs, size_t n) {
    qsort(xs, n, sizeof *xs, cmp_double);
    return n % 2 ? xs[n / 2] : (xs[n / 2 - 1] + xs[n / 2]) / 2;
}

// Distribution-free 95% CI for the median: order statistics x(j) and x(n+1-j), with
// P(Binomial(n, 1/2) < j) <= 2.5%. Assumes nothing about the (skewed) timing
// distribution. xs must be sorted; false when n < 6.
static bool median_ci(const double* xs, size_t n, double* lo, double* hi) {
    double p = pow(0.5, (double)n), cdf = 0;
    size_t j = 0;
    for (size_t k = 0; k < n; k++) {
        if (cdf + p > 0.025)
            break;
        cdf += p;
        j = k + 1;
        p *= (double)(n - k) / (double)(k + 1);
    }
    if (j == 0)
        return false;
    *lo = xs[j - 1];
    *hi = xs[n - j];
    return true;
}

// Bootstrap 95% CI for median(base) / median(other), resampling both each round.
#define BOOTSTRAP_ROUNDS 2000
static void speedup_ci(const double* base, const double* other, size_t n, double* lo, double* hi) {
    double* ratios = mmap_zeroed(BOOTSTRAP_ROUNDS * sizeof *ratios);
    double* a = mmap_zeroed(n * sizeof *a);
    double* b = mmap_zeroed(n * sizeof *b);

    for (size_t r = 0; r < BOOTSTRAP_ROUNDS; r++) {
        for (size_t i = 0; i < n; i++) {
            a[i] = base[rng_below(n)];
            b[i] = other[rng_below(n)];
        }
        ratios[r] = median(a, n) / median(b, n);
    }
    qsort(ratios, BOOTSTRAP_ROUNDS, sizeof *ratios, cmp_double);
    *lo = ratios[(size_t)(0.025 * BOOTSTRAP_ROUNDS)];
    *hi = ratios[(size_t)(0.975 * BOOTSTRAP_ROUNDS) - 1];

    mmap_release(ratios, BOOTSTRAP_ROUNDS * sizeof *ratios);
    mmap_release(a, n * sizeof *a);
    mmap_release(b, n * sizeof *b);
}

// 97.5% quantile of Student's t with df degrees of freedom
static double t_975(size_t df) {
    static const double table[] = {
        0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
        2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
    };
    if (df < sizeof table / sizeof table[0])
        return table[df];
    return df < 40 ? 2.021 : df < 60 ? 2.000 : df < 120 ? 1.980 : 1.960;
}

// mean, plus the half-width of its 95% t-interval (NAN when n < 2)
static double mean_ci(const double* xs, size_t n, double* half_width) {
    double sum = 0, sq = 0;
    for (size_t i = 0; i < n; i++)
        sum += xs[i];
    double mean = sum / (double)n;
    for (size_t i = 0; i < n; i++)
        sq += (xs[i] - mean) * (xs[i] - mean);
    *half_width = n < 2 ? NAN : t_975(n - 1) * sqrt(sq / (double)(n - 1) / (double)n);
    return mean;
}

// ---- driver ----

typedef struct {
    bool ok;
    double kops, kops_lo, kops_hi;
    bool has_ci;
    double speedup, speedup_lo, speedup_hi;  // baseline time / time: > 1 is faster
    double util, frag;                       // percent
} cell;

typedef struct {
    char family[64];
    cell cells[N_ALLOCATORS];
} trace_summary;

typedef struct {
    const allocator* selected[N_ALLOCATORS];
    size_t n_selected;
    size_t baseline;
    int reps;
    int warmup;
    FILE* csv;
} options;

static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// "traces/churn-s3.rep" -> "churn": the seeds of one workload form a family
static void family_of(const char* path, char* out, size_t cap) {
    snprintf(out, cap, "%s", basename_of(path));
    char* dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".rep") == 0)
        *dot = '\0';
    char* dash = strrchr(out, '-');
    if (dash && dash[1] == 's' && dash[2] && strspn(dash + 2, "0123456789") == strlen(dash + 2))
        *dash = '\0';
}

static void print_header(const options* o) {
    printf("%-20s %-6s %8s %9s %19s %8s %17s %7s %7s\n",
           "trace", "alloc", "ops", "Kops/s", "95% CI", "speedup", "95% CI", "util", "frag");
    printf("%-20s %-6s %8s %9s %19s %8s %17s %7s %7s\n",
           "", "", "", "(median)", "", "vs", o->selected[o->baseline]->name, "", "");
    fflush(stdout);
}

static void bench_trace(const trace* t, const options* o, trace_summary* s) {
    size_t n_alloc = o->n_selected;
    size_t reps = (size_t)o->reps;
    double* seconds = mmap_zeroed(n_alloc * reps * sizeof *seconds);
    bool failed[N_ALLOCATORS] = { false };
    char error[N_ALLOCATORS][sizeof(((result*)0)->error)];

    // warmups are discarded: they pay for first page faults and cold caches
    for (int w = 0; w < o->warmup; w++)
        for (size_t a = 0; a < n_alloc; a++)
            run_child(o->selected[a], t, MODE_THROUGHPUT);

    // interleave the allocators in a fresh random order each round, so drift
    // (clock speed, heat, other processes) hits all of them alike
    for (size_t r = 0; r < reps; r++) {
        size_t order[N_ALLOCATORS];
        for (size_t a = 0; a < n_alloc; a++)
            order[a] = a;
        for (size_t a = n_alloc; a > 1; a--) {
            size_t j = rng_below(a);
            size_t tmp = order[a - 1];
            order[a - 1] = order[j];
            order[j] = tmp;
        }
        for (size_t k = 0; k < n_alloc; k++) {
            size_t a = order[k];
            if (failed[a])
                continue;
            result res = run_child(o->selected[a], t, MODE_THROUGHPUT);
            if (!res.ok) {
                failed[a] = true;
                memcpy(error[a], res.error, sizeof error[a]);
            }
            seconds[a * reps + r] = res.seconds;
        }
    }

    result measured[N_ALLOCATORS];
    for (size_t a = 0; a < n_alloc; a++) {
        if (failed[a])
            continue;
        measured[a] = run_child(o->selected[a], t, MODE_UTILIZATION);
        if (!measured[a].ok) {
            failed[a] = true;
            memcpy(error[a], measured[a].error, sizeof error[a]);
        }
    }

    if (o->csv) {
        for (size_t a = 0; a < n_alloc; a++) {
            if (failed[a])
                continue;
            for (size_t r = 0; r < reps; r++)
                fprintf(o->csv, "%s,%s,%s,%zu,%zu,%.9f,%zu,%zu,%zu\n",
                        basename_of(t->name), s->family, o->selected[a]->name, r, t->n_ops,
                        seconds[a * reps + r], measured[a].peak_live, measured[a].peak_heap,
                        measured[a].frag_at_peak);
        }
        fflush(o->csv);
    }

    const double* base = failed[o->baseline] ? nullptr : &seconds[o->baseline * reps];
    for (size_t a = 0; a < n_alloc; a++) {
        cell* c = &s->cells[a];
        printf("%-20s %-6s %8zu ", basename_of(t->name), o->selected[a]->name, t->n_ops);
        if (failed[a]) {
            printf("FAILED: %s\n", error[a]);
            continue;
        }

        double* xs = &seconds[a * reps];
        if (base && a != o->baseline)
            speedup_ci(base, xs, reps, &c->speedup_lo, &c->speedup_hi);

        double sorted[reps];
        memcpy(sorted, xs, sizeof sorted);
        double med = median(sorted, reps), lo, hi;
        c->ok = true;
        c->kops = (double)t->n_ops / med / 1e3;
        c->has_ci = median_ci(sorted, reps, &lo, &hi);
        if (c->has_ci) { // a longer time is a lower throughput, so the ends swap
            c->kops_lo = (double)t->n_ops / hi / 1e3;
            c->kops_hi = (double)t->n_ops / lo / 1e3;
        }
        if (base) {
            double base_sorted[reps];
            memcpy(base_sorted, base, sizeof base_sorted);
            c->speedup = median(base_sorted, reps) / med;
        }

        const result* m = &measured[a];
        c->util = m->peak_heap ? 100.0 * (double)m->peak_live / (double)m->peak_heap : 0;
        c->frag = m->peak_live ? 100.0 * (double)m->frag_at_peak / (double)m->peak_live : 0;

        printf("%9.0f ", c->kops);
        if (c->has_ci)
            printf("[%8.0f, %8.0f] ", c->kops_lo, c->kops_hi);
        else
            printf("%19s ", "-");
        if (!base) {
            printf("%8s %17s ", "-", "");
        } else if (a == o->baseline) {
            printf("%7.2fx %17s ", 1.0, "");
        } else {
            bool significant = c->speedup_lo > 1 || c->speedup_hi < 1;  // CI excludes 1x
            printf("%7.2fx [%5.2f, %5.2f]%c ", c->speedup, c->speedup_lo, c->speedup_hi,
                   significant ? '*' : ' ');
        }
        printf("%6.1f%% %6.1f%%\n", c->util, c->frag);
    }
    fflush(stdout);
    mmap_release(seconds, n_alloc * reps * sizeof *seconds);
}

// One summary row over a family's traces (all traces if family is nullptr): the
// geometric mean of the speedups, since arithmetic means of ratios mislead, with a
// t-interval on their logs; utilization as paired differences from the baseline.
static void summarize(const trace_summary* s, size_t n_traces, const char* family, size_t a, const options* o) {
    double* log_speedup = mmap_zeroed(n_traces * sizeof *log_speedup);
    double* util = mmap_zeroed(n_traces * sizeof *util);
    double* util_diff = mmap_zeroed(n_traces * sizeof *util_diff);
    size_t n = 0, faster = 0, slower = 0;

    for (size_t i = 0; i < n_traces; i++) {
        const cell* c = &s[i].cells[a];
        const cell* b = &s[i].cells[o->baseline];
        if ((family && strcmp(family, s[i].family) != 0) || !c->ok || !b->ok)
            continue;
        log_speedup[n] = log(c->speedup);
        util[n] = c->util;
        util_diff[n] = c->util - b->util;
        faster += c->speedup_lo > 1;
        slower += c->speedup_hi < 1;
        n++;
    }

    printf("%-12s %-6s %6zu ", family ? family : "ALL", o->selected[a]->name, n);
    if (n == 0) {
        printf("no results\n");
    } else {
        double hw, mean = mean_ci(log_speedup, n, &hw);
        double util_mean = 0;
        for (size_t i = 0; i < n; i++)
            util_mean += util[i] / (double)n;
        double diff_hw, diff_mean = mean_ci(util_diff, n, &diff_hw);
        printf("%7.2fx ", exp(mean));
        if (isnan(hw))
            printf("%17s ", "-");
        else
            printf("[%5.2f, %5.2f]%c ", exp(mean - hw), exp(mean + hw),
                   mean - hw > 0 || mean + hw < 0 ? '*' : ' ');
        printf("%4zu/%-4zu %6.1f%% %+7.1f ", faster, slower, util_mean, diff_mean);
        if (isnan(diff_hw))
            printf("%17s\n", "-");
        else
            printf("[%+6.1f, %+6.1f]%c\n", diff_mean - diff_hw, diff_mean + diff_hw,
                   diff_mean - diff_hw > 0 || diff_mean + diff_hw < 0 ? '*' : ' ');
    }

    mmap_release(log_speedup, n_traces * sizeof *log_speedup);
    mmap_release(util, n_traces * sizeof *util);
    mmap_release(util_diff, n_traces * sizeof *util_diff);
}

static void print_summary(const trace_summary* s, size_t n_traces, const options* o) {
    printf("\nsummary vs %s, over traces (95%% CIs from the t distribution; * = excludes no difference)\n",
           o->selected[o->baseline]->name);
    printf("%-12s %-6s %6s   %8s %12s  %9s %7s  %8s %13s\n",
           "family", "alloc", "traces", "speedup", "95% CI", "fast/slow", "util", "util diff", "95% CI");

    for (size_t a = 0; a < o->n_selected; a++) {
        if (a == o->baseline)
            continue;
        for (size_t i = 0; i < n_traces; i++) {
            bool seen = false;
            for (size_t j = 0; j < i; j++)
                seen |= strcmp(s[i].family, s[j].family) == 0;
            if (!seen && s[i].family[0])
                summarize(s, n_traces, s[i].family, a, o);
        }
        summarize(s, n_traces, nullptr, a, o);
    }
}

static void usage(const char* prog) {
    fprintf(stderr,
            "usage: %s [-a allocator] [-b baseline] [-r reps] [-w warmup] [-c cpu] [-o file.csv] [-s seed] [trace ...]\n"
            "  -a  run only this allocator (default: all)\n"
            "  -b  allocator compared against (default: glibc)\n"
            "  -r  timed runs per allocator per trace (default: 15; 6 or more for CIs)\n"
            "  -w  discarded warmup runs (default: 1)\n"
            "  -c  pin benchmark to CPU core\n"
            "  -o  write timed runs to CSV\n"
            "  -s  seed for synthetic workload (default: 1)\n"
            "with no traces, replays the synthetic workload()\n"
            "allocators:", prog);
    for (size_t i = 0; i < N_ALLOCATORS; i++)
        fprintf(stderr, " %s", allocators[i].name);
    fputc('\n', stderr);
}

int main(int argc, char** argv) {
    options o = { .reps = 15, .warmup = 1 };
    const char* only = nullptr;
    const char* baseline = "glibc";
    const char* csv_path = nullptr;
    int cpu = -1;
    unsigned seed = 1;
    int c;

    while ((c = getopt(argc, argv, "a:b:r:w:c:o:s:h")) != -1) {
        switch (c) {
        case 'a': only = optarg; break;
        case 'b': baseline = optarg; break;
        case 'r': o.reps = atoi(optarg); break;
        case 'w': o.warmup = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'o': csv_path = optarg; break;
        case 's': seed = (unsigned)strtoul(optarg, nullptr, 0); break;
        default:  usage(argv[0]); return c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    if (o.reps < 1 || o.warmup < 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < N_ALLOCATORS; i++)
        if (!only || strcmp(only, allocators[i].name) == 0)
            o.selected[o.n_selected++] = &allocators[i];
    if (!o.n_selected) {
        fprintf(stderr, "unknown allocator '%s'\n", only);
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    // without the requested baseline (e.g. -a mm), the first allocator is the baseline
    for (size_t i = 0; i < o.n_selected; i++)
        if (strcmp(o.selected[i]->name, baseline) == 0)
            o.baseline = i;

    if (cpu >= 0) { // children inherit the pinning
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) == -1) {
            perror("sched_setaffinity");
            return EXIT_FAILURE;
        }
    }

    if (csv_path) {
        o.csv = fopen(csv_path, "w");
        if (!o.csv) {
            perror(csv_path);
            return EXIT_FAILURE;
        }
        fprintf(o.csv, "trace,family,allocator,run,ops,seconds,peak_live,peak_heap,int_frag\n");
    }

    size_t n_traces = optind < argc ? (size_t)(argc - optind) : 1;
    trace_summary* summaries = mmap_zeroed(n_traces * sizeof *summaries);
    int status = EXIT_SUCCESS;

    print_header(&o);
    for (size_t i = 0; i < n_traces; i++) {
        trace t = { 0 };
        if (optind < argc) {
            t.name = argv[optind + (int)i];
            if (!load_trace(&t, t.name)) {
                status = EXIT_FAILURE;
                continue;
            }
        } else {
            t.name = "synthetic";
            workload(&t, seed);
        }
        family_of(t.name, summaries[i].family, sizeof summaries[i].family);

        if (!t.n_ops) {
            fprintf(stderr, "%s: no operations\n", t.name);
        } else if (!check_trace(&t)) {
            status = EXIT_FAILURE;
        } else {
            bench_trace(&t, &o, &summaries[i]);
        }
        mmap_release(t.ops, t.cap * sizeof *t.ops);
    }

    if (o.n_selected > 1)
        print_summary(summaries, n_traces, &o);

    if (o.csv)
        fclose(o.csv);
    mmap_release(summaries, n_traces * sizeof *summaries);
    return status;
}
