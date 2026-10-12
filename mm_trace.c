/*
 * Logs each allocator call and dumps the heap after it. Linked with
 * -Wl,--wrap=mm_malloc,--wrap=mm_calloc,--wrap=mm_free: calls land in __wrap_*,
 * which reach the allocator through __real_*.
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

// mm_calloc's own call to mm_malloc stays inside mm_alloc.o, where --wrap does
// not reach, so calloc needs a wrapper of its own
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
