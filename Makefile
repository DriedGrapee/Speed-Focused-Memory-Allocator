CC = clang

STD_FLAGS  = -std=c23 -D_POSIX_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
WARN_FLAGS = -Wall -Wextra -Werror

CFLAGS       = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC
DEBUG_CFLAGS = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -Og -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
PERF_CFLAGS  = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -O2 -DNDEBUG
# no sanitizers here: ASan and valgrind both rewrite memory management and cannot
# run together
MASSIF_CFLAGS = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -O0 -fno-omit-frame-pointer

TEST_CFLAGS  = -Wl,-rpath=.
TEST_LDFLAGS = -ldl

# Tooling hooks into the allocator at link time instead of through #ifdefs in
# mm_alloc.c: with these flags the harness's calls go to the __wrap_mm_* functions
# in whichever wrapper object is linked (mm_trace.c, mm_valgrind.c), and those
# reach the allocator through __real_mm_*.
WRAP_LDFLAGS = -Wl,--wrap=mm_malloc,--wrap=mm_calloc,--wrap=mm_free

ALLOC_HEADERS = mm_alloc.h mm_alloc_internal.h mm_heap_dump.h

MUNIT_DIR = vendor/munit
# munit is vendored third-party code: build it with its own relaxed flags so our
# -Wall -Wextra -Werror cannot fail the build on upstream style rather than ours
MUNIT_CFLAGS = -g3 -std=c11 -D_POSIX_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -w

.PHONY: all debug perf gdb pwndbg massif memcheck test test-asan clean

all: hw3lib.so mm_test mm_debug

