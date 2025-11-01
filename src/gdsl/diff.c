#include "gdsl/diff.h"

#include <stdlib.h>
#include <string.h>

#define GDSL_DIFF_VERSION 1u
#define GDSL_DEFAULT_PAGE_SIZE 4096u

static size_t page_count_for_length(size_t length, size_t page_size) {
    if (length == 0) {
        return 0;
    }
    return (length + page_size - 1) / page_size;
}

static size_t min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

void gdsl_diff_result_destroy(gdsl_diff_result_t *result) {
    if (!result) {
        return;
    }

    free(result->chunks);
    free(result->payload);
    result->chunks = NULL;
    result->payload = NULL;
    result->chunk_count = 0;
    result->payload_length = 0;
    result->header.chunk_count = 0;
    result->header.target_length = 0;
}

static int ensure_capacity(gdsl_diff_result_t *out,
                           size_t chunk_count,
                           size_t payload_size) {
    out->chunks = (gdsl_diff_chunk_t *)malloc(chunk_count * sizeof(gdsl_diff_chunk_t));
    if (!out->chunks && chunk_count > 0) {
        return -1;
    }
    out->payload = (uint8_t *)malloc(payload_size);
    if (!out->payload && payload_size > 0) {
        free(out->chunks);
        out->chunks = NULL;
        return -1;
    }
    out->chunk_count = chunk_count;
    out->payload_length = payload_size;
    return 0;
}

int gdsl_diff(const uint8_t *base,
              size_t base_length,
              const uint8_t *target,
              size_t target_length,
              gdsl_diff_result_t *out) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->header.version = GDSL_DIFF_VERSION;
    out->header.page_size = GDSL_DEFAULT_PAGE_SIZE;
    out->header.flags = 0;
    out->header.target_length = target_length;

    if (!base && base_length > 0) {
        return -1;
    }
    if (!target && target_length > 0) {
        return -1;
    }

    size_t page_size = out->header.page_size;
    size_t max_length = base_length > target_length ? base_length : target_length;
    size_t total_pages = page_count_for_length(max_length, page_size);

    size_t chunk_count = 0;
    size_t payload_size = 0;

    for (size_t page_index = 0; page_index < total_pages; ++page_index) {
        size_t page_offset = page_index * page_size;
        size_t page_end = page_offset + page_size;
        if (page_end > max_length) {
            page_end = max_length;
        }
        size_t span = page_end - page_offset;
        if (span == 0) {
            continue;
        }

        size_t target_span = 0;
        if (page_offset < target_length) {
            size_t remaining = target_length - page_offset;
            target_span = min_size(span, remaining);
        }
        if (target_span == 0) {
            continue;
        }

        const uint8_t *base_ptr = page_offset < base_length ? base + page_offset : NULL;
        size_t base_available = base_ptr ? min_size(target_span, base_length - page_offset) : 0;

        const uint8_t *target_ptr = (target && target_span > 0) ? target + page_offset : NULL;
        size_t target_available = target_ptr ? target_span : 0;

        int changed = 0;
        for (size_t i = 0; i < target_span; ++i) {
            uint8_t base_byte = (i < base_available) ? base_ptr[i] : 0;
            uint8_t target_byte = (i < target_available) ? target_ptr[i] : 0;
            if (base_byte != target_byte) {
                changed = 1;
                break;
            }
        }

        if (changed) {
            chunk_count++;
            payload_size += target_span;
        }
    }

    if (chunk_count == 0) {
        out->header.chunk_count = 0;
        out->chunk_count = 0;
        return 0;
    }

    if (ensure_capacity(out, chunk_count, payload_size) != 0) {
        return -1;
    }

    size_t payload_offset = 0;
    size_t emitted = 0;

    for (size_t page_index = 0; page_index < total_pages; ++page_index) {
        size_t page_offset = page_index * page_size;
        size_t page_end = page_offset + page_size;
        if (page_end > max_length) {
            page_end = max_length;
        }
        size_t span = page_end - page_offset;
        if (span == 0) {
            continue;
        }

        size_t target_span = 0;
        if (page_offset < target_length) {
            size_t remaining = target_length - page_offset;
            target_span = min_size(span, remaining);
        }
        if (target_span == 0) {
            continue;
        }

        const uint8_t *base_ptr = page_offset < base_length ? base + page_offset : NULL;
        size_t base_available = base_ptr ? min_size(target_span, base_length - page_offset) : 0;

        const uint8_t *target_ptr = (target && target_span > 0) ? target + page_offset : NULL;
        size_t target_available = target_ptr ? target_span : 0;

        int changed = 0;
        for (size_t i = 0; i < target_span; ++i) {
            uint8_t base_byte = (i < base_available) ? base_ptr[i] : 0;
            uint8_t target_byte = (i < target_available) ? target_ptr[i] : 0;
            if (base_byte != target_byte) {
                changed = 1;
                break;
            }
        }

        if (!changed) {
            continue;
        }

        gdsl_diff_chunk_t *chunk = &out->chunks[emitted];
        chunk->page_index = page_index;
        chunk->length = target_span;
        chunk->data_offset = payload_offset;

        for (size_t i = 0; i < target_span; ++i) {
            uint8_t value = (i < target_available) ? target_ptr[i] : 0;
            out->payload[payload_offset + i] = value;
        }

        payload_offset += target_span;
        emitted++;
    }

    out->chunk_count = emitted;
    out->header.chunk_count = (uint32_t)emitted;
    out->payload_length = payload_offset;
    if (payload_offset == 0) {
        free(out->payload);
        out->payload = NULL;
    }
    return 0;
}

