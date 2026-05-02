#include "gdsl/diff.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_pattern(uint8_t *buffer, size_t length, uint8_t seed) {
    for (size_t i = 0; i < length; ++i) {
        buffer[i] = (uint8_t)(seed + (uint8_t)i * 17u);
    }
}

static void assert_zeroed_result(const gdsl_diff_result_t *diff) {
    assert(diff->header.version == 0);
    assert(diff->header.page_size == 0);
    assert(diff->header.flags == 0);
    assert(diff->header.chunk_count == 0);
    assert(diff->header.target_length == 0);
    assert(diff->chunk_count == 0);
    assert(diff->chunks == NULL);
    assert(diff->payload == NULL);
    assert(diff->payload_length == 0);
}

static void test_diff_roundtrip(void) {
    const size_t base_length = 8192;
    const size_t target_length = 8192;

    uint8_t *base = (uint8_t *)malloc(base_length);
    uint8_t *target = (uint8_t *)malloc(target_length);
    assert(base && target);

    fill_pattern(base, base_length, 1);
    memcpy(target, base, target_length);

    fill_pattern(target + 1024, 128, 42);
    fill_pattern(target + 4096, 4096, 9);

    gdsl_diff_result_t diff = {0};
    int rc = gdsl_diff(base, base_length, target, target_length, &diff);
    assert(rc == 0);
    assert(diff.chunk_count >= 1);

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    rc = gdsl_patch(base, base_length, &diff, &patched, &patched_length);
    assert(rc == 0);
    assert(patched_length == target_length);
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


static void test_diff_reuses_result_storage(void) {
    const size_t length = 4096;
    uint8_t *base = (uint8_t *)malloc(length);
    uint8_t *target1 = (uint8_t *)malloc(length);
    uint8_t *target2 = (uint8_t *)malloc(length);
    assert(base && target1 && target2);

    fill_pattern(base, length, 5);
    memcpy(target1, base, length);
    memcpy(target2, base, length);
    target1[0] ^= 0x5A;
    target2[length - 1] ^= 0xA5;

    gdsl_diff_result_t diff = {0};
    int rc = gdsl_diff(base, length, target1, length, &diff);
    assert(rc == 0);
    assert(diff.chunk_count >= 1);

    rc = gdsl_diff(base, length, target2, length, &diff);
    assert(rc == 0);
    assert(diff.chunk_count >= 1);

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    rc = gdsl_patch(base, length, &diff, &patched, &patched_length);
    assert(rc == 0);
    assert(patched_length == length);
    assert(memcmp(patched, target2, length) == 0);

    free(patched);
    gdsl_diff_result_destroy(&diff);
    free(base);
    free(target1);
    free(target2);
}

static void test_diff_handles_shrinking(void) {
    const size_t base_length = 8192;
    const size_t target_length = 2048;

    uint8_t *base = (uint8_t *)malloc(base_length);
    uint8_t *target = (uint8_t *)malloc(target_length);
    assert(base && target);

    memset(base, 7, base_length);
    memset(target, 3, target_length);

    gdsl_diff_result_t diff = {0};
    int rc = gdsl_diff(base, base_length, target, target_length, &diff);
    assert(rc == 0);

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    rc = gdsl_patch(base, base_length, &diff, &patched, &patched_length);
    assert(rc == 0);
    assert(patched_length == target_length);
    assert(memcmp(patched, target, target_length) == 0);

    free(patched);
    gdsl_diff_result_destroy(&diff);
    free(base);
    free(target);
}

static void test_diff_rejects_excessive_chunks(void) {
    const size_t page_size = 4096;
    const size_t page_count = 3;
    const size_t length = page_size * page_count;

    uint8_t *base = (uint8_t *)calloc(length, 1);
    uint8_t *target = (uint8_t *)malloc(length);
    assert(base && target);
    memset(target, 1, length);

    assert(setenv("GDSL_DIFF_CHUNK_LIMIT", "2", 1) == 0);

    gdsl_diff_result_t diff = {0};
    int rc = gdsl_diff(base, length, target, length, &diff);
    assert(rc != 0);
    assert(diff.chunk_count == 0);
    assert(diff.header.chunk_count == 0);
    assert(diff.payload == NULL);
    assert(diff.payload_length == 0);
    assert(diff.chunks == NULL);

    unsetenv("GDSL_DIFF_CHUNK_LIMIT");
    free(base);
    free(target);
}

static void test_diff_rejects_seeded_chunk_count_overflow(void) {
    uint8_t base[1] = {0};
    uint8_t target[1] = {1};
    gdsl_diff_result_t diff = {0};

    char seed[32];
    snprintf(seed, sizeof(seed), "%zu", SIZE_MAX);
    assert(setenv("GDSL_DIFF_CHUNK_COUNT_SEED", seed, 1) == 0);

    int rc = gdsl_diff(base, sizeof(base), target, sizeof(target), &diff);
    assert(rc == -1);
    assert_zeroed_result(&diff);

    unsetenv("GDSL_DIFF_CHUNK_COUNT_SEED");
}

static void test_diff_rejects_seeded_payload_overflow(void) {
    uint8_t base[1] = {0};
    uint8_t target[1] = {1};
    gdsl_diff_result_t diff = {0};

    char seed[32];
    snprintf(seed, sizeof(seed), "%zu", SIZE_MAX);
    assert(setenv("GDSL_DIFF_PAYLOAD_SIZE_SEED", seed, 1) == 0);

    int rc = gdsl_diff(base, sizeof(base), target, sizeof(target), &diff);
    assert(rc == -1);
    assert_zeroed_result(&diff);

    unsetenv("GDSL_DIFF_PAYLOAD_SIZE_SEED");
}

static void test_diff_rejects_chunk_array_size_overflow(void) {
    uint8_t base[1] = {7};
    uint8_t target[1] = {7};
    gdsl_diff_result_t diff = {0};

    const size_t overflow_count = (SIZE_MAX / sizeof(gdsl_diff_chunk_t)) + 1;
    char seed[32];
    snprintf(seed, sizeof(seed), "%zu", overflow_count);
    assert(setenv("GDSL_DIFF_CHUNK_COUNT_SEED", seed, 1) == 0);

    int rc = gdsl_diff(base, sizeof(base), target, sizeof(target), &diff);
    assert(rc == -1);
    assert_zeroed_result(&diff);

    unsetenv("GDSL_DIFF_CHUNK_COUNT_SEED");
}

static void test_rejects_invalid_metadata(void) {
    gdsl_diff_result_t diff = {0};
    memset(&diff, 0, sizeof(diff));

    diff.header.version = GDSL_DIFF_VERSION + 1;
    diff.header.page_size = GDSL_DEFAULT_PAGE_SIZE;
    diff.header.target_length = 1;
    uint8_t *patched = (uint8_t *)0x1;
    size_t patched_length = 123;
    int rc = gdsl_patch(NULL, 0, &diff, &patched, &patched_length);
    assert(rc != 0);
    assert(patched == NULL);
    assert(patched_length == 0);

    memset(&diff, 0, sizeof(diff));
    diff.header.version = GDSL_DIFF_VERSION;
    diff.header.page_size = 0;
    diff.header.target_length = 1;
    patched = (uint8_t *)0x1;
    patched_length = 123;
    rc = gdsl_patch(NULL, 0, &diff, &patched, &patched_length);
    assert(rc != 0);
    assert(patched == NULL);
    assert(patched_length == 0);

    memset(&diff, 0, sizeof(diff));
    diff.header.version = GDSL_DIFF_VERSION;
    diff.header.page_size = GDSL_MAX_PAGE_SIZE + 1;
    diff.header.target_length = 1;
    patched = (uint8_t *)0x1;
    patched_length = 123;
    rc = gdsl_patch(NULL, 0, &diff, &patched, &patched_length);
    assert(rc != 0);
    assert(patched == NULL);
    assert(patched_length == 0);
}

static void test_patch_rejects_non_monotonic_chunks(void) {
    const size_t page_size = 4096;

    gdsl_diff_result_t diff = {0};
    diff.header.version = GDSL_DIFF_VERSION;
    diff.header.page_size = page_size;
    diff.header.target_length = page_size * 2;
    diff.header.chunk_count = 2;
    diff.chunk_count = 2;

    diff.chunks = (gdsl_diff_chunk_t *)malloc(sizeof(gdsl_diff_chunk_t) * 2);
    diff.payload_length = 20;
    diff.payload = (uint8_t *)malloc(diff.payload_length);
    assert(diff.chunks && diff.payload);
    memset(diff.payload, 0xAB, diff.payload_length);

    diff.chunks[0].page_index = 1;
    diff.chunks[0].length = 10;
    diff.chunks[0].data_offset = 0;

    diff.chunks[1].page_index = 1;
    diff.chunks[1].length = 10;
    diff.chunks[1].data_offset = 10;

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    int rc = gdsl_patch(NULL, 0, &diff, &patched, &patched_length);
    assert(rc == -1);

    free(patched);
    free(diff.payload);
    free(diff.chunks);
}

static void test_patch_rejects_overlapping_chunks(void) {
    const size_t page_size = 4096;

    gdsl_diff_result_t diff = {0};
    diff.header.version = GDSL_DIFF_VERSION;
    diff.header.page_size = page_size;
    diff.header.target_length = page_size * 2;
    diff.header.chunk_count = 2;
    diff.chunk_count = 2;

    diff.chunks = (gdsl_diff_chunk_t *)malloc(sizeof(gdsl_diff_chunk_t) * 2);
    diff.payload_length = 5100;
    diff.payload = (uint8_t *)malloc(diff.payload_length);
    assert(diff.chunks && diff.payload);
    memset(diff.payload, 0xCD, diff.payload_length);

    diff.chunks[0].page_index = 0;
    diff.chunks[0].length = 5000;
    diff.chunks[0].data_offset = 0;

    diff.chunks[1].page_index = 1;
    diff.chunks[1].length = 100;
    diff.chunks[1].data_offset = 5000;

    uint8_t *patched = NULL;
    size_t patched_length = 0;
    int rc = gdsl_patch(NULL, 0, &diff, &patched, &patched_length);
    assert(rc == -1);

    free(patched);
    free(diff.payload);
    free(diff.chunks);
}

int main(void) {
    test_diff_roundtrip();
    test_diff_reuses_result_storage();
    test_diff_handles_shrinking();
    test_diff_rejects_excessive_chunks();
    test_diff_rejects_seeded_chunk_count_overflow();
    test_diff_rejects_seeded_payload_overflow();
    test_diff_rejects_chunk_array_size_overflow();
    test_rejects_invalid_metadata();
    test_patch_rejects_non_monotonic_chunks();
    test_patch_rejects_overlapping_chunks();
    puts("All diff tests completed.");
    return 0;
}
