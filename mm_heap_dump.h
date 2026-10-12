/* Heap and free-list dumps for debug builds; `call print_heap_data()` from gdb. */
#pragma once

#include "mm_alloc_internal.h"

void print_block_data(free_list* free_block);
void print_free_list(void);
void print_heap_data(void);