int gdsl_patch(const uint8_t *base,
               size_t base_length,
               const gdsl_diff_result_t *diff,
               uint8_t **out_buffer,
               size_t *out_length) {
    if (!diff || !out_buffer || !out_length) {
        return -1;
    }
    if (diff->header.page_size == 0) {
        return -1;
    }

    size_t page_size = diff->header.page_size;
    size_t result_length = diff->header.target_length;
    size_t buffer_length = result_length;
    for (size_t i = 0; i < diff->chunk_count; ++i) {
        const gdsl_diff_chunk_t *chunk = &diff->chunks[i];

        if (chunk->length > page_size) {
            return -1;
        }
        if (chunk->data_offset > diff->payload_length) {
            return -1;
        }
        if (diff->payload_length - chunk->data_offset < chunk->length) {
            return -1;
        }

        if (page_size != 0 && chunk->page_index > SIZE_MAX / page_size) {
            return -1;
        }
        size_t offset = chunk->page_index * page_size;
        if (offset > buffer_length) {
            return -1;
        }
        if (buffer_length - offset < chunk->length) {
            return -1;
        }
    }

    uint8_t *buffer = (uint8_t *)malloc(buffer_length);
    if (!buffer && buffer_length > 0) {
        return -1;
    }

    if (buffer_length > 0) {
        memset(buffer, 0, buffer_length);
    }

    if (base && base_length > 0) {
        size_t copy = base_length < buffer_length ? base_length : buffer_length;
        memcpy(buffer, base, copy);
    }

    for (size_t i = 0; i < diff->chunk_count; ++i) {
        const gdsl_diff_chunk_t *chunk = &diff->chunks[i];
        size_t offset = chunk->page_index * page_size;
        memcpy(buffer + offset, diff->payload + chunk->data_offset, chunk->length);
    }

    *out_buffer = buffer;
    *out_length = result_length;
    return 0;
}

int gdsl_read_changed_set(const gdsl_diff_result_t *diff,
                          size_t *out_pages,
                          size_t max_pages,
                          size_t *out_count) {
    if (!diff || !out_count) {
        return -1;
    }

    size_t count = diff->chunk_count;
    if (out_pages) {
        if (count > max_pages) {
            return -1;
        }
        for (size_t i = 0; i < count; ++i) {
            out_pages[i] = diff->chunks[i].page_index;
        }
    }

    *out_count = count;
    return 0;
}
