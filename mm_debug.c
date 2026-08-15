/*
 * mm_debug.c
 *
 * Direct-linked debug harness. Includes mm_alloc.c outright rather than going
 * through dlopen, so breakpoints bind at gdb startup and file-static state
 * (program_free_list, free_list_is_initialized) is visible from the test binary.
 *
 * mm_test.c remains the dlopen harness; it is what checks that hw3lib.so builds
 * and exports the right symbols. Use this one for debugging, that one for fidelity.
 *
 *   make mm_debug && gdb ./mm_debug
 */

#include "mm_alloc.c" // not mm_alloc.h: the definitions must land in this translation unit

#include <assert.h>
#include <stdio.h>

int main(void) {
    
    int* data = mm_malloc(3*sizeof(int));
    assert(data != NULL);

    float* flute = mm_malloc(sizeof(float));

    
    
    mm_free(flute);
    
    long* loud = mm_malloc(sizeof(long));

    unsigned long* ul_array = mm_calloc(599999, sizeof(unsigned long));

    mm_free(ul_array);
    
    *loud = 1231241234123154;
    
    mm_free(data);
    mm_free(loud);
    
    puts("malloc test successful!");
}
