/*
 * mm_trace.c
 *
 * Logs every mm_malloc/mm_calloc/mm_free call and dumps the heap after it.
 * Hooked in at link time rather than compiled into the allocator: linking with
 *
 *   -Wl,--wrap=mm_malloc,--wrap=mm_calloc,--wrap=mm_free
 *
 * sends the harness's calls to the __wrap_ functions below, and __real_ names
 * the allocator's own. Calls made from inside mm_alloc.o (mm_calloc -> mm_malloc)
 * are resolved within that object and never wrapped, which is why mm_calloc has
 * its own wrapper.
 */

#include "mm_heap_dump.h"

#include <stdio.h>

void* __real_mm_malloc(size_t size);
void* __real_mm_calloc(size_t n, size_t size);
void  __real_mm_free(void* ptr);

void* __wrap_mm_malloc(size_t size) {
    void* ptr = __real_mm_malloc(size);
    printf("Allocating %zu bytes at %p\n", size, ptr);
    print_heap_data();
    return ptr;
}

void* __wrap_mm_calloc(size_t n, size_t size) {
    void* ptr = __real_mm_calloc(n, size);
    printf("Allocating %zu x %zu zeroed bytes at %p\n", n, size, ptr);
    print_heap_data();
    return ptr;
}

void __wrap_mm_free(void* ptr) {
    printf("Freeing %p\n", ptr ? (void *)block_of(ptr) : nullptr);
    __real_mm_free(ptr);
    print_heap_data();
}
