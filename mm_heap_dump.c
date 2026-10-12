/*
 * mm_heap_dump.c
 */

#include "mm_heap_dump.h"

#include <stdio.h>
#include <unistd.h>

void print_block_data(free_list* free_block) {
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
void print_free_list(void) {
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

void print_heap_data(void) {
    if (!prologue_ptr || !epilogue_ptr) { // both are null until the first mm_malloc sets up the heap
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
