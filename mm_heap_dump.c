#include "mm_heap_dump.h"

#include <stdio.h>
#include <unistd.h>

void print_block_data(free_list* free_block) {
    size_t sz = calculate_size(free_block);
    int alloc = is_allocated(free_block);
    printf("The block at %p is:\nAllocated = %d\nSize = %zu\nPrevious Allocated = %d\n",
           (void *)free_block, alloc, sz, previous_is_allocated(free_block));
    if (!alloc) {
        if (sz >= sizeof(size_t)) // a corrupt size would underflow the footer offset
            printf("Footer = %zu\n", *(size_t *)((unsigned char *)free_block + sz - sizeof(size_t)));
        printf("Prev = %p\nNext = %p\n", (void *)free_block->body.links.prev, (void *)free_block->body.links.next);
    }
}

// Bounded, since a corrupt list is usually cyclic. Flags links that leave the heap
// rather than following them.
void print_free_list(void) {
    printf("Free list:\n");
    unsigned char* heap_low = (unsigned char *)prologue_ptr;
    unsigned char* heap_high = (unsigned char *)sbrk(0);

    free_list* iter = program_free_list;
    for (int i = 0; iter && i < 64; i++) {
        if ((unsigned char *)iter < heap_low || (unsigned char *)iter + sizeof(free_list) > heap_high) {
            printf("  [%d] %p lies outside the heap, stopping\n", i, (void *)iter);
            return;
        }
        printf("  [%d] %p size = %zu%s\n", i, (void *)iter, calculate_size(iter),
               is_allocated(iter) ? "  <-- marked ALLOCATED" : "");
        iter = iter->body.links.next;
    }
    if (iter)
        printf("  stopped after 64 nodes, list is cyclic\n");
}

void print_heap_data(void) {
    if (!prologue_ptr || !epilogue_ptr) {
        printf("=== heap not initialized yet ===\n\n");
        fflush(stdout);
        return;
    }

    printf("=== heap [%p .. %p)  free list head = %p ===\n",
           (void *)prologue_ptr, (void *)epilogue_ptr, (void *)program_free_list);

    unsigned char* heap_end = (unsigned char *)epilogue_ptr;
    free_list* iter = prologue_ptr;
    bool reached_epilogue = false;

    // >= rather than ==: if the sizes do not tile the heap, the walk steps past the epilogue
    for (int i = 0; i < 64; i++) {
        if ((unsigned char *)iter >= heap_end) {
            reached_epilogue = ((unsigned char *)iter == heap_end);
            break;
        }
        if (iter == prologue_ptr)
            printf("Prologue:\n");
        print_block_data(iter);

        if (calculate_size(iter) == 0) { // would loop forever
            printf("size 0 at %p, walk cannot continue\n", (void *)iter);
            break;
        }
        iter = find_next_in_memory(iter);
    }

    if (!reached_epilogue)
        printf("walk stopped at %p, epilogue is at %p\n", (void *)iter, (void *)epilogue_ptr);

    printf("Epilogue:\n");
    print_block_data(epilogue_ptr);
    print_free_list();
    printf("\n");
    fflush(stdout); // the caller often crashes next, losing buffered output
}
