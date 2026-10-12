/*
 * mm_alloc_internal.h
 *
 * Block layout and heap state shared between the allocator and the tooling built
 * around it (heap dumps, the valgrind wrapper, unit tests, benchmarks). Not part
 * of the public interface -- programs that only allocate include mm_alloc.h.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALIGNMENT 8

typedef struct free_list free_list;
struct free_list {
    size_t header;  // size of the entire block including header and footer (i.e. adding size to a pointer to the block will be the pointer to the next block)
                    // the LSB of header is whether or not it is allocated, the next LSB is whether or not the previous block in memory is allocated
    union {
        struct { free_list* prev; free_list* next; } links;
        unsigned char mem_block[];
    } body;
};

// head of the explicit free list, null whenever no block is free
extern free_list* program_free_list;
// first and last blocks of the heap, null until the first mm_malloc; a heap walk runs from one to the other
extern free_list* prologue_ptr;
extern free_list* epilogue_ptr;

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
// recovers the block header from a pointer mm_malloc handed back
static inline free_list* block_of(void* payload) {
    return (free_list *)((unsigned char *)payload - offsetof(free_list, body));
}
