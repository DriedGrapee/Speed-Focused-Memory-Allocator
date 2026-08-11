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


/* If I wanted this to work on 32bit machines I would have to change the default alignment
static bool is32bit = ...;
typedef (is32bit ? uint8_t : uint16_t) Align;
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

static size_t calculate_size(free_list* free_block) {
    return (free_block->header & ~3);
}

static inline void set_footer(free_list* free_block) {
    *((size_t *)((unsigned char*)free_block + (calculate_size(free_block)-sizeof(size_t)))) = calculate_size(free_block); // replaces the last sizeof(size_t) bytes with the size of the free_list
}
static inline void set_previous_allocated(free_list* free_block, bool previous_allocated) {
    free_block->header = ((free_block->header & ~(1 << 1)) | (previous_allocated << 1));
}
static inline void set_allocated(free_list* free_block, bool allocated) {
    free_block->header = ((free_block->header & ~1) | allocated);
}

static void setup_free_block(free_list* free_block, size_t total_size, bool allocated, bool previous_allocated) {
    free_block->header = total_size;

    set_allocated(free_block, allocated);
    set_previous_allocated(free_block, previous_allocated);
    
    if(!allocated) 
       set_footer(free_block); 

    return;
}

/* Will be used when freeing

static inline bool is_allocated(free_list* free_block) {
    return (free_block->header & (size_t)1);
}

static inline bool previous_is_allocated(free_list* free_block) {
    return (free_block->header & (size_t)(1 << 1));
}

*/

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

free_list* create_and_insert_free_block(size_t size) {
    free_list* heap_ptr = grow_heap(size);
    if (heap_ptr) {
        setup_free_block(heap_ptr, size, true, false); //previous_allocated is false because *prev is nullptr for the first entry in the list
    } else {
        
    }
    
    return heap_ptr;
}

static void split(free_list* current_free_block, size_t size_of_first_block) {// checks if the chosen free block is large enough (after word alignment has room for more than another 2 headers) then splits it

    assert(current_free_block);
    
    free_list* new_free_block = current_free_block + size_of_first_block;
    *new_free_block = (free_list) {};
    setup_free_block(new_free_block, calculate_size(current_free_block) - size_of_first_block, false, true);

    new_free_block->body.links.next = current_free_block->body.links.next;
    new_free_block->body.links.prev = current_free_block;
    current_free_block->body.links.next = new_free_block;
    
    return;
}

static void remove_from_free_list(free_list* free_block) {
    
    assert(free_block);
    
    (free_block->body.links.prev)->body.links.next = free_block->body.links.next;
    (free_block->body.links.next)->body.links.prev = free_block->body.links.prev;
    
    set_allocated(free_block, true);

    return;
}

static free_list* find_block(free_list* current_free_block, size_t size) {

    assert(current_free_block);
    assert(size >= 0);
    
    if (calculate_size(current_free_block) >= size) {
        if (calculate_size(current_free_block) > (size + 2*ALIGNMENT)) {
            split(current_free_block, size);
        }
        remove_from_free_list(current_free_block);
        
        return current_free_block;
    } else if ((current_free_block->body.links.next)) {
        return find_block(current_free_block->body.links.next, size);
    } else {
        return create_and_insert_free_block(size); // this needs to use setup_free_block
    }
}

void* mm_malloc(size_t size) {
    size_t total_size = ALIGNMENT + sizeof(free_list) + ((size + 31) & ~(size_t)31);
    free_list* chosen_block; 
    
    if (!free_list_is_initialized) {
        if (!(chosen_block = grow_heap(total_size) + ALIGNMENT)) {
            perror("Error Upon Initializing Free List");
            exit(1);
        }
        setup_free_block(chosen_block, total_size, true, false);
        
        free_list_is_initialized = true;
    } else {
        // search for location
        chosen_block = find_block(program_free_list, total_size);
    }
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

void mm_free(void* ptr) { // implement boundary tags for coalescing which should be done on every free (and therefore doesn't need to be recursive)
  //TODO: Implement free
  return;
}
*/