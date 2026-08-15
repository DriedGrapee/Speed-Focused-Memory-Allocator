/*
 * mm_alloc.c
 */

#include "mm_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <stdckdint.h>

#define ALIGNMENT 8
/*
 * Basic Program Structure: 
 * Initialize a default (empty) free list on malloc call 
 * move brk ptr to accomodate new memory allocation (investigate whether its redundant to use sbrk and mmap)
 * use mmap to get space in the heap
 * update free list to tell user that space has been allocated
 * return pointer to mem_block
 * 
 * Will start with immediate coalescence then add differed coalescence later
 */

static char stdout_buf[BUFSIZ];

typedef struct free_list free_list;
struct free_list {
    size_t header;  // size of the entire block including header and footer (i.e. adding size to a pointer to the block will be the pointer to the next block) 
                    // the LSB of header is whether or not it is allocated, the next LSB is whether or not the previous block in memory is allocated
    union {
        struct { free_list* prev; free_list* next; } links;
        unsigned char mem_block[];
    } body;
};

static inline size_t calculate_size(free_list* free_block) {
    return (free_block->header & ~(size_t)3);
}
static inline bool is_allocated(free_list* free_block) {
    return (free_block->header & (size_t)1);
}
static inline bool previous_is_allocated(free_list* free_block) {
    return (free_block->header & (size_t)(1 << 1));
}
static inline void set_previous_allocated(free_list* free_block, bool previous_allocated) {
    free_block->header = ((free_block->header & ~(size_t)(1 << 1)) | (previous_allocated << 1));
}
static inline void set_allocated(free_list* free_block, bool allocated) {
    free_block->header = ((free_block->header & ~1) | allocated);
}
static inline void set_footer(free_list* free_block) {
    *((size_t *)((unsigned char*)free_block + (calculate_size(free_block)-sizeof(size_t)))) = calculate_size(free_block); // replaces the last sizeof(size_t) bytes with the size of the free_list
}
static inline free_list* find_prev_in_memory(free_list* free_block) {
    return (free_list *)((unsigned char *)free_block - *(size_t *)((unsigned char *)free_block - sizeof(size_t)));
}
static inline free_list* find_next_in_memory(free_list* free_block) {
    return (free_list *)((unsigned char *)free_block + calculate_size(free_block));
}

static void setup_free_block(free_list* free_block, size_t total_size, bool allocated, bool previous_allocated) {
    free_block->header = total_size;

    set_allocated(free_block, allocated);
    set_previous_allocated(free_block, previous_allocated);
    
    if(!allocated) 
       set_footer(free_block); 

    return;
}

static free_list* program_free_list;
static bool heap_boundary_markers_initialized = false;

// FOR TESTING
static free_list* prologue_ptr;
static free_list* epilogue_ptr;

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

    //TESTING
    epilogue_ptr = free_block;
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

