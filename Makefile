CC = clang

STD_FLAGS  = -std=c23 -D_POSIX_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
WARN_FLAGS = -Wall -Wextra -Werror

CFLAGS       = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC
DEBUG_CFLAGS = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -Og -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
PERF_CFLAGS  = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -O2 -DNDEBUG
# no sanitizers here: ASan and valgrind both rewrite memory management and cannot
# run together. MM_VALGRIND switches on the MALLOCLIKE/FREELIKE client requests.
MASSIF_CFLAGS = -g3 $(WARN_FLAGS) $(STD_FLAGS) -fPIC -O0 -fno-omit-frame-pointer -DMM_VALGRIND

TEST_CFLAGS  = -Wl,-rpath=.
TEST_LDFLAGS = -ldl

.PHONY: all debug perf gdb pwndbg massif memcheck clean

all: hw3lib.so mm_test mm_debug

# ---- default build: -g3, no optimization ----
hw3lib.so: mm_alloc.o
	$(CC) -shared -o $@ $^

mm_alloc.o: mm_alloc.c
	$(CC) $(CFLAGS) -c -o $@ $^

mm_test: mm_test.c
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# mm_debug.c #includes mm_alloc.c, so compile $< alone -- passing $^ would
# define every allocator symbol twice
mm_debug: mm_debug.c mm_alloc.c mm_alloc.h
	$(CC) $(CFLAGS) -O0 -fno-omit-frame-pointer -o $@ $<

# ---- debug build: -Og, ASan+UBSan, abort on first violation ----
# mm_test dlopens "hw3lib.so" by that literal filename, so it can only ever
# exercise whichever .so currently sits at that exact path -- there is no
# variant-specific mm_test here. mm_debug_dbg is the one that matters: it
# #includes mm_alloc.c directly, so the sanitizers instrument it and
# breakpoints/watchpoints work without any dlopen ceremony. hw3lib_dbg.so is
# built alongside it for anyone who wants to dlopen the sanitized allocator
# by hand (rpath . + explicit path to dlopen).
debug: hw3lib_dbg.so mm_debug_dbg

hw3lib_dbg.so: mm_alloc_dbg.o
	$(CC) -shared -o $@ $^

mm_alloc_dbg.o: mm_alloc.c
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

mm_debug_dbg: mm_debug.c mm_alloc.c mm_alloc.h
	$(CC) $(DEBUG_CFLAGS) -o $@ $<

# ---- performance build: -O2 ----
perf: hw3lib_perf.so mm_debug_perf

hw3lib_perf.so: mm_alloc_perf.o
	$(CC) -shared -o $@ $^

mm_alloc_perf.o: mm_alloc.c
	$(CC) $(PERF_CFLAGS) -c -o $@ $<

mm_debug_perf: mm_debug.c mm_alloc.c mm_alloc.h
	$(CC) $(PERF_CFLAGS) -o $@ $<

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

mm_debug_massif: mm_debug.c mm_alloc.c mm_alloc.h
	$(CC) $(MASSIF_CFLAGS) -o $@ $<

# ---- gdb session ----
# mm_debug is already the right shape for this: -O0 and -fno-omit-frame-pointer
# with no sanitizers, so single-stepping is exact and there is no sanitizer
# runtime/stack frames to step through to reach your own code. -x .gdbinit
# loads the project's gdbinit explicitly, since gdb otherwise refuses to
# auto-load a local .gdbinit unless the directory is in auto-load safe-path.
gdb: mm_debug
	gdb -x .gdbinit ./mm_debug

# ---- pwndbg session ----
# -nx skips ~/.gdbinit on purpose: it holds gdb-dashboard, which hooks the same
# stop event pwndbg does, and running both fights over the display. -x .gdbinit
# then loads this project's commands (blk/flist/hwalk/bpalloc) on top of pwndbg.
pwndbg: mm_debug
	pwndbg -nx -x .gdbinit ./mm_debug

clean:
	rm -rf hw3lib.so mm_alloc.o mm_test mm_debug \
	       hw3lib_dbg.so mm_alloc_dbg.o mm_debug_dbg \
	       hw3lib_perf.so mm_alloc_perf.o mm_debug_perf \
	       mm_debug_massif massif.out
