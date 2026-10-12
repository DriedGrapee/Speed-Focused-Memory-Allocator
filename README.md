# Speed-Focused Memory Allocator

A `malloc`/`free`/`calloc` implementation in C23, built on `sbrk`, for the CS 162
(Fall 2025) memory homework. Around it are tooling for testing, debugging, heap
profiling and statistically rigorous benchmarking against glibc.

## Status

- **Implemented and tested:** `mm_malloc`, `mm_free`, `mm_calloc`
- **In progress:** `mm_realloc`. It is declared in `mm_alloc.h` but commented out in
  `mm_alloc.c`; the benchmark stands in for it with malloc + copy + free.
- **Planned:** segregated free lists, deferred coalescing, broader unit tests.
  The [benchmark results](#results-against-glibc) suggest where to start.

## Requirements

- Linux x86-64 (the allocator uses `sbrk`; the benchmark uses `fork`, `mmap` and `sched_setaffinity`)
- clang with C23 support (`nullptr`, `<stdckdint.h>`) and GNU make
- Python 3, to generate benchmark traces
- Optional: gdb or [pwndbg](https://github.com/pwndbg/pwndbg), valgrind

Developed with Ubuntu clang 21.1.8 and glibc 2.43, under WSL2.

## Quick start

```bash
make              # hw3lib.so, mm_test, mm_debug
make test         # unit tests
make bench-suite  # benchmark against glibc (about 2 minutes)
```

## Project layout

| File | Purpose |
|---|---|
| `mm_alloc.c` | The allocator, and nothing else |
| `mm_alloc.h` | Public interface (`mm_malloc`, `mm_calloc`, `mm_realloc`, `mm_free`) |
| `mm_alloc_internal.h` | Block layout and header helpers, shared with the tooling |
| `mm_test.c` | Starter-code harness: `dlopen`s `hw3lib.so` to check the exported symbols |
| `mm_debug.c` | Direct-linked harness for gdb, ASan and valgrind |
| `mm_heap_dump.c/.h` | `print_heap_data()`: walks the heap and the free list |
| `mm_trace.c` | Logs each call and dumps the heap after it (linked into `mm_debug_dbg`) |
| `mm_valgrind.c` | Tells valgrind where blocks begin and end |
| `mm_unit_tests.c` | munit unit tests |
| `mm_bench.c` | Benchmark harness |
| `gen_traces.py` | Generates the benchmark's workload traces |
| `.gdbinit` | gdb commands for inspecting blocks and the heap |
| `vendor/munit/` | [µnit](https://nemequ.github.io/munit/) test framework (MIT) |

## Allocator design

**Block layout.** Every block starts with an 8-byte header holding the block's
total size. Sizes are multiples of 32, which frees the two low bits for flags.
A free block also stores its free-list links and a footer copy of its size:

```
allocated:  [ size | prev_alloc | alloc ][ payload ...                        ]
free:       [ size | prev_alloc | alloc ][ prev ][ next ][ ... ][ size footer ]
```

- **Sizes:** a request is rounded up to a multiple of 32 bytes, including the
  header. The smallest block is therefore 32 bytes, and payloads are 16-byte
  aligned.
- **Free list:** free blocks sit on one explicit, doubly linked list. Freed
  blocks go on the front (LIFO), and `mm_malloc` takes the first block that is
  big enough (first fit).
- **Splitting:** a chosen block is split when more than 56 bytes would be left
  over; the remainder goes back on the free list.
- **Coalescing:** `mm_free` merges a block with free neighbours immediately, using
  boundary tags ([Knuth][knuth]; CS:APP §9.9). The `prev_alloc` bit means only
  free blocks need a footer, so an allocated block can use that space for its
  payload.
- **Sentinels:** a prologue and an epilogue block bound the heap, so coalescing
  never needs to check for the heap's edges.
- **Growth:** when nothing on the list fits, the heap grows with `sbrk` by
  exactly the new block's size. Memory is never returned to the OS.
- **`mm_calloc`:** checks `n * size` for overflow with `ckd_mul` before zeroing.

**Limitations.**
- **Not thread-safe.**
- **Shares the program break with glibc.** Any glibc `malloc` call in the same
  process (a first `printf` allocates its buffer) moves the break underneath the
  heap. This is why the harnesses give stdout a static buffer.

## Building

| Target | Builds | Flags |
|---|---|---|
| `make` / `make all` | `hw3lib.so`, `mm_test`, `mm_debug` | `-g3 -O0` |
| `make debug` | `hw3lib_dbg.so`, `mm_debug_dbg` | `-Og`, ASan + UBSan, stops at the first error |
| `make perf` | `hw3lib_perf.so`, `mm_debug_perf` | `-O2 -DNDEBUG` |
| `make clean` | removes the builds and `traces/`; keeps `bench.csv` | |

Every build uses `-Wall -Wextra -Werror`. Each source file is compiled once per
variant: `foo.o`, `foo_dbg.o`, `foo_perf.o`, `foo_massif.o`.

The tooling never adds `#ifdef`s to the allocator. Instead it hooks in at link
time with `-Wl,--wrap=mm_malloc,--wrap=mm_calloc,--wrap=mm_free`: the harness's
calls then go to the `__wrap_mm_*` functions in `mm_trace.c` or `mm_valgrind.c`,
which reach the allocator through `__real_mm_*`.

## Testing

| Command | What it does |
|---|---|
| `make test` | munit unit tests |
| `make test-asan` | the same tests under AddressSanitizer and UBSan |
| `make memcheck` | runs `mm_debug` under valgrind memcheck |
| `make massif` | heap profile of `mm_debug` (valgrind massif, then `ms_print`) |
| `./mm_test` | smoke test that `hw3lib.so` loads and exports the right symbols |

**Unit tests.** They cover non-null returns, 16-byte payload alignment, block
headers, and reuse of a freed block. munit forks before every test. The
allocator has no reset hook, so forking is what gives each test a fresh heap,
and it turns a segfault into a reported failure. To add a test, write a
`static MunitResult test_xyz(const MunitParameter[], void*)` and add one row to
`tests[]` in `mm_unit_tests.c`.

**Valgrind.** On its own, valgrind can't see inside the `sbrk` region.
`mm_valgrind.c` reports each block to it with the `MALLOCLIKE`/`FREELIKE`
client requests, using the *requested* size, so memcheck flags a write past what
the caller asked for. Valgrind also caps `brk` at about 8 MB, so a larger
workload runs out of memory under valgrind even though it works normally.

## Debugging

```bash
make gdb       # gdb on mm_debug, with .gdbinit loaded
make pwndbg    # pwndbg on mm_debug (skips ~/.gdbinit)
```

`mm_debug` links the allocator directly at `-O0`, so breakpoints bind at startup
and globals like `program_free_list` are visible. `.gdbinit` adds:

| Command | Prints |
|---|---|
| `blk ADDR` | one block's size, flags, and footer and links if it is free |
| `flist` | the free list, stopping after 32 nodes in case it is cyclic |
| `hwalk ADDR` | every block from `ADDR` to `sbrk(0)`, free or not |
| `bpalloc` | sets breakpoints on `mm_malloc`, `mm_free`, `coalesce` and `split` |

`call print_heap_data()` dumps the whole heap and free list at any stop.
`./mm_debug_dbg` (from `make debug`) prints that dump after every call, with the
sanitizers enabled.

## Benchmarking

`mm_bench` replays allocation traces against each allocator and reports, per trace:

- **Throughput:** operations per second, timed with `clock_gettime`, against the
  `-O2` build of the allocator.
- **Utilization:** peak live data divided by peak heap growth. Higher is better.
- **Internal fragmentation:** the sum over live blocks of (usable size −
  requested size), taken when live data peaks. Usable size is counted the way
  glibc's `malloc_usable_size` counts it: the block minus its 8-byte header.

### Running

```bash
make bench-suite                         # all traces; every run saved to bench.csv
make bench-suite BENCH_FLAGS="-r 30 -c 2"
./mm_bench -r 30 traces/churn-s*.rep      # any subset of traces
make bench                               # the synthetic workload() in mm_bench.c
```

| Option | Meaning | Default |
|---|---|---|
| `-a NAME` | run only this allocator (`mm`, `glibc`) | all |
| `-b NAME` | allocator the others are compared against | `glibc` |
| `-r N` | timed runs per allocator per trace (6 or more for confidence intervals) | 15 |
| `-w N` | warmup runs, discarded | 1 |
| `-c CPU` | pin the benchmark to one CPU core | off |
| `-o FILE` | write every timed run to a CSV | off |
| `-s SEED` | seed for the synthetic `workload()` | 1 |

### Trace format (`.rep`)

`.rep` files use the trace format of the CS:APP malloc lab. An optional header of
four numbers (suggested heap size, id count, op count, weight) is ignored, then
there is one operation per line:

```
a <id> <size>    ptr[id] = malloc(size)
f <id>           free(ptr[id])
r <id> <size>    ptr[id] = realloc(ptr[id], size)
```

An id names one live allocation and can be reused once it has been freed. Lines
starting with `#` are comments. Each trace is checked before it runs, so freeing
an id that isn't live is reported as a bad trace rather than as an allocator crash.

### Workloads

`gen_traces.py` writes `traces/<family>-s<seed>.rep`: 9 families × 5 seeds,
about 100k operations each. Uniformly random traces are a poor model of real
programs, which request a few common sizes, mostly small ones, and whose live
memory grows and shrinks in phases ([Wilson et al.][wilson]; ramps, peaks and
plateaus as described in [Stanford CS140][cs140]). Each family models one of
those patterns:

| Family | Models |
|---|---|
| `churn` | steady state: short-lived small objects |
| `ramp` | live memory only grows (rare frees) |
| `peaks` | build a large structure, free 98% of it, repeat |
| `plateau` | long-lived objects, with short-lived churn beside them |
| `bimodal` | tiny nodes mixed with 4–64 KB buffers (fragmentation stress) |
| `fifo` / `lifo` | objects freed in queue order / stack order |
| `realloc` | growable buffers doubling through `realloc` |
| `uniform` | uniformly random sizes and lifetimes; kept only as a baseline |

```bash
python3 gen_traces.py --seeds 10 --ops 400000 --families churn,ramp
```

To benchmark a workload of your own, fill in `workload()` in `mm_bench.c` with
`trace_malloc`, `trace_free` and `trace_realloc` calls. They record the
operations, and every allocator then replays the same sequence, so random
number generation stays out of the timing.

### Method

- **A fresh process per run.** Every run is a forked child, because the
  allocator can't reset its heap. This also keeps glibc and `mm_alloc` from
  sharing a program break, and turns a crash into a `FAILED` row instead of
  ending the benchmark. The harness's own buffers come from `mmap`, so they stay
  out of the glibc heap being measured.
- **Timing kept separate.** Timed runs do nothing but the calls. A separate,
  untimed run does the bookkeeping for utilization and fragmentation.
- **Interleaved runs.** The allocators take turns, in a new random order every
  round, so drift during the benchmark (clock speed, heat, other processes)
  affects all of them equally ([Mytkowicz et al.][mytkowicz]).
- **Confidence intervals, not single numbers** ([Georges et al.][georges]).
- **Throughput:** the median of the `-r` runs, with a distribution-free 95%
  confidence interval taken from the sorted runs. Timing noise is skewed, since
  interference only ever slows a run down.
- **Speedup vs the baseline:** a bootstrap 95% CI from 2000 resamples
  ([Kalibera & Jones][kalibera]). `*` means the CI excludes 1×, i.e. the
  difference is significant at the 5% level.
- **Summary per family:** the geometric mean of the per-trace speedups, since
  the arithmetic mean of ratios is misleading ([Fleming & Wallace][fleming]),
  plus the mean utilization difference on the same traces. The CIs come from the
  t distribution across seeds; `fast/slow` counts the traces that were
  significantly faster or slower.

Utilization and fragmentation don't vary between runs of the same trace. Their
uncertainty comes from which trace was generated, which is why each family has
several seeds.

To benchmark another allocator (mimalloc, jemalloc, …), add an entry to the
`allocators[]` table in `mm_bench.c` and link the library.

## Results against glibc

Measured on 2026-10-12 with an AMD Ryzen 7 9800X3D under WSL2 (Linux 6.18),
glibc 2.43 and clang 21.1.8 (`-O2`). The run covered 45 traces, timing each
allocator 30 times per trace while pinned to one core:

```bash
make bench-suite BENCH_FLAGS="-r 30 -c 2"
```

| Family | Speedup vs glibc (95% CI) | Utilization mm / glibc | Difference (95% CI) |
|---|---|---|---|
| `bimodal` | 0.40× [0.38, 0.41] | 46.6% / 88.4% | −41.8 [−43.3, −40.4] |
| `churn` | 0.46× [0.45, 0.48] | 44.5% / 51.2% | −6.7 [−8.6, −4.9] |
| `fifo` | 0.76× [0.73, 0.80] | 69.0% / 31.4% | +37.6 [+30.8, +44.3] |
| `lifo` | 0.77× [0.74, 0.81] | 70.7% / 53.5% | +17.2 [+11.6, +22.9] |
| `peaks` | 0.73× [0.68, 0.78] | 98.1% / 99.2% | −1.1 [−3.3, +1.1], not significant |
| `plateau` | 0.67× [0.66, 0.68] | 99.3% / 98.7% | +0.6 [+0.5, +0.6] |
| `ramp` | 0.35× [0.35, 0.36] | 69.9% / 76.1% | −6.2 [−6.6, −5.7] |
| `realloc`† | 0.91× [0.84, 0.99] | 44.5% / 84.8% | −40.3 [−42.7, −37.8] |
| `uniform` | 0.15× [0.14, 0.15] | 84.2% / 96.5% | −12.3 [−12.6, −12.0] |
| **All 45 traces** | **0.51× [0.44, 0.60]** | 69.6% / 75.5% | −5.9 [−13.0, +1.2], not significant |

† `mm_realloc` is not implemented yet, so the harness stands in for it with
malloc + copy + free.

Utilization differences are in percentage points. Speedups below 1× mean `mm`
is slower. Pinning kept most per-trace intervals within a few percent; every
timed run is in `bench.csv`.

### What stands out

**Throughput: about half of glibc's overall.** `mm` is significantly slower on
44 of 45 traces; the remaining one (`realloc-s3`) is a statistical tie.
Instrumenting the seed-1 traces shows two separate causes:

- **One system call per heap growth (`ramp`, 0.35×).** In `ramp` the heap grows on
  85% of mallocs, and each growth is its own `sbrk`: about 76,000 `brk` calls per
  run (counted with `strace -c`), against about 40 for glibc. glibc extends the
  heap with 128 KB of padding at a time (`M_TOP_PAD`, see [mallopt(3)][mallopt]).
  Growing the heap in large chunks and splitting them would remove most of these
  calls.
- **Long first-fit searches (`uniform` 0.15×, `bimodal` 0.40×, `churn` 0.46×).**
  Each malloc walks a single free list that averages 2,149, 1,118 and 620 blocks
  in these traces. Segregated free lists (planned) would bound these searches.

The families with short free lists, `fifo` and `lifo` (under 10 blocks), come
closest at 0.76–0.77×. The gap that remains there is per-call overhead.

**Utilization: mixed, with no significant difference overall.**

- **`bimodal` (−42 points).** Even with about 1,100 blocks on the free list, 6%
  of mallocs still grow the heap: the free holes are too small for the 4–64 KB
  buffers. Heap growth also never extends a free block that sits at the end of
  the heap; it adds a whole new block after it.
- **`realloc` (−40 points).** With realloc emulated as malloc + copy + free, a
  buffer can never grow in place, and the old and new copies are both live
  during the copy. This should improve once `mm_realloc` exists.
- **The `fifo`/`lifo` wins (+38 and +17) are mostly an artifact.** These traces
  keep under 300 KB live, so glibc's peak heap is dominated by the 128 KB padding
  its first `sbrk` takes.

**Internal fragmentation: about 2× glibc's on small objects.** For example,
`churn` wastes 33.5% of live data against glibc's 13.9%. Blocks are rounded to
32 bytes where glibc rounds to 16, so a 32-byte request takes a 64-byte block
(24 bytes wasted) here and a 48-byte chunk (8 bytes wasted) in glibc.

## Sources

- [Wilson, Johnstone, Neely & Boles, *Dynamic Storage Allocation: A Survey and Critical Review* (IWMM 1995)][wilson]
- [Stanford CS140 lecture on dynamic memory allocation (ramps, peaks, plateaus)][cs140]
- [Kalibera & Jones, *Rigorous Benchmarking in Reasonable Time* (ISMM 2013)][kalibera]
- [Mytkowicz, Diwan, Hauswirth & Sweeney, *Producing Wrong Data Without Doing Anything Obviously Wrong!* (ASPLOS 2009)][mytkowicz]
- [Fleming & Wallace, *How Not to Lie with Statistics: The Correct Way to Summarize Benchmark Results* (CACM 1986)][fleming]
- [Georges, Buytaert & Eeckhout, *Statistically Rigorous Java Performance Evaluation* (OOPSLA 2007)][georges]
- Bryant & O'Hallaron, *Computer Systems: A Programmer's Perspective*, 3rd ed., §9.9 (Dynamic Memory Allocation) and the malloc lab trace format
- [Knuth, *The Art of Computer Programming*, Vol. 1, §2.5 (boundary tags)][knuth]
- [mallopt(3): glibc's `M_TOP_PAD` heap padding][mallopt]
- [µnit testing framework][munit]
- [Valgrind client requests (`MALLOCLIKE_BLOCK`)][valgrind]

[wilson]: https://users.cs.northwestern.edu/~pdinda/ics-f08/doc/dsa.pdf
[cs140]: https://web.stanford.edu/class/archive/cs/cs140/cs140.1088/lectures/sum0708/lecture14.pdf
[kalibera]: https://kar.kent.ac.uk/33611/
[mytkowicz]: https://users.cs.northwestern.edu/~robby/courses/322-2013-spring/mytkowicz-wrong-data.pdf
[fleming]: https://cacm.acm.org/research/how-not-to-lie-with-statistics-the-correct-way-to-summarize-benchmark-results
[georges]: https://dri.es/files/oopsla07-georges.pdf
[knuth]: https://www-cs-faculty.stanford.edu/~knuth/taocp.html
[mallopt]: https://man7.org/linux/man-pages/man3/mallopt.3.html
[munit]: https://nemequ.github.io/munit/
[valgrind]: https://valgrind.org/docs/manual/manual-core-adv.html#manual-core-adv.clientreq
