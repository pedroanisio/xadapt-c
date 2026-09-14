/*
 * adapt_decode.c — the decoding entry points: the streaming decoder, the
 * in-memory decoder, the header and trailer readers, the memory bounds
 * and the messages. The model and the framing rules they share with the
 * encoder are in adapt_model.h.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 */
/* For the tables' mapping (adapt_model.h): mmap, MAP_ANONYMOUS, madvise. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "adapt.h"
#include "adapt_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t adapt_crc32(const uint8_t *data, size_t len) {
    crc32_table t;
    crc32_table_init(&t);
    return ~crc32_update(&t, CRC32_INIT, data, len);
}

/* ------------------------------------------------------------------------
 * container.rs — framing
 * --------------------------------------------------------------------- */

size_t adapt_header_len(const uint8_t *fixed) {
    return ADAPT_HEADER_BYTES + ((fixed[7] & ADAPT_PERIOD_FLAG) ? ADAPT_PERIOD_BYTES : 0);
}

size_t adapt_sizes_header_bytes(adapt_sizes sizes) {
    return ADAPT_HEADER_BYTES + (sizes.period != 0 ? ADAPT_PERIOD_BYTES : 0);
}

size_t adapt_segment_table_len(uint64_t count) {
    return (size_t)(1 + count * ADAPT_SEGMENT_ENTRY_BYTES + ADAPT_SEGMENT_COUNT_BYTES);
}

int adapt_segment_bytes_valid(uint64_t segment_bytes) {
    return segment_bytes != 0 && segment_bytes % ADAPT_BLOCK_BYTES == 0 && segment_bytes <= UINT32_MAX;
}

