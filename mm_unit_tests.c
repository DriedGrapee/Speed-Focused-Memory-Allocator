/*
 * munit forks before each test, giving every test a fresh copy of the heap (the
 * allocator has no reset) and reporting a segfault as a failure.
 * To add a test: write a test_* function and add a row to tests[].
 */
#include "mm_alloc.h"
#include "mm_alloc_internal.h"
#include "munit.h"

#include <stdio.h>

static MunitResult test_malloc_returns_nonnull(const MunitParameter p[], void* f) {
    (void)p; (void)f;
    void* ptr = mm_malloc(32);
    munit_assert_not_null(ptr);
    mm_free(ptr);
    return MUNIT_OK;
}

// max_align_t is 16 bytes on x86-64
static MunitResult test_malloc_payload_alignment(const MunitParameter p[], void* f) {
    (void)p; (void)f;
    for (size_t size = 1; size <= 128; size++) {
        void* ptr = mm_malloc(size);
        munit_assert_not_null(ptr);
        munit_assert_size((uintptr_t)ptr % 16, ==, 0);
        mm_free(ptr);
    }
    return MUNIT_OK;
}

static MunitResult test_malloc_block_is_allocated(const MunitParameter p[], void* f) {
    (void)p; (void)f;
    size_t req = 40;
    void* ptr = mm_malloc(req);
    munit_assert_not_null(ptr);
    free_list* block = block_of(ptr);
    munit_assert_true(is_allocated(block));
    munit_assert_size(calculate_size(block), >=, req + offsetof(free_list, body));
    mm_free(ptr);
    return MUNIT_OK;
}

// the freed block should come back from the free list, not from heap growth
static MunitResult test_free_then_malloc_reuses_block(const MunitParameter p[], void* f) {
    (void)p; (void)f;
    void* first = mm_malloc(64);
    munit_assert_not_null(first);
    free_list* first_block = block_of(first);
    mm_free(first);

    void* second = mm_malloc(64);
    munit_assert_not_null(second);
    munit_assert_ptr_equal(block_of(second), first_block);
    mm_free(second);
    return MUNIT_OK;
}

static MunitTest tests[] = {
    { (char*)"/malloc/returns-nonnull",    test_malloc_returns_nonnull,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*)"/malloc/payload-alignment",  test_malloc_payload_alignment,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*)"/malloc/block-is-allocated", test_malloc_block_is_allocated,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*)"/free/reuses-block",         test_free_then_malloc_reuses_block, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } // terminator
};

static const MunitSuite suite = {
    (char*)"/mm_alloc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

static char stdout_buf[BUFSIZ];

int main(int argc, char* argv[]) {
    // static so printf never calls glibc malloc and moves the break; line buffered
    // so each test is reported as it finishes
    setvbuf(stdout, stdout_buf, _IOLBF, sizeof stdout_buf);
    return munit_suite_main(&suite, NULL, argc, argv);
}
