CC = clang

STD_FLAGS  = -std=c23 -D_POSIX_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
WARN_FLAGS = -Wall -Wextra -Werror

CFLAGS        = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC
DEBUG_CFLAGS  = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -Og -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
PERF_CFLAGS   = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -O2 -DNDEBUG
# no sanitizers: they and valgrind both take over memory management
MASSIF_CFLAGS = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -O0 -fno-omit-frame-pointer

TEST_CFLAGS   = -Wl,-rpath=.
TEST_LDFLAGS  = -ldl
# tooling hooks in at link time: calls go to __wrap_mm_* (mm_trace.c, mm_valgrind.c)
WRAP_LDFLAGS  = -Wl,--wrap=mm_malloc,--wrap=mm_calloc,--wrap=mm_free
ALLOC_HEADERS = mm_alloc.h mm_alloc_internal.h mm_heap_dump.h

# vendored: relaxed flags, so -Werror cannot fail on upstream style
MUNIT_DIR     = vendor/munit
MUNIT_CFLAGS  = -g3 -std=c11 -D_POSIX_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -w

.PHONY: all debug perf bench bench-suite gdb pwndbg massif memcheck test test-asan clean

all: hw3lib.so mm_test mm_debug

# one object per source per variant: foo.o, foo_dbg.o, foo_perf.o, foo_massif.o
%.o: %.c $(ALLOC_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

%_dbg.o: %.c $(ALLOC_HEADERS)
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

%_perf.o: %.c $(ALLOC_HEADERS)
	$(CC) $(PERF_CFLAGS) -c -o $@ $<

%_massif.o: %.c $(ALLOC_HEADERS)
	$(CC) $(MASSIF_CFLAGS) -c -o $@ $<

mm_unit_tests.o: CFLAGS += -I$(MUNIT_DIR)
mm_unit_tests_dbg.o: DEBUG_CFLAGS += -I$(MUNIT_DIR)

# Default build (-g3, -O0)
hw3lib.so: mm_alloc.o
	$(CC) -shared -o $@ $^

mm_test: mm_test.c
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

mm_debug: mm_debug.o mm_alloc.o mm_heap_dump.o
	$(CC) $(CFLAGS) -o $@ $^

# Debug build (-Og, ASan, UBSan). mm_test dlopens the literal "hw3lib.so", so the
# sanitized harness is mm_debug_dbg, which also logs every call (mm_trace.c).
debug: hw3lib_dbg.so mm_debug_dbg

hw3lib_dbg.so: mm_alloc_dbg.o
	$(CC) -shared -o $@ $^

mm_debug_dbg: mm_debug_dbg.o mm_alloc_dbg.o mm_heap_dump_dbg.o mm_trace_dbg.o
	$(CC) $(DEBUG_CFLAGS) $(WRAP_LDFLAGS) -o $@ $^

# Performance build (-O2)
perf: hw3lib_perf.so mm_debug_perf

hw3lib_perf.so: mm_alloc_perf.o
	$(CC) -shared -o $@ $^

mm_debug_perf: mm_debug_perf.o mm_alloc_perf.o mm_heap_dump_perf.o
	$(CC) $(PERF_CFLAGS) -o $@ $^

# Benchmarking. TRACES="traces/*.rep" replays traces (default: workload() in
# mm_bench.c); BENCH_FLAGS passes options, e.g. "-r 30 -c 2".
bench: mm_bench
	./mm_bench $(TRACES)

bench-suite: mm_bench traces/.generated
	./mm_bench $(BENCH_FLAGS) -o bench.csv traces/*.rep

# regenerated when the generator changes; not checked in
traces/.generated: gen_traces.py
	python3 gen_traces.py --out traces
	touch $@

mm_bench: mm_bench_perf.o mm_alloc_perf.o
	$(CC) $(PERF_CFLAGS) -o $@ $^ -lm

# Valgrind massif and memcheck. Valgrind caps brk at ~8 MB, so larger workloads
# run out of memory here only.
massif: mm_debug_massif
	valgrind --tool=massif --massif-out-file=massif.out ./mm_debug_massif >/dev/null
	ms_print massif.out

memcheck: mm_debug_massif
	valgrind --tool=memcheck --leak-check=full ./mm_debug_massif >/dev/null

mm_debug_massif: mm_debug_massif.o mm_alloc_massif.o mm_heap_dump_massif.o mm_valgrind_massif.o
	$(CC) $(MASSIF_CFLAGS) $(WRAP_LDFLAGS) -o $@ $^

# Unit tests
test: mm_unit_tests
	./mm_unit_tests

mm_unit_tests: mm_unit_tests.o mm_alloc.o $(MUNIT_DIR)/munit.o
	$(CC) $(CFLAGS) -o $@ $^

test-asan: mm_unit_tests_asan
	./mm_unit_tests_asan

mm_unit_tests_asan: mm_unit_tests_dbg.o mm_alloc_dbg.o $(MUNIT_DIR)/munit_asan.o
	$(CC) $(DEBUG_CFLAGS) -o $@ $^

$(MUNIT_DIR)/munit.o: $(MUNIT_DIR)/munit.c $(MUNIT_DIR)/munit.h
	$(CC) $(MUNIT_CFLAGS) -c -o $@ $<

$(MUNIT_DIR)/munit_asan.o: $(MUNIT_DIR)/munit.c $(MUNIT_DIR)/munit.h
	$(CC) $(MUNIT_CFLAGS) -Og -fsanitize=address,undefined -c -o $@ $<

# Debugger sessions. -x because gdb will not auto-load a local .gdbinit; -nx skips
# ~/.gdbinit, whose gdb-dashboard fights pwndbg over the display.
gdb: mm_debug
	gdb -x .gdbinit ./mm_debug

pwndbg: mm_debug
	pwndbg -nx -x .gdbinit ./mm_debug

clean:
	rm -rf *.o hw3lib.so mm_test mm_debug \
	       hw3lib_dbg.so mm_debug_dbg \
	       hw3lib_perf.so mm_debug_perf mm_bench \
	       traces \
	       mm_debug_massif massif.out \
	       mm_unit_tests mm_unit_tests_asan \
	       $(MUNIT_DIR)/munit.o $(MUNIT_DIR)/munit_asan.o
