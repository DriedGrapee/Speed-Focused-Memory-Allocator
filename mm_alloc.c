/*
 * mm_alloc.c
 *
 * The allocator and nothing else. Debugging, valgrind and benchmarking support
 * live in their own files and hook in from outside: they read the heap through
 * mm_alloc_internal.h, or intercept mm_malloc/mm_calloc/mm_free at link time with
 * -Wl,--wrap (see mm_trace.c and mm_valgrind.c).
 */

#include "mm_alloc.h"
#include "mm_alloc_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <stdckdint.h>

/*
 * Will start with immediate coalescence then add differed coalescence later
 * Will add segmented free list
 */

free_list* program_free_list;
static bool heap_boundary_markers_initialized = false;

free_list* prologue_ptr;
free_list* epilogue_ptr;

static void setup_free_block(free_list* free_block, size_t total_size, bool allocated, bool previous_allocated) {
    free_block->header = total_size;

    set_allocated(free_block, allocated);
    set_previous_allocated(free_block, previous_allocated);

    if(!allocated)
       set_footer(free_block);

    return;
}

static void* grow_heap(size_t size) {
    void* heap_ptr;
    if ((heap_ptr = sbrk(size)) == (void *)-1) { // forces size to the next greatest multiple of 32 (unless it is already a mult of 32)
        perror("Error No Memory");
        return nullptr;
    }

    return heap_ptr;
}

// puts an epilogue at the end of the heap. It is initialzed with previous_allocated = true because a new epilogue is added only when a new allocated block is inserted at the end of the heap
static inline void add_epilogue(free_list* free_block) {
    setup_free_block(free_block, 0, true, true); // sets up epilogue
    epilogue_ptr = free_block;
}

// puts the prologue and epiloge blocks
static void initialize_prologue_and_epilogue(void) {
    free_list* prologue;

    if (!(prologue = (free_list *)((unsigned char *)grow_heap(2*sizeof(free_list))))) {
       errno = ENOMEM;
       perror("Out Of Memory Therefore Cannot Organize Heap");
    } else {
        prologue = (free_list *)((unsigned char*)prologue + ALIGNMENT); // need space for 1 ALIGNMENT + 1 Header + 1 Footer + 16byte padding + 1 Header
    }

    prologue_ptr = prologue;

    setup_free_block(prologue, 32, true, false); // sets up prologue
    add_epilogue((free_list *)((unsigned char*)prologue + 4*ALIGNMENT));
    heap_boundary_markers_initialized = true;

    return;
}

static free_list* create_block_at_end_of_heap(size_t size) {
    free_list* heap_ptr = grow_heap(size);
    free_list* epilogue = (free_list *)((unsigned char*)heap_ptr  - ALIGNMENT);
    if (heap_ptr) {
        setup_free_block(epilogue, size, true, previous_is_allocated(epilogue));
    } else {
       return nullptr;
    }

    add_epilogue((free_list *)((unsigned char*)heap_ptr + size - ALIGNMENT));

    return (free_list*)((unsigned char*)heap_ptr - ALIGNMENT); //now that there is an epilogue, you want to return the address of where the old epilogue was, so it is 1 header's size before the ptr returned by grow_heap
}

static void remove_from_free_list(free_list* free_block) {

    assert(free_block);
    if (free_block->body.links.prev) {
        (free_block->body.links.prev)->body.links.next = free_block->body.links.next;
    }
    if (free_block->body.links.next) {
        (free_block->body.links.next)->body.links.prev = free_block->body.links.prev;
    }

    if (free_block == program_free_list) {
        program_free_list = free_block->body.links.next;
    }

    return;
}

// splits chosen block into a first block (first in terms of memory address) of size = size_of_first_block and a second block whose size is the rest of the free block being split
static void split(free_list* current_free_block, size_t size_of_first_block) {

    assert(current_free_block);

    free_list* new_free_block = (free_list *)((unsigned char *)current_free_block + size_of_first_block);
    *new_free_block = (free_list) {};
    setup_free_block(new_free_block, calculate_size(current_free_block) - size_of_first_block, false, true);
    setup_free_block(current_free_block, size_of_first_block, true, true);
    new_free_block->body.links.prev = nullptr;
    new_free_block->body.links.next = program_free_list;
    if (program_free_list) // program_free_list is null whenever the free list is empty
        program_free_list->body.links.prev = new_free_block;

    program_free_list = new_free_block;

    return;
}

