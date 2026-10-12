/*
 * mm_alloc.c
 *
 * The allocator alone: one explicit free list, first fit, LIFO insertion and
 * immediate coalescing. Tooling reads the heap through mm_alloc_internal.h or
 * wraps the entry points at link time (mm_trace.c, mm_valgrind.c).
 *
 * TODO: mm_realloc, segregated free lists, deferred coalescing.
 */

#include "mm_alloc.h"
#include "mm_alloc_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

free_list* program_free_list;
free_list* prologue_ptr;
free_list* epilogue_ptr;
static bool heap_boundary_markers_initialized = false;

static void setup_free_block(free_list* block, size_t total_size, bool allocated, bool previous_allocated) {
    block->header = total_size;
    set_allocated(block, allocated);
    set_previous_allocated(block, previous_allocated);
    if (!allocated)
        set_footer(block);
}

static void* grow_heap(size_t size) {
    void* heap_ptr = sbrk(size);
    if (heap_ptr == (void *)-1) {
        perror("Error No Memory");
        return nullptr;
    }
    return heap_ptr;
}

// The epilogue is a zero-size allocated header at the end of the heap. Its
// prev_alloc starts set because it only ever follows a newly allocated block.
static inline void add_epilogue(free_list* block) {
    setup_free_block(block, 0, true, true);
    epilogue_ptr = block;
}

// 48 bytes: 8 of padding (so payloads land 16-byte aligned), a 32-byte allocated
// prologue, and the epilogue header.
static void initialize_prologue_and_epilogue(void) {
    free_list* prologue = grow_heap(2*sizeof(free_list));
    if (!prologue) {
        errno = ENOMEM;
        perror("Out Of Memory Therefore Cannot Organize Heap");
    } else {
        prologue = (free_list *)((unsigned char *)prologue + ALIGNMENT);
    }

    prologue_ptr = prologue;
    setup_free_block(prologue, 32, true, false);
    add_epilogue((free_list *)((unsigned char *)prologue + 4*ALIGNMENT));
    heap_boundary_markers_initialized = true;
}

// Grows the heap by size: the old epilogue becomes the new block's header, and a
// new epilogue goes at the end.
static free_list* create_block_at_end_of_heap(size_t size) {
    unsigned char* heap_ptr = grow_heap(size);
    if (!heap_ptr)
        return nullptr;

    free_list* block = (free_list *)(heap_ptr - ALIGNMENT);
    setup_free_block(block, size, true, previous_is_allocated(block));
    add_epilogue((free_list *)(heap_ptr + size - ALIGNMENT));
    return block;
}

static void push_free_list(free_list* block) {
    block->body.links.prev = nullptr;
    block->body.links.next = program_free_list;
    if (program_free_list)
        program_free_list->body.links.prev = block;
    program_free_list = block;
}

static void remove_from_free_list(free_list* block) {
    assert(block);
    if (block->body.links.prev)
        block->body.links.prev->body.links.next = block->body.links.next;
    if (block->body.links.next)
        block->body.links.next->body.links.prev = block->body.links.prev;
    if (block == program_free_list)
        program_free_list = block->body.links.next;
}

// Cuts block down to first_size and frees the rest. A free block's previous
// neighbour is always allocated (coalescing guarantees it), so prev_alloc is true.
static void split(free_list* block, size_t first_size) {
    assert(block);
    free_list* rest = (free_list *)((unsigned char *)block + first_size);
    setup_free_block(rest, calculate_size(block) - first_size, false, true);
    setup_free_block(block, first_size, true, true);
    push_free_list(rest);
}

// First fit over the free list; grows the heap when nothing fits.
static free_list* find_block(size_t size) {
    for (free_list* block = program_free_list; block; block = block->body.links.next) {
        if (calculate_size(block) < size)
            continue;
        // split only if the excess is worth keeping: more than 2 nodes and a footer (arbitrary)
        if (calculate_size(block) > size + sizeof(free_list)*2 + sizeof(size_t))
            split(block, size);
        remove_from_free_list(block);
        set_allocated(block, true);
        set_previous_allocated(find_next_in_memory(block), true);
        return block;
    }
    return create_block_at_end_of_heap(size);
}

// Merges a just-freed block (not yet on the free list) with its free neighbours
// and returns the merged block, which starts earlier if the previous one was absorbed.
static free_list* coalesce(free_list* block) {
    assert(!is_allocated(block));
    size_t merged_size = calculate_size(block);

    free_list* next = find_next_in_memory(block);
    if (!is_allocated(next)) {
        remove_from_free_list(next);
        merged_size += calculate_size(next);
    }

    if (!previous_is_allocated(block)) {
        free_list* prev = find_prev_in_memory(block);
        remove_from_free_list(prev);
        merged_size += calculate_size(prev);
        block = prev;
    }

    setup_free_block(block, merged_size, false, true);
    return block;
}

void* mm_malloc(size_t size) {
    // header + payload, rounded up to a multiple of 32
    size_t total_size = (offsetof(free_list, body) + size + 31) & ~(size_t)31;

    if (!heap_boundary_markers_initialized)
        initialize_prologue_and_epilogue();

    free_list* block = find_block(total_size);
    return block ? block->body.mem_block : nullptr;
}

void* mm_calloc(size_t n, size_t size) {
    size_t bytes;
    if (ckd_mul(&bytes, n, size)) {
        errno = ERANGE;
        perror("Integer Overflow");
        return nullptr;
    }
    void* ptr = mm_malloc(bytes);
    return ptr ? memset(ptr, 0, bytes) : nullptr;
}

/*
void* mm_realloc(void* ptr, size_t size) {
  //TODO: Implement realloc

  return nullptr;
}
*/

void mm_free(void* ptr) {
    if (!ptr)
        return;

    free_list* block = block_of(ptr);
    assert(is_allocated(block));

    set_allocated(block, false);
    set_footer(block);
    block = coalesce(block);
    set_previous_allocated(find_next_in_memory(block), false);
    push_free_list(block);
}
