/*
 * mm_valgrind.c
 *
 * Valgrind annotations. mm_malloc/mm_free are not malloc/free, so valgrind cannot
 * see them on its own -- massif reports nothing and memcheck treats the whole sbrk
 * region as uniformly valid. These client requests hand it the block boundaries.
 *
 * Hooked in at link time with -Wl,--wrap=mm_malloc,--wrap=mm_calloc,--wrap=mm_free
 * (see the massif target), so the allocator itself carries none of this and the
 * project still builds without valgrind headers.
 */

#include "mm_alloc_internal.h"

#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

void* __real_mm_malloc(size_t size);
void* __real_mm_calloc(size_t n, size_t size);
void  __real_mm_free(void* ptr);

// report the REQUESTED size, not the block size, so memcheck flags a write that
// runs past what the caller actually asked for rather than only past the block
void* __wrap_mm_malloc(size_t size) {
    void* ptr = __real_mm_malloc(size);
    if (ptr)
        VALGRIND_MALLOCLIKE_BLOCK(ptr, size, 0, 0);
    return ptr;
}

// mm_calloc reaches mm_malloc inside mm_alloc.o, where --wrap does not apply, so it
// needs its own wrapper. is_zeroed because the memset has already run by now.
// n * size cannot overflow: mm_calloc returns null when it would.
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

    VALGRIND_FREELIKE_BLOCK(ptr, 0); // before coalescing, while ptr still names the block the caller was given

    // FREELIKE marks a block unaddressable, but this allocator stores its free-list
    // links and footer inside that same space, and is about to rewrite them while
    // coalescing. Without this the allocator's own bookkeeping reads as an invalid access.
    // DEFINED, not UNDEFINED: the headers, footers and links in this span are real
    // initialised data the allocator wrote, so marking them undefined would make
    // every later header read look like a use of an uninitialised value.
    free_list* block = block_of(ptr);
    VALGRIND_MAKE_MEM_DEFINED(block, calculate_size(block));

    __real_mm_free(ptr);
}
