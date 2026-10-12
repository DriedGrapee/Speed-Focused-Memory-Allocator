/* Direct-linked harness for gdb, ASan and valgrind. mm_test.c is the dlopen one. */
#include "mm_alloc.h"

#include <assert.h>
#include <stdio.h>

static char stdout_buf[BUFSIZ];

int main(void) {
    // a static stdout buffer, so printf never calls glibc malloc and moves the break
    setvbuf(stdout, stdout_buf, _IOFBF, sizeof stdout_buf);

    int* data = mm_malloc(3 * sizeof(int));
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
