/*
 * mm_heap_dump.h
 *
 * Human-readable dumps of the allocator's heap and free list. Linked into the
 * debug binaries only; from gdb, `call print_heap_data()` at any stop.
 */

#pragma once

#include "mm_alloc_internal.h"

void print_block_data(free_list* free_block);
void print_free_list(void);
void print_heap_data(void);
