/*
 * mm_alloc_internal.h
 *
 * Block layout and heap state, shared by the allocator and its tooling. Programs
 * that only allocate include mm_alloc.h.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALIGNMENT 8

// header: the whole block's size (a multiple of 32), with bit 0 = allocated and
// bit 1 = previous block in memory is allocated. Free blocks also hold their list
// links and, in their last 8 bytes, a footer copy of the size.
typedef struct free_list free_list;
struct free_list {
    size_t header;
    union {
        struct { free_list* prev; free_list* next; } links;
        unsigned char mem_block[];
    } body;
};

extern free_list* program_free_list;  // null when no block is free
extern free_list* prologue_ptr;       // heap bounds, null until the first mm_malloc
extern free_list* epilogue_ptr;

static inline size_t calculate_size(free_list* block) {
    return block->header & ~(size_t)3;
}
static inline bool is_allocated(free_list* block) {
    return block->header & (size_t)1;
}
static inline bool previous_is_allocated(free_list* block) {
    return block->header & (size_t)2;
}
static inline void set_previous_allocated(free_list* block, bool previous_allocated) {
    block->header = (block->header & ~(size_t)2) | ((size_t)previous_allocated << 1);
}
static inline void set_allocated(free_list* block, bool allocated) {
    block->header = (block->header & ~(size_t)1) | allocated;
}
static inline void set_footer(free_list* block) {
    *(size_t *)((unsigned char *)block + calculate_size(block) - sizeof(size_t)) = calculate_size(block);
}
// reads the previous block's footer, so only valid when that block is free
static inline free_list* find_prev_in_memory(free_list* block) {
    return (free_list *)((unsigned char *)block - *(size_t *)((unsigned char *)block - sizeof(size_t)));
}
static inline free_list* find_next_in_memory(free_list* block) {
    return (free_list *)((unsigned char *)block + calculate_size(block));
}
// the block header behind a pointer mm_malloc returned
static inline free_list* block_of(void* payload) {
    return (free_list *)((unsigned char *)payload - offsetof(free_list, body));
}
