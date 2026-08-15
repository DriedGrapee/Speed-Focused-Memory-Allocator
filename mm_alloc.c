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
    return (free_block->header & ~3);
}
static inline bool is_allocated(free_list* free_block) {
    return (free_block->header & (size_t)1);
}
static inline bool previous_is_allocated(free_list* free_block) {
    return (free_block->header & (size_t)(1 << 1));
}
static inline void set_previous_allocated(free_list* free_block, bool previous_allocated) {
    free_block->header = ((free_block->header & ~(1 << 1)) | (previous_allocated << 1));
}
static inline void set_allocated(free_list* free_block, bool allocated) {
    free_block->header = ((free_block->header & ~1) | allocated);
}
static inline void set_footer(free_list* free_block) {
    *((size_t *)((unsigned char*)free_block + (calculate_size(free_block)-sizeof(size_t)))) = calculate_size(free_block); // replaces the last sizeof(size_t) bytes with the size of the free_list
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
static bool free_list_is_initialized = false;

static void* grow_heap(size_t size) {
    void* heap_ptr;
    if ((heap_ptr = sbrk(size)) == (void *)-1) { // forces size to the next greatest multiple of 32 (unless it is already a mult of 32)
        perror("Error No Memory");
        return nullptr;
    }

    return heap_ptr;
}

static inline void add_epilogue(free_list* free_block) {
    setup_free_block(free_block, 0, true, false); // sets up epilogue
}

static free_list* create_block_at_end_of_heap(size_t size) {
    free_list* heap_ptr = grow_heap(size);
    if (heap_ptr) {
        setup_free_block(heap_ptr, size, true, false); // this is wrong! Previously allocated is unkown until an epilogue for the free list is implemented. 
    } else {
        
    }

    add_epilogue(heap_ptr + size - ALIGNMENT);
    
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
        
        return current_free_block;
    } else if ((current_free_block->body.links.next)) {
        return find_block(current_free_block->body.links.next, size);
    } else {
        return create_block_at_end_of_heap(size); // this needs to use setup_free_block
    }
}

// puts the prologue and epiloge blocks 
static void initialize_free_list(void) {
    free_list* prologue = grow_heap(3*ALIGNMENT);
    setup_free_block(prologue, 8, true, false); // sets up prologue
    add_epilogue((free_list *)((unsigned char*)prologue + 2*ALIGNMENT));

    program_free_list = (free_list *)((unsigned char *)prologue + 2*ALIGNMENT);
    free_list_is_initialized = true;
    
    return;
}

void* mm_malloc(size_t size) {
    size_t total_size = ALIGNMENT + sizeof(free_list) + ((size + 31) & ~(size_t)31);
    free_list* chosen_block; 
    
    if (!free_list_is_initialized) {
        initialize_free_list();
        create_block_at_end_of_heap(total_size);
    }
    if (program_free_list) {
        chosen_block = find_block(program_free_list, total_size);
    } else {
        printf("Free list == nullptr. Retrying freelist initialization...");
        free_list_is_initialized = false;
        return mm_malloc(size);
    }

    if (!chosen_block) // grow_heap failed
        return nullptr;

    return (void *)(chosen_block->body.mem_block);
}

/*
void* mm_calloc(size_t size) {
    // call malloc
    // just memset to all 0s (vectorization is already done within memset)
    return nullptr;
}

void* mm_realloc(void* ptr, size_t size) {
  //TODO: Implement realloc

  return nullptr;
}
*/

static inline free_list* find_prev_in_memory(free_list* free_block) {
    return (free_list *)((unsigned char *)free_block - *(size_t *)((unsigned char *)free_block - sizeof(size_t)));
}

static inline free_list* find_next_in_memory(free_list* free_block) {
    return (free_list *)((unsigned char *)free_block + calculate_size(free_block));
}
// merges free_block with whichever of its two neighbours in memory are also free, and marks the result free.
static free_list* coalesce(free_list* free_block) {

    //assert(!is_allocated(free_block));
    
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

void mm_free(void* ptr) { // implement boundary tags for coalescing which should be done on every free (and therefore doesn't need to be recursive)
    if (!ptr) {
        return;
    }

    free_list* current_free_block = (free_list *)((unsigned char *)ptr - offsetof(free_list, body));
   
    //assert(is_allocated(current_free_block));
    
    current_free_block->body.links.next = program_free_list;
    current_free_block->body.links.prev = nullptr;
    if (program_free_list) // program_free_list is null after the initial heap setup, and again whenever the last free block is taken
        program_free_list->body.links.prev = current_free_block;
    program_free_list = current_free_block;

    set_allocated(current_free_block, true);
    set_footer(current_free_block);
    set_previous_allocated(find_next_in_memory(current_free_block), false);
    
    current_free_block = coalesce(current_free_block); // coalesce returns the merged block, whose base moves backwards if the previous block was absorbed
    
    return;
}


// TODO: Fix all incorrect pointer arithmetic. The above example is correct. Make sure if adding a number of bytes the pointer you are adding to is of type unsigned char.
// TODO: Add prologue and epilogue blocks which are at the beginning and end of the heap memory