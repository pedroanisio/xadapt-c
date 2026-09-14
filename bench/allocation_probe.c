/* Linux research client: compare table/window allocations through the public
 * codec API. It writes an ordinary max-profile container with filesystem
 * metadata. No predictor constants or codec sources change between probes.
 * Source: OpenAI GPT-6 via Codex. */
#define _POSIX_C_SOURCE 200809L
#include "../adpat/c/adapt.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct writer {
    FILE *file;
    adapt_metadata metadata;
    uint64_t bytes;
    int started;
} writer;

static int read_source(void *context, uint8_t *data, size_t capacity, size_t *length) {
    FILE *file = context;
    *length = fread(data, 1, capacity, file);
    return ferror(file) ? 1 : 0;
}

static int write_container(void *context, const uint8_t *data, size_t length) {
    writer *out = context;
    uint8_t header[ADAPT_HEADER_MAX_BYTES];
    if (!out->started) {
        length = adapt_header_add_metadata(data, length, &out->metadata, header, sizeof(header));
        if (!length) return 1;
        data = header;
        out->started = 1;
    }
    if (fwrite(data, 1, length, out->file) != length) return 1;
    out->bytes += length;
    return 0;
}

static int number(const char *text, uint64_t *value) {
    char *end;
    unsigned long long parsed;
    if (*text < '0' || *text > '9') return 0;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || *end) return 0;
    *value = (uint64_t)parsed;
    return 1;
}

int main(int argc, char **argv) {
    uint64_t budget, table, window;
    struct stat source_stat;
    adapt_sizes sizes;
    adapt_error error;
    adapt_stats stats;
    adapt_status status;
    FILE *source;
    writer out = {0};
    int fd, closed;
    char message[256];
    if (argc != 6 || !number(argv[3], &budget) || !number(argv[4], &table) ||
        !number(argv[5], &window) || table > 255 || window > 255) {
        fputs("usage: allocation-probe INPUT OUTPUT MEMORY_BYTES TABLE_LOG WINDOW_LOG\n", stderr);
        return 2;
    }
    if (stat(argv[1], &source_stat) || !S_ISREG(source_stat.st_mode) || source_stat.st_size < 0) {
        fputs("input must be a readable regular file\n", stderr);
        return 1;
    }
    status = adapt_sizes_for_input_max(1, (uint64_t)source_stat.st_size, budget, &sizes, &error);
    if (status != ADAPT_OK) goto codec_error;
    sizes.table_log = (uint8_t)table;
    sizes.window_log = (uint8_t)window;
    if (!adapt_sizes_valid(sizes) || adapt_encode_memory_bound(sizes) > budget) {
        fputs("invalid allocation or memory budget exceeded\n", stderr);
        return 1;
    }
    source = fopen(argv[1], "rb");
    if (!source) return 1;
    fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { fclose(source); perror("output"); return 1; }
    out.file = fdopen(fd, "wb");
    if (!out.file) { close(fd); fclose(source); return 1; }
    out.metadata.mode = (uint32_t)(source_stat.st_mode & 07777);
    out.metadata.atime_seconds = source_stat.st_atim.tv_sec;
    out.metadata.atime_nanoseconds = (uint32_t)source_stat.st_atim.tv_nsec;
    out.metadata.mtime_seconds = source_stat.st_mtim.tv_sec;
    out.metadata.mtime_nanoseconds = (uint32_t)source_stat.st_mtim.tv_nsec;
    status = adapt_encode_stream(read_source, source, write_container, &out, sizes, budget, &stats, &error);
    fclose(source);
    closed = fclose(out.file);
    if (status != ADAPT_OK) goto codec_error;
    if (closed) { perror("output"); return 1; }
    printf("{\"container_bytes\":%" PRIu64 ",\"table_log\":%u,\"window_log\":%u,"
           "\"memory_bound_bytes\":%" PRIu64 "}\n",
           out.bytes, sizes.table_log, sizes.window_log, adapt_encode_memory_bound(sizes));
    return 0;
codec_error:
    adapt_error_message(&error, message, sizeof(message));
    fprintf(stderr, "%s\n", message);
    return 1;
}
