/*
 * mm_debug.c
 *
 * Direct-linked debug harness. Links mm_alloc.o (and mm_heap_dump.o) straight in
 * rather than going through dlopen, so breakpoints bind at gdb startup and the
 * allocator's state (program_free_list, prologue_ptr, ...) is visible from gdb.
 * `call print_heap_data()` dumps the heap at any stop.
 *
 * mm_test.c remains the dlopen harness; it is what checks that hw3lib.so builds
 * and exports the right symbols. Use this one for debugging, that one for fidelity.
 *
 *   make mm_debug && gdb ./mm_debug
 */

#include "mm_alloc.h"

#include <assert.h>
#include <stdio.h>

static char stdout_buf[BUFSIZ];

int main(void) {
    // give stdout a static buffer: otherwise the first printf mallocs one through
    // glibc, which moves the program break underneath the allocator's sbrk heap
    setvbuf(stdout, stdout_buf, _IOFBF, sizeof stdout_buf);

    int* data = mm_malloc(3*sizeof(int));
    assert(data != NULL);

    float* flute = mm_malloc(sizeof(float));



    mm_free(flute);

    long* loud = mm_malloc(sizeof(long));

    unsigned long* ul_array = mm_calloc(599999, sizeof(unsigned long));

    mm_free(ul_array);

    *loud = 1231241234123154;

    mm_free(data);
    mm_free(loud);

    puts("malloc test successful!");
}