adapt_status adapt_read_header(const uint8_t *header, size_t len, adapt_sizes *sizes, adapt_error *error) {
    adapt_sizes s;
    if (header == NULL || sizes == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    if (memcmp(header, "ADPT", 4) != 0) {
        return fail(error, ADAPT_ERROR_BAD_MAGIC, 0, 0);
    }
    s.max = 0;
    if (header[4] == ADAPT_FORMAT_VERSION) {
        s.fast = 0;
    } else if (header[4] == ADAPT_FAST_VERSION) {
        s.fast = 1;
    } else if (header[4] == ADAPT_MAX_VERSION) {
        s.fast = 0;
        s.max = 1;
    } else {
        return fail(error, ADAPT_ERROR_UNSUPPORTED_VERSION, header[4], 0);
    }
    s.period = 0;
    if (header[7] & ADAPT_PERIOD_FLAG) {
        if (len < ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
        s.period = (uint16_t)(header[8] | (uint16_t)header[9] << 8);
        if (s.period == 0) {
            return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
        }
    }
    s.table_log = header[5] & 31;
    s.match_log = header[6] & 31;
    s.window_log = (uint8_t)(header[7] & ~(ADAPT_PERIOD_FLAG | ADAPT_SEGMENT_FLAG | ADAPT_X86_FLAG));
    s.class_code = (header[5] >> 5) & 3;
    s.archive = (header[5] & ADAPT_ARCHIVE_FLAG) != 0;
    s.set_code = header[6] >> 5;
    s.segmented = (header[7] & ADAPT_SEGMENT_FLAG) != 0;
    s.x86 = (header[7] & ADAPT_X86_FLAG) != 0;
    if (!sizes_valid(s)) {
        return fail(error, ADAPT_ERROR_BAD_HEADER, 0, 0);
    }
    *sizes = s;
    return fail(error, ADAPT_OK, 0, 0);
}

adapt_status adapt_read_trailer(const uint8_t *trailer, uint64_t *length, uint32_t *crc32, adapt_error *error) {
    if (trailer == NULL || length == NULL || crc32 == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (trailer[0] != METHOD_END) {
        return fail(error, ADAPT_ERROR_BAD_BLOCK, 0, 0);
    }
    read_trailer_payload(trailer + 1, length, crc32);
    return fail(error, ADAPT_OK, 0, 0);
}

uint32_t adapt_class_width(adapt_sizes sizes) {
    return sizes.class_code < 3 ? CLASS_WIDTHS[sizes.class_code] : 0;
}

void adapt_dropped_contexts(adapt_sizes sizes, char *buf, size_t len) {
    uint16_t mask = sizes.set_code < ADAPT_CONTEXT_SETS ? CONTEXT_SETS[sizes.set_code] : 0;
    /* the max profile's kinds are not the normal profile's to drop */
    size_t kinds = sizes.max ? HASHED : BASE_HASHED;
    size_t i, at = 0;
    if (len == 0) {
        return;
    }
    buf[0] = 0;
    for (i = 0; i <= kinds; i++) {
        const char *name = i < kinds ? CONTEXT_NAMES[i] : "runs";
        int dropped = i < kinds ? (mask >> i & 1) != 0 : (mask & NO_RUNS) != 0;
        if (dropped) {
            int w = snprintf(buf + at, len - at, "%s%s", at == 0 ? "" : ", ", name);
            if (w < 0 || (size_t)w >= len - at) {
                return;
            }
            at += (size_t)w;
        }
    }
}

uint64_t adapt_model_bytes(adapt_sizes sizes) {
    return ((uint64_t)1 << sizes.table_log) * 16 + (sizes.fast ? O1_BYTES : 0) + ((uint64_t)1 << sizes.match_log) * 4 +
           (uint64_t)(1 << 16) * 33 * 2 + side_table_bytes(sizes.fast, sizes.max) + ((uint64_t)1 << sizes.window_log);
}

void adapt_fast_contexts(adapt_sizes sizes, char *buf, size_t len) {
    uint16_t mask = sizes.set_code < ADAPT_FAST_SETS ? FAST_SETS[sizes.set_code] : 0;
    size_t i, at = 0;
    if (len == 0) {
        return;
    }
    buf[0] = 0;
    for (i = 0; i < HASHED; i++) {
        if ((mask >> i & 1) == 0) {
            int w = snprintf(buf + at, len - at, "%s%s", at == 0 ? "" : ", ", CONTEXT_NAMES[i]);
            if (w < 0 || (size_t)w >= len - at) {
                return;
            }
            at += (size_t)w;
        }
    }
}

uint64_t adapt_decode_memory_bound(adapt_sizes sizes) {
    return adapt_model_bytes(sizes) + block_workspace() + HEADROOM;
}

adapt_limits adapt_default_limits(void) {
    adapt_limits l;
    l.max_output = ADAPT_DEFAULT_MAX_OUTPUT;
    l.max_memory = ADAPT_DEFAULT_MAX_MEMORY;
    return l;
}

/* ------------------------------------------------------------------------
 * container.rs — the streaming decoder
 * --------------------------------------------------------------------- */

adapt_status adapt_decode_stream(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                 adapt_limits limits, adapt_stats *stats, adapt_error *error) {
    uint8_t header[ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES];
    uint8_t block_header[ADAPT_BLOCK_HEADER_BYTES];
    adapt_sizes sizes;
    adapt_status status;
    size_t header_len;
    block_decoder decoder = {NULL};
    uint8_t *payload = NULL;
    uint8_t *scratch = NULL;
    uint8_t *table = NULL;
    segment_counter segments;
    crc32_table crc_table;
    uint64_t written = 0;
    uint32_t crc = CRC32_INIT;
    uint64_t output_bytes = ADAPT_HEADER_BYTES;
    size_t got;
    if (read == NULL || write == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    fail(error, ADAPT_OK, 0, 0);
    counter_init(&segments, 0);
    status = fill(read, read_context, header, ADAPT_HEADER_BYTES, &got, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (got != ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    header_len = adapt_header_len(header);
    if (header_len > ADAPT_HEADER_BYTES) {
        status = fill(read, read_context, header + ADAPT_HEADER_BYTES, header_len - ADAPT_HEADER_BYTES, &got, error);
        if (status != ADAPT_OK) {
            return status;
        }
        if (got != header_len - ADAPT_HEADER_BYTES) {
            return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
        }
    }
    status = adapt_read_header(header, header_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    output_bytes = header_len;
    status = check_memory(adapt_decode_memory_bound(sizes), limits.max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    segments.segmented = sizes.segmented;
    payload = malloc(ADAPT_BLOCK_BYTES);
    scratch = malloc(ADAPT_BLOCK_BYTES);
    if (payload == NULL || scratch == NULL) {
        status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
        goto done;
    }
    crc32_table_init(&crc_table);
    for (;;) {
        uint8_t method;
        size_t raw, payload_len;
        status = fill(read, read_context, block_header, 1, &got, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (got != 1) {
            status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
            goto done;
        }
        if (block_header[0] == METHOD_SEGMENT) {
            /* A fresh model for the next segment; the old one goes first,
             * so that one model's memory is ever held. */
            status = counter_marker(&segments, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            output_bytes += 1;
            model_free(decoder.m);
            decoder.m = NULL;
            continue;
        }
        if (block_header[0] == METHOD_TABLE) {
            size_t len;
            status = counter_table_len(&segments, &len, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            free(table);
            table = malloc(len);
            if (table == NULL) {
                status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
                goto done;
            }
            status = fill(read, read_context, table, len, &got, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            if (got != len) {
                status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
                goto done;
            }
            status = counter_table(&segments, table, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            output_bytes += 1 + len;
            continue;
        }
        if (block_header[0] == METHOD_END) {
            uint8_t trailer[TRAILER_PAYLOAD];
            uint64_t len;
            uint32_t sum;
            uint8_t rest[4096];
            status = counter_end(&segments, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            status = fill(read, read_context, trailer, TRAILER_PAYLOAD, &got, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            if (got != TRAILER_PAYLOAD) {
                status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
                goto done;
            }
            output_bytes += ADAPT_TRAILER_BYTES;
            read_trailer_payload(trailer, &len, &sum);
            if (len != written || sum != ~crc) {
                status = fail(error, ADAPT_ERROR_CHECKSUM, 0, 0);
                goto done;
            }
            status = fill(read, read_context, rest, 1, &got, error);
            if (status != ADAPT_OK) {
                goto done;
            }
            if (got != 0) {
                uint64_t extra = 1;
                for (;;) {
                    status = fill(read, read_context, rest, sizeof(rest), &got, error);
                    if (status != ADAPT_OK) {
                        goto done;
                    }
                    extra += got;
                    if (got < sizeof(rest)) {
                        break;
                    }
                }
                status = fail(error, ADAPT_ERROR_TRAILING_BYTES, extra, 0);
                goto done;
            }
            if (stats != NULL) {
                stats->input_bytes = written;
                stats->output_bytes = output_bytes;
                stats->crc32 = sum;
                stats->segments = segments.segmented ? (uint32_t)segments.count : 1;
            }
            status = ADAPT_OK;
            goto done;
        }
        status = fill(read, read_context, block_header + 1, ADAPT_BLOCK_HEADER_BYTES - 1, &got, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (got != ADAPT_BLOCK_HEADER_BYTES - 1) {
            status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
            goto done;
        }
        status = parse_block_header(block_header, &method, &raw, &payload_len, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        status = counter_block(&segments, raw, ADAPT_BLOCK_HEADER_BYTES + payload_len, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        status = fill(read, read_context, payload, payload_len, &got, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (got != payload_len) {
            status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
            goto done;
        }
        output_bytes += ADAPT_BLOCK_HEADER_BYTES + payload_len;
        if (written + raw > limits.max_output) {
            status = fail(error, ADAPT_ERROR_TOO_LARGE, written + raw, limits.max_output);
            goto done;
        }
        if (decoder.m == NULL) {
            decoder.m = model_new(sizes);
            if (decoder.m == NULL) {
                status = fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
                goto done;
            }
        }
        status = decode_block(&decoder, method, raw, payload, payload_len, scratch, error);
        if (status != ADAPT_OK) {
            goto done;
        }
        if (sizes.x86) {
            x86_inverse(scratch, raw);
        }
        crc = crc32_update(&crc_table, crc, scratch, raw);
        written += raw;
        {
            int rc = write(write_context, scratch, raw);
            if (rc != 0) {
                status = fail(error, ADAPT_ERROR_IO, (uint64_t)(rc < 0 ? -(int64_t)rc : rc), 0);
                goto done;
            }
        }
    }
done:
    model_free(decoder.m);
    counter_free(&segments);
    free(table);
    free(payload);
    free(scratch);
    return status;
}

/* ------------------------------------------------------------------------
 * container.rs — the in-memory decoder
 * --------------------------------------------------------------------- */

typedef struct memory_reader {
    const uint8_t *src;
    size_t len;
    size_t pos;
} memory_reader;

static int memory_read(void *context, uint8_t *buf, size_t len, size_t *got) {
    memory_reader *r = context;
    size_t n = r->len - r->pos < len ? r->len - r->pos : len;
    /* An empty source may be NULL, which may not be offset or handed to
     * memcpy even for zero bytes. */
    if (n != 0) {
        memcpy(buf, r->src + r->pos, n);
    }
    r->pos += n;
    *got = n;
    return 0;
}

typedef struct memory_writer {
    uint8_t *out;
    size_t len;
    size_t pos;
} memory_writer;

static int memory_write(void *context, const uint8_t *buf, size_t len) {
    memory_writer *w = context;
    if (len > w->len - w->pos) {
        return 1;
    }
    memcpy(w->out + w->pos, buf, len);
    w->pos += len;
    return 0;
}

adapt_status adapt_decode(const uint8_t *file, size_t file_len, adapt_limits limits, uint8_t **out, size_t *out_len,
                          adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    size_t pos = ADAPT_HEADER_BYTES;
    uint64_t total = 0, len = 0, required;
    uint32_t sum = 0;
    memory_reader reader;
    memory_writer writer;
    adapt_stats stats;
    segment_counter segments;
    if ((file == NULL && file_len != 0) || out == NULL || out_len == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    *out = NULL;
    *out_len = 0;
    if (file_len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    status = adapt_read_header(file, file_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    pos = adapt_sizes_header_bytes(sizes);
    counter_init(&segments, sizes.segmented);
    for (;;) {
        uint8_t method;
        size_t raw, payload_len, start = pos;
        const uint8_t *payload;
        status = read_block(file, file_len, &pos, &method, &raw, &payload, &payload_len, error);
        if (status != ADAPT_OK) {
            counter_free(&segments);
            return status;
        }
        if (method == METHOD_END) {
            status = counter_end(&segments, error);
            if (status != ADAPT_OK) {
                counter_free(&segments);
                return status;
            }
            read_trailer_payload(payload, &len, &sum);
            break;
        } else if (method == METHOD_SEGMENT) {
            status = counter_marker(&segments, error);
        } else if (method == METHOD_TABLE) {
            size_t table_len;
            status = counter_table_len(&segments, &table_len, error);
            if (status == ADAPT_OK) {
                if (file_len - pos < table_len) {
                    status = fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
                } else {
                    status = counter_table(&segments, file + pos, error);
                    pos += table_len;
                }
            }
        } else {
            status = counter_block(&segments, raw, pos - start, error);
            total += raw;
        }
        if (status != ADAPT_OK) {
            counter_free(&segments);
            return status;
        }
    }
    counter_free(&segments);
    if (pos != file_len) {
        return fail(error, ADAPT_ERROR_TRAILING_BYTES, file_len - pos, 0);
    }
    if (len != total) {
        return fail(error, ADAPT_ERROR_CHECKSUM, 0, 0);
    }
    if (len > limits.max_output) {
        return fail(error, ADAPT_ERROR_TOO_LARGE, len, limits.max_output);
    }
    required = adapt_decode_memory_bound(sizes) + len;
    status = check_memory(required, limits.max_memory, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (len > (uint64_t)(size_t)-1) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    writer.out = malloc(len > 0 ? (size_t)len : 1);
    if (writer.out == NULL) {
        return fail(error, ADAPT_ERROR_ALLOCATION, 0, 0);
    }
    writer.len = (size_t)len;
    writer.pos = 0;
    reader.src = file;
    reader.len = file_len;
    reader.pos = 0;
    status = adapt_decode_stream(memory_read, &reader, memory_write, &writer, limits, &stats, error);
    if (status != ADAPT_OK) {
        free(writer.out);
        return status;
    }
    *out = writer.out;
    *out_len = writer.pos;
    return ADAPT_OK;
}

void adapt_free(uint8_t *bytes) {
    free(bytes);
}

adapt_status adapt_declared_len(const uint8_t *file, size_t file_len, uint64_t *length, adapt_error *error) {
    adapt_sizes sizes;
    adapt_status status;
    uint32_t crc;
    if ((file == NULL && file_len != 0) || length == NULL) {
        return fail(error, ADAPT_ERROR_INVALID_ARGUMENT, 0, 0);
    }
    if (file_len < ADAPT_HEADER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    status = adapt_read_header(file, file_len, &sizes, error);
    if (status != ADAPT_OK) {
        return status;
    }
    if (file_len < adapt_sizes_header_bytes(sizes) + ADAPT_TRAILER_BYTES) {
        return fail(error, ADAPT_ERROR_TRUNCATED, 0, 0);
    }
    return adapt_read_trailer(file + file_len - ADAPT_TRAILER_BYTES, length, &crc, error);
}

/* ------------------------------------------------------------------------
 * lib.rs — the messages
 * --------------------------------------------------------------------- */

int adapt_error_message(const adapt_error *error, char *buf, size_t len) {
    if (error == NULL) {
        return snprintf(buf, len, "no error");
    }
    switch (error->status) {
        case ADAPT_OK:
            return snprintf(buf, len, "no error");
        case ADAPT_ERROR_MEMORY_LIMIT:
            return snprintf(buf, len, "memory budget exceeded: need %llu bytes, limit %llu", (unsigned long long)error->detail,
                            (unsigned long long)error->limit);
        case ADAPT_ERROR_ALLOCATION:
            return snprintf(buf, len, "memory allocation failed");
        case ADAPT_ERROR_BAD_MAGIC:
            return snprintf(buf, len, "not an adapt file (bad signature)");
        case ADAPT_ERROR_UNSUPPORTED_VERSION:
            return snprintf(buf, len, "unsupported format version %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_BAD_HEADER:
            return snprintf(buf, len, "invalid header field");
        case ADAPT_ERROR_TOO_LARGE:
            return snprintf(buf, len, "declared size %llu exceeds the limit %llu", (unsigned long long)error->detail,
                            (unsigned long long)error->limit);
        case ADAPT_ERROR_TRUNCATED:
            return snprintf(buf, len, "file is truncated");
        case ADAPT_ERROR_BAD_BLOCK:
            return snprintf(buf, len, "invalid block");
        case ADAPT_ERROR_BAD_METHOD:
            return snprintf(buf, len, "unknown block method %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_TRAILING_BYTES:
            return snprintf(buf, len, "%llu unexpected trailing byte(s)", (unsigned long long)error->detail);
        case ADAPT_ERROR_CHECKSUM:
            return snprintf(buf, len, "checksum mismatch: the recovered bytes are damaged");
        case ADAPT_ERROR_VERIFICATION:
            if (error->cause == ADAPT_OK) {
                return snprintf(buf, len, "verification failed: decoded bytes differ from the input");
            } else {
                adapt_error cause;
                char text[128];
                cause.status = error->cause;
                cause.cause = ADAPT_OK;
                cause.detail = error->detail;
                cause.limit = error->limit;
                adapt_error_message(&cause, text, sizeof(text));
                return snprintf(buf, len, "verification failed: %s", text);
            }
        case ADAPT_ERROR_IO:
            return snprintf(buf, len, "i/o error: callback status %llu", (unsigned long long)error->detail);
        case ADAPT_ERROR_INVALID_ARGUMENT:
            return snprintf(buf, len, "invalid argument");
        case ADAPT_ERROR_BAD_ARCHIVE:
            return snprintf(buf, len, "invalid archive at byte %llu", (unsigned long long)error->detail);
    }
    return snprintf(buf, len, "unknown error %d", (int)error->status);
}
