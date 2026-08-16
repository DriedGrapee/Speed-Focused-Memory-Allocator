/*
 * mm_unit_tests.c
 *
 * munit forks before each test. the allocator keeps global state on an sbrk heap
 * that only grows and has no reset hook, so without forking every test would inherit
 * the previous test's heap. Each test instead gets a pristine copy-on-write snapshot,
 * and a test that segfaults is reported as a failure rather than killing the run.
 *
 * TO ADD A TEST:
 *   1. write a `static MunitResult test_xyz(const MunitParameter params[], void* fixture)`
 *   2. add one row to the tests[] array below, above the NULL terminator
 */

#include "mm_alloc.c" // not mm_alloc.h: the definitions must land in this translation unit

#include "munit.h"

/* ------------------------------------------------------------------ helpers */

// recovers the block header from a pointer mm_malloc handed back
static free_list* block_of(void* payload) {
    return (free_list*)((unsigned char*)payload - offsetof(free_list, body));
}

/* -------------------------------------------------------------------- tests */

static MunitResult test_malloc_returns_nonnull(const MunitParameter params[], void* fixture) {
    (void)params; (void)fixture; // -Wextra -Werror: both are unused in most tests

    void* p = mm_malloc(32);
    munit_assert_not_null(p);
    mm_free(p);

    return MUNIT_OK;
}

// malloc must return memory aligned for any type; on x86-64 max_align_t is 16
static MunitResult test_malloc_payload_alignment(const MunitParameter params[], void* fixture) {
    (void)params; (void)fixture;

    for (size_t size = 1; size <= 128; size++) {
        void* p = mm_malloc(size);
        munit_assert_not_null(p);
        munit_assert_size((uintptr_t)p % 16, ==, 0);
        mm_free(p);
    }

    return MUNIT_OK;
}

// the block behind a live pointer should be marked allocated and be big enough
// to hold both the header and everything the caller asked for
static MunitResult test_malloc_block_is_allocated(const MunitParameter params[], void* fixture) {
    (void)params; (void)fixture;

    size_t requested = 40;
    void* p = mm_malloc(requested);
    munit_assert_not_null(p);

    free_list* block = block_of(p);
    munit_assert_true(is_allocated(block));
    munit_assert_size(calculate_size(block), >=, requested + offsetof(free_list, body));

    mm_free(p);
    return MUNIT_OK;
}

// a block that has been freed and immediately re-requested at the same size
// should come back from the free list rather than growing the heap
static MunitResult test_free_then_malloc_reuses_block(const MunitParameter params[], void* fixture) {
    (void)params; (void)fixture;

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

/* ------------------------------------------------------- test registration */

static MunitTest tests[] = {
    { (char*)"/malloc/returns-nonnull",     test_malloc_returns_nonnull,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*)"/malloc/payload-alignment",   test_malloc_payload_alignment,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*)"/malloc/block-is-allocated",  test_malloc_block_is_allocated,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*)"/free/reuses-block",          test_free_then_malloc_reuses_block, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },

    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } // terminator, keep last
};

static const MunitSuite suite = {
    (char*)"/mm_alloc",     // prefix for every test name above
    tests,
    NULL,                   // no nested suites
    1,                      // iterations
    MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[]) {
    return munit_suite_main(&suite, NULL, argc, argv);
}