# ---- one object per source file per build variant ----
# the variant is the suffix: foo.o (default), foo_dbg.o, foo_perf.o, foo_massif.o
%.o: %.c $(ALLOC_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

%_dbg.o: %.c $(ALLOC_HEADERS)
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

%_perf.o: %.c $(ALLOC_HEADERS)
	$(CC) $(PERF_CFLAGS) -c -o $@ $<

%_massif.o: %.c $(ALLOC_HEADERS)
	$(CC) $(MASSIF_CFLAGS) -c -o $@ $<

# the unit tests need munit's header on top of the usual flags
mm_unit_tests.o: CFLAGS += -I$(MUNIT_DIR)
mm_unit_tests_dbg.o: DEBUG_CFLAGS += -I$(MUNIT_DIR)

# ---- default build: -g3, no optimization ----
# hw3lib*.so is the allocator alone: no heap dumps, no wrappers
hw3lib.so: mm_alloc.o
	$(CC) -shared -o $@ $^

mm_test: mm_test.c
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

mm_debug: mm_debug.o mm_alloc.o mm_heap_dump.o
	$(CC) $(CFLAGS) -o $@ $^

# ---- debug build: -Og, ASan+UBSan, abort on first violation ----
# mm_test dlopens "hw3lib.so" by that literal filename, so it can only ever
# exercise whichever .so currently sits at that exact path -- there is no
# variant-specific mm_test here. mm_debug_dbg is the one that matters: it
# links the allocator directly, so the sanitizers instrument it and
# breakpoints/watchpoints work without any dlopen ceremony. It also links
# mm_trace.c, which logs every call and dumps the heap after it. hw3lib_dbg.so
# is built alongside it for anyone who wants to dlopen the sanitized allocator
# by hand (rpath . + explicit path to dlopen).
debug: hw3lib_dbg.so mm_debug_dbg

hw3lib_dbg.so: mm_alloc_dbg.o
	$(CC) -shared -o $@ $^

mm_debug_dbg: mm_debug_dbg.o mm_alloc_dbg.o mm_heap_dump_dbg.o mm_trace_dbg.o
	$(CC) $(DEBUG_CFLAGS) $(WRAP_LDFLAGS) -o $@ $^

# ---- performance build: -O2 ----
perf: hw3lib_perf.so mm_debug_perf

hw3lib_perf.so: mm_alloc_perf.o
	$(CC) -shared -o $@ $^

mm_debug_perf: mm_debug_perf.o mm_alloc_perf.o mm_heap_dump_perf.o
	$(CC) $(PERF_CFLAGS) -o $@ $^

# ---- massif heap profile ----
# The allocator's own logging goes to /dev/null; massif writes to massif.out and
# ms_print renders it. Note valgrind emulates brk with a ~8MB ceiling (measured),
# far below the native limit, so a workload larger than that will see grow_heap
# fail here in a way it never would when run normally.
massif: mm_debug_massif
	valgrind --tool=massif --massif-out-file=massif.out ./mm_debug_massif >/dev/null
	ms_print massif.out

# Same annotations make memcheck understand the allocator, so it can finally see
# past the end of a block instead of treating the whole sbrk region as valid.
memcheck: mm_debug_massif
	valgrind --tool=memcheck --leak-check=full ./mm_debug_massif >/dev/null

# mm_valgrind.c wraps the allocator with the MALLOCLIKE/FREELIKE client requests
mm_debug_massif: mm_debug_massif.o mm_alloc_massif.o mm_heap_dump_massif.o mm_valgrind_massif.o
	$(CC) $(MASSIF_CFLAGS) $(WRAP_LDFLAGS) -o $@ $^

# ---- unit tests ----
# munit forks before every test, which is what makes these tests independent:
# the allocator's global state sits on an sbrk heap with no reset hook, so in a
# single process each test would inherit the previous one's heap. Forking also
# means a segfaulting test is reported as a failure instead of ending the run.
test: mm_unit_tests
	./mm_unit_tests

mm_unit_tests: mm_unit_tests.o mm_alloc.o $(MUNIT_DIR)/munit.o
	$(CC) $(CFLAGS) -o $@ $^

# forking is also what makes ASan usable here: a heap poisoned by one test
# cannot bleed into the next, and each report lands against the test that caused it
test-asan: mm_unit_tests_asan
	./mm_unit_tests_asan

mm_unit_tests_asan: mm_unit_tests_dbg.o mm_alloc_dbg.o $(MUNIT_DIR)/munit_asan.o
	$(CC) $(DEBUG_CFLAGS) -o $@ $^

$(MUNIT_DIR)/munit.o: $(MUNIT_DIR)/munit.c $(MUNIT_DIR)/munit.h
	$(CC) $(MUNIT_CFLAGS) -c -o $@ $<

$(MUNIT_DIR)/munit_asan.o: $(MUNIT_DIR)/munit.c $(MUNIT_DIR)/munit.h
	$(CC) $(MUNIT_CFLAGS) -Og -fsanitize=address,undefined -c -o $@ $<

# ---- gdb session ----
# mm_debug is already the right shape for this: -O0 and no sanitizers, so
# single-stepping is exact and there is no sanitizer runtime/stack frames to
# step through to reach your own code. -x .gdbinit loads the project's gdbinit
# explicitly, since gdb otherwise refuses to auto-load a local .gdbinit unless
# the directory is in auto-load safe-path.
gdb: mm_debug
	gdb -x .gdbinit ./mm_debug

# ---- pwndbg session ----
# -nx skips ~/.gdbinit on purpose: it holds gdb-dashboard, which hooks the same
# stop event pwndbg does, and running both fights over the display. -x .gdbinit
# then loads this project's commands (blk/flist/hwalk/bpalloc) on top of pwndbg.
pwndbg: mm_debug
	pwndbg -nx -x .gdbinit ./mm_debug

clean:
	rm -rf *.o hw3lib.so mm_test mm_debug \
	       hw3lib_dbg.so mm_debug_dbg \
	       hw3lib_perf.so mm_debug_perf \
	       mm_debug_massif massif.out \
	       mm_unit_tests mm_unit_tests_asan \
	       $(MUNIT_DIR)/munit.o $(MUNIT_DIR)/munit_asan.o
