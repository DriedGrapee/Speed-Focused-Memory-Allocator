/*
 * mm_alloc.c
 */

#include "mm_alloc.h"
#include "internal_mm_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

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
	size_t size;
	free_list* prev;
	free_list* next;
	bool free;
	alignas(max_align_t) unsigned char mem_block[];
};

static free_list* program_free_list;
static bool free_list_is_initialized = false;

static void* grow_heap(size_t size) {
    void* heap_ptr;
    if ((heap_ptr = sbrk((size + 31) & ~(size_t)31) == (void *)-1) { // forces size to the next greatest multiple of 32 (unless it is already a mult of 32)
        perror("Error No Memory");
        return nullptr;
    }

    return heap_ptr;
}

static void* new_free_block(size_t size) {
    void* heap_ptr = grow_heap(size);

    
}

static void init_free_list_values(free_list* free_l) {
    free_l->prev = nullptr;
    free_l->next = nullptr;
    free_l->free = false;
}

free_list* create_and_insert_free_block(size_t size) {
    free_list* heap_ptr = grow_heap(sizeof(free_list) + size);
    *heap_ptr = (free_list) {
        .prev = nullptr,
        .next = program_free_list,
        .size = size,
        .free = true,
    };

    program_free_list->prev = heap_ptr;

    return heap_ptr;
}

static void check_and_split(free_list* current_free_block, size_t size) {// checks if the chosen free block is large enough (after word alignment has room for more than another 2 headers) then splits it
    return;
}

static free_list* find_block(free_list* current_free_block, size_t size) {
    if (current_free_block->free && current_free_block->size <= size) {
        check_and_split(current_free_block, size); 
        return current_free_block;
    } else if ((current_free_block->next)) {
        return find_block(current_free_block->next, size);
    } else {
        return create_and_insert_free_block(size);
    }
}

void* mm_malloc(size_t size) {
    if (!free_list_is_initialized) {
        if (!(program_free_list = (free_list*) grow_heap(sizeof(free_list) + size))) {
            perror("Error Upon Initializing Free List");
            exit(1);
        }
        init_free_list_values(program_free_list);
        program_free_list->size = size;
        
        free_list_is_initialized = true;
        
        return (void *) (program_free_list->mem_block);
    } else {
        // search for location
        free_list* chosen_block = find_block(program_free_list, size);
        chosen_block->free = false;

        return (void *) (chosen_block->mem_block);
    }
  return nullptr;
}

void* mm_calloc(size_t size) {
    // call malloc
    // just memset to all 0s (vectorization is already done within memset)
}

void* mm_realloc(void* ptr, size_t size) {
  //TODO: Implement realloc

  return NULL;
}

void mm_free(void* ptr) { // implement boundary tags for coalescing which should be done on every free (and therefore doesn't need to be recursive)
  //TODO: Implement free
}