// puts the prologue and epiloge blocks 
static void initialize_prologue_and_epilogue(void) {
    setvbuf(stdout, stdout_buf, _IOFBF, sizeof stdout_buf); // necessary when testing as printf uses glibc malloc

    free_list* prologue;

    if (!(prologue = (free_list *)((unsigned char *)grow_heap(2*sizeof(free_list))))) {
       errno = ENOMEM;
       perror("Out Of Memory Therefore Cannot Organize Heap"); 
    } else {
        prologue = (free_list *)((unsigned char*)prologue + ALIGNMENT); // need space for 1 ALIGNMENT + 1 Header + 1 Footer + 16byte padding + 1 Header
    }
    
    // TESTING
    prologue_ptr = prologue;
    
    setup_free_block(prologue, 32, true, false); // sets up prologue
    add_epilogue((free_list *)((unsigned char*)prologue + 4*ALIGNMENT));
    heap_boundary_markers_initialized = true;
    
    return;
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

// FOR TESTING
static void print_block_data(free_list* free_block) {
    /*
     * Want to know:
     * Allocated
     * Size
     * Previous is Allocated
     * If free:
     * footer
     * prev
     * next
     * If Allocated:
     * data
     */
    size_t block_size = calculate_size(free_block);

    if (is_allocated(free_block)) {
        printf("The block at %p is:\nAllocated = 1\nSize = %zu\nPrevious Allocated = %d\n", (void *)free_block, block_size, previous_is_allocated(free_block));
    } else {
        printf("The block at %p is:\nAllocated = 0\nSize = %zu\nPrevious Allocated = %d\n", (void *)free_block, block_size, previous_is_allocated(free_block));
        if (block_size >= sizeof(size_t)) // a corrupt size below one footer would make the offset underflow
            printf("Footer = %zu\n", *((size_t *)((unsigned char*)free_block + (block_size - sizeof(size_t)))));
        printf("Prev = %p\nNext = %p\n", (void *)free_block->body.links.prev, (void *)free_block->body.links.next);
    }
}

// the heap walk alone cannot show list membership, so walk the links too. bounded because
// a corrupted list is usually cyclic, and flagging blocks marked allocated because the list
// is only ever supposed to hold free ones
static void print_free_list(void) {
    printf("Free list:\n");

    unsigned char* heap_low = (unsigned char *)prologue_ptr;
    unsigned char* heap_high = (unsigned char *)sbrk(0);

    free_list* iter_ptr = program_free_list;
    for (int i = 0; iter_ptr && i < 64; i++) {
        // a link pointing outside the heap is exactly the corruption this is meant to show,
        // so report it rather than dereferencing it
        if ((unsigned char *)iter_ptr < heap_low || (unsigned char *)iter_ptr + sizeof(free_list) > heap_high) {
            printf("  [%d] %p lies outside the heap, stopping\n", i, (void *)iter_ptr);
            return;
        }

        printf("  [%d] %p size = %zu%s\n", i, (void *)iter_ptr, calculate_size(iter_ptr),
               is_allocated(iter_ptr) ? "  <-- marked ALLOCATED" : "");
        iter_ptr = iter_ptr->body.links.next;
    }

    if (iter_ptr)
        printf("  stopped after 64 nodes, list is cyclic\n");
}

static void print_heap_data(void) {
    if (!prologue_ptr || !epilogue_ptr) { // both are null until initialize_free_list runs
        printf("=== heap not initialized yet ===\n\n");
        fflush(stdout);
        return;
    }

    printf("=== heap [%p .. %p)  free list head = %p ===\n", (void *)prologue_ptr, (void *)epilogue_ptr, (void *)program_free_list);

    unsigned char* heap_end = (unsigned char *)epilogue_ptr;
    free_list* iter_ptr = prologue_ptr;
    bool reached_epilogue = false;

    // '<' rather than '!=': if the block sizes do not tile the heap exactly the walk
    // steps over the epilogue, and an equality test would run off the end of the heap
    for (int i = 0; i < 64; i++) {
        if ((unsigned char *)iter_ptr >= heap_end) {
            reached_epilogue = ((unsigned char *)iter_ptr == heap_end);
            break;
        }

        if (iter_ptr == prologue_ptr)
            printf("Prologue:\n");
        print_block_data(iter_ptr);

        if (calculate_size(iter_ptr) == 0) { // find_next_in_memory would return iter_ptr forever
            printf("size 0 at %p, walk cannot continue\n", (void *)iter_ptr);
            break;
        }
        iter_ptr = find_next_in_memory(iter_ptr);
    }

    if (!reached_epilogue) // the block sizes do not tile the heap
        printf("walk stopped at %p, epilogue is at %p\n", (void *)iter_ptr, (void *)epilogue_ptr);

    // printed from epilogue_ptr rather than iter_ptr so it shows even when the walk went astray
    printf("Epilogue:\n");
    print_block_data(epilogue_ptr);

    print_free_list();

    printf("\n");
    fflush(stdout); // the caller usually crashes right after this, and buffered output would be lost
}

void* mm_malloc(size_t size) {
    size_t total_size = (offsetof(free_list, body) + size + 31) & ~(size_t)31;
    free_list* chosen_block; 

    printf("Allocating %zu bytes of data into %zu bytes of memory ", size, total_size);
    
    if (!program_free_list) {
        if (!heap_boundary_markers_initialized) {
            initialize_prologue_and_epilogue();
        } 
        chosen_block = create_block_at_end_of_heap(total_size);
    } else {
        chosen_block = find_block(program_free_list, total_size);
    }

    printf("at %p\n", chosen_block);
    
    print_heap_data();

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

void mm_free(void* ptr) { // implement boundary tags for coalescing which should be done on every free (and therefore doesn't need to be recursive)
    if (!ptr) {
        return;
    }

    free_list* current_free_block = (free_list *)((unsigned char *)ptr - offsetof(free_list, body));
    
    printf("Freeing %p\n", current_free_block); // logging
   
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

    print_heap_data(); // logging
    
    return;
}


//TODO: Proper visualization of the heap with reference to the free list. Proper testing suite
//TODO: Once the (likely to exist) bugs are ironed out focus on optimization. Mainly in design instead of implementation (at the moment)