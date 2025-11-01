#include "gdsl/diff.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_pattern(uint8_t *buffer, size_t length, uint8_t seed) {
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (uint8_t)(seed + (uint8_t)i * 17u);
    }
}

static void test_diff_roundtrip(void) {
    const size_t base_length = 8192;
    const size_t target_length = 8192;

    uint8_t *base = (uint8_t *)malloc(base_length);
    uint8_t *target = (uint8_t *)malloc(target_length);
    assert(base && target);

    memset(base, 0, base_length);
    memset(target, 0, target_length);

    fill_pattern(base, base_length, 1);
    memcpy(target, base, base_length);

    /* Modify two pages */
    fill_pattern(target + 1024, 128, 42);
    fill_pattern(target + 4096, 4096, 9);

    gdsl_diff_result_t diff;
    int rc = gdsl_diff(base, base_length, target, target_length, &diff);
    assert(rc == 0);
    printf("diff: chunks=%zu payload=%zu target_length=%llu\n",
           diff.chunk_count,
           diff.payload_length,
           (unsigned long long)diff.header.target_length);
    assert(diff.chunk_count >= 1);

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    rc = gdsl_patch(base, base_length, &diff, &patched, &patched_length);
    assert(rc == 0);
    assert(patched_length >= target_length);
    assert(memcmp(patched, target, target_length) == 0);

    size_t changed_pages[8];
    size_t changed_count = 0;
    rc = gdsl_read_changed_set(&diff, changed_pages, 8, &changed_count);
    assert(rc == 0);
    assert(changed_count == diff.chunk_count);

    free(patched);
    gdsl_diff_result_destroy(&diff);
    free(base);
    free(target);
}

static void test_diff_handles_shrinking(void) {
    const size_t base_length = 8192;
    const size_t target_length = 2048;

    uint8_t *base = (uint8_t *)malloc(base_length);
    uint8_t *target = (uint8_t *)malloc(target_length);
    assert(base && target);

    memset(base, 7, base_length);
    memset(target, 3, target_length);

    gdsl_diff_result_t diff;
    int rc = gdsl_diff(base, base_length, target, target_length, &diff);
    assert(rc == 0);

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    rc = gdsl_patch(base, base_length, &diff, &patched, &patched_length);
    assert(rc == 0);
    assert(patched_length >= target_length);
    assert(memcmp(patched, target, target_length) == 0);

    free(patched);
    gdsl_diff_result_destroy(&diff);
    free(base);
    free(target);
}

int main(void) {
    test_diff_roundtrip();
    test_diff_handles_shrinking();
    puts("All diff tests completed.");
    return 0;
}
