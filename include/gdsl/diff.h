#ifndef GDSL_DIFF_H
#define GDSL_DIFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDSL_DIFF_VERSION 1u
#define GDSL_DEFAULT_PAGE_SIZE 4096u
#define GDSL_MAX_PAGE_SIZE (1u << 20)

typedef struct {
    uint32_t version;
    uint32_t page_size;
    uint32_t flags;
    uint32_t chunk_count;
    uint64_t target_length;
} gdsl_diff_header_t;

typedef struct {
    size_t page_index;
    size_t length;
    size_t data_offset;
} gdsl_diff_chunk_t;

/*
 * Chunks in a diff must be ordered by strictly increasing page_index. When
 * expanded using the diff header's page_size, each chunk describes the byte
 * interval [page_index * page_size, page_index * page_size + length), and
 * these intervals must not overlap.
 */

typedef struct {
    gdsl_diff_header_t header;
    gdsl_diff_chunk_t *chunks;
    size_t chunk_count;
    uint8_t *payload;
    size_t payload_length;
} gdsl_diff_result_t;

void gdsl_diff_result_destroy(gdsl_diff_result_t *result);

int gdsl_diff(const uint8_t *base,
              size_t base_length,
              const uint8_t *target,
              size_t target_length,
              gdsl_diff_result_t *out);

int gdsl_patch(const uint8_t *base,
               size_t base_length,
               const gdsl_diff_result_t *diff,
               uint8_t **out_buffer,
               size_t *out_length);

int gdsl_read_changed_set(const gdsl_diff_result_t *diff,
                          size_t *out_pages,
                          size_t max_pages,
                          size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // GDSL_DIFF_H