// finds a block that is large enough to house the requested memory as well as the header by looking through the free_list in order, and if it comes to the end it increases the heap by size and allocates a block within that new memory
static free_list* find_block(free_list* current_free_block, size_t size) {

    assert(current_free_block);

    if (calculate_size(current_free_block) >= size) {
        if (calculate_size(current_free_block) > (size + sizeof(free_list)*2 + sizeof(size_t))) { //arbitrarily choosing excess size > 2 headers + a footer
            split(current_free_block, size);
        }

        remove_from_free_list(current_free_block);
        set_allocated(current_free_block, true);
        set_previous_allocated(find_next_in_memory(current_free_block), true);

        return current_free_block;
    } else if ((current_free_block->body.links.next)) {
        return find_block(current_free_block->body.links.next, size);
    } else {
        return create_block_at_end_of_heap(size); // this needs to use setup_free_block
    }
}

// merges free_block with whichever of its two neighbours in memory are also free, and marks the result free.
static free_list* coalesce(free_list* free_block) { // coalesce will always be called on a block that has just been turned from allocated to free so it will not be linked into the free list

    assert(!is_allocated(free_block));

    size_t merged_size = calculate_size(free_block);

    free_list* next_in_memory = find_next_in_memory(free_block);
    if (!is_allocated(next_in_memory)) {
        remove_from_free_list(next_in_memory);
        merged_size += calculate_size(next_in_memory);
    }

    if (!previous_is_allocated(free_block)) {
        free_list* prev_in_memory = find_prev_in_memory(free_block);
        remove_from_free_list(prev_in_memory);
        merged_size += calculate_size(prev_in_memory);
        free_block = prev_in_memory;
    }

    setup_free_block(free_block, merged_size, false, true);
    return free_block;
}

void* mm_malloc(size_t size) {
    size_t total_size = (offsetof(free_list, body) + size + 31) & ~(size_t)31;
    free_list* chosen_block;

    if (!program_free_list) {
        if (!heap_boundary_markers_initialized) {
            initialize_prologue_and_epilogue();
        }
        chosen_block = create_block_at_end_of_heap(total_size);
    } else {
        chosen_block = find_block(program_free_list, total_size);
    }

    if (!chosen_block) // grow_heap failed
        return nullptr;

    return (void *)(chosen_block->body.mem_block);
}

void* mm_calloc(size_t n, size_t size) {
    void* mem_ptr;
    size_t mul;
    if (ckd_mul(&mul, n, size)) {
        errno = ERANGE;
        perror("Integer Overflow");
        return nullptr;
    } else if (!(mem_ptr = mm_malloc(mul))) {
        return nullptr;
    }
    return memset(mem_ptr, 0, mul);
}

/*
void* mm_realloc(void* ptr, size_t size) {
  //TODO: Implement realloc

  return nullptr;
}
*/

void mm_free(void* ptr) {
    if (!ptr) {
        return;
    }

    free_list* current_free_block = block_of(ptr);

    assert(is_allocated(current_free_block));

    set_allocated(current_free_block, false);
    set_footer(current_free_block);

    current_free_block = coalesce(current_free_block); // coalesce returns the merged block, whose base moves backwards if the previous block was absorbed
    set_previous_allocated(find_next_in_memory(current_free_block), false);

    current_free_block->body.links.next = program_free_list;
    current_free_block->body.links.prev = nullptr;
    if (program_free_list) // program_free_list is null after the initial heap setup, and again whenever the last free block is taken
        program_free_list->body.links.prev = current_free_block;
    program_free_list = current_free_block;

    return;
}


//TODO: Beef up unit tests
//TODO: focus on optimization. Mainly in design instead of implementation (at the moment)
