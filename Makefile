CFLAGS=-g3 -Wall -Werror -Wextra -std=c23 -D_POSIX_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -fPIC
TEST_CFLAGS=-Wl,-rpath=.
TEST_LDFLAGS=-ldl

all: hw3lib.so mm_test mm_debug

hw3lib.so: mm_alloc.o
	clang -shared -o $@ $^

mm_alloc.o: mm_alloc.c
	clang $(CFLAGS) -c -o $@ $^

mm_test: mm_test.c
	clang $(CFLAGS) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# mm_debug.c #includes mm_alloc.c, so compile $< alone -- passing $^ would
# define every allocator symbol twice
mm_debug: mm_debug.c mm_alloc.c mm_alloc.h
	clang $(CFLAGS) -O0 -fno-omit-frame-pointer -o $@ $<

clean:
	rm -rf hw3lib.so mm_alloc.o mm_test mm_debug
