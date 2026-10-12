/*
 * Tells valgrind where blocks begin and end; on its own it sees the sbrk heap as
 * one valid region. Linked with --wrap like mm_trace.c.
 */
#include "mm_alloc_internal.h"

#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

void* __real_mm_malloc(size_t size);
void* __real_mm_calloc(size_t n, size_t size);
void  __real_mm_free(void* ptr);

// the requested size, not the block size, so a write past what was asked for is caught
void* __wrap_mm_malloc(size_t size) {
    void* ptr = __real_mm_malloc(size);
    if (ptr)
        VALGRIND_MALLOCLIKE_BLOCK(ptr, size, 0, 0);
    return ptr;
}

// needs its own wrapper (see mm_trace.c); is_zeroed = 1 because memset already ran
void* __wrap_mm_calloc(size_t n, size_t size) {
    void* ptr = __real_mm_calloc(n, size);
    if (ptr)
        VALGRIND_MALLOCLIKE_BLOCK(ptr, n * size, 0, 1);
    return ptr;
}

void __wrap_mm_free(void* ptr) {
    if (!ptr) {
        __real_mm_free(ptr);
        return;
    }
    VALGRIND_FREELIKE_BLOCK(ptr, 0);
    // FREELIKE makes the block unaddressable, but the allocator is about to write
    // its links and footer there. DEFINED, not UNDEFINED: those bytes are real data.
    free_list* block = block_of(ptr);
    VALGRIND_MAKE_MEM_DEFINED(block, calculate_size(block));
    __real_mm_free(ptr);
}
