/*
 * adapt_archive.c — the archive stream a folder is coded as: the entry
 * headers the packing side writes and the unpacker that reads a stream
 * back, in any pieces, into a sink of callbacks. The rules are stated in
 * adapt.h; every one of them is checked here, at the byte that breaks
 * it, so a damaged or hostile stream never names a path outside the
 * folder being restored.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14.
 *
 * C99 and the standard library only, like the rest of the library: the
 * filesystem is the sink's business.
 */
#include "adapt.h"

#include <stdlib.h>
#include <string.h>

static void put_integer(uint8_t *out, uint64_t value, size_t bytes) {
    size_t i;
    for (i = 0; i < bytes; i++) {
        out[i] = (uint8_t)(value >> (8 * i));
    }
}

static uint64_t get_integer(const uint8_t *data, size_t bytes) {
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < bytes; i++) {
        value |= (uint64_t)data[i] << (8 * i);
    }
    return value;
}

static int64_t signed_seconds(uint64_t value) {
    return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}

static int metadata_valid(const adapt_metadata *m) {
    return m != NULL && (m->mode & ~07777u) == 0 && m->atime_nanoseconds < 1000000000u &&
           m->mtime_nanoseconds < 1000000000u;
}

int adapt_metadata_write(const adapt_metadata *m, uint8_t *out) {
    if (out == NULL || !metadata_valid(m)) {
        return 0;
    }
    put_integer(out, m->mode, 4);
    put_integer(out + 4, (uint64_t)m->atime_seconds, 8);
    put_integer(out + 12, m->atime_nanoseconds, 4);
    put_integer(out + 16, (uint64_t)m->mtime_seconds, 8);
    put_integer(out + 24, m->mtime_nanoseconds, 4);
    return 1;
}

int adapt_metadata_read(const uint8_t *data, adapt_metadata *m) {
    adapt_metadata value;
    if (data == NULL || m == NULL) {
        return 0;
    }
    value.mode = (uint32_t)get_integer(data, 4);
    value.atime_seconds = signed_seconds(get_integer(data + 4, 8));
    value.atime_nanoseconds = (uint32_t)get_integer(data + 12, 4);
    value.mtime_seconds = signed_seconds(get_integer(data + 16, 8));
    value.mtime_nanoseconds = (uint32_t)get_integer(data + 24, 4);
    if (!metadata_valid(&value)) {
        return 0;
    }
    *m = value;
    return 1;
}

/* ------------------------------------------------------------------------
 * Paths and entry headers
 * --------------------------------------------------------------------- */

/* Whether the component ending here — `len` bytes since the last '/' —
 * is one a path may hold. */
static int component_ok(const char *start, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (len == 1 && start[0] == '.') {
        return 0;
    }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
        return 0;
    }
    return 1;
}

int adapt_archive_path_valid(const char *path) {
    size_t i, n, component = 0;
    if (path == NULL) {
        return 0;
    }
    n = strlen(path);
    if (n == 0 || n > ADAPT_ARCHIVE_PATH_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (path[i] == '/') {
            if (!component_ok(path + component, i - component)) {
                return 0;
            }
            component = i + 1;
        }
    }
    return component_ok(path + component, n - component);
}

static void put_u16(uint8_t *out, size_t v) {
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
}

size_t adapt_archive_entry(uint8_t kind, const char *path, uint64_t size, const char *target, uint8_t *out, size_t cap) {
    size_t path_len, target_len = 0, n, i;
    if (kind != ADAPT_ARCHIVE_DIRECTORY && kind != ADAPT_ARCHIVE_FILE && kind != ADAPT_ARCHIVE_SYMLINK) {
        return 0;
    }
    if (!adapt_archive_path_valid(path)) {
        return 0;
    }
    path_len = strlen(path);
    n = 1 + 2 + path_len;
    if (kind == ADAPT_ARCHIVE_FILE) {
        n += 8;
    } else if (kind == ADAPT_ARCHIVE_SYMLINK) {
        if (target == NULL) {
            return 0;
        }
        target_len = strlen(target);
        if (target_len == 0 || target_len > ADAPT_ARCHIVE_PATH_MAX) {
            return 0;
        }
        n += 2 + target_len;
    }
    if (out == NULL || cap < n) {
        return n;
    }
    out[0] = kind;
    put_u16(out + 1, path_len);
    memcpy(out + 3, path, path_len);
    if (kind == ADAPT_ARCHIVE_FILE) {
        for (i = 0; i < 8; i++) {
            out[3 + path_len + i] = (uint8_t)(size >> (8 * i));
        }
    } else if (kind == ADAPT_ARCHIVE_SYMLINK) {
        put_u16(out + 3 + path_len, target_len);
        memcpy(out + 5 + path_len, target, target_len);
    }
    return n;
}

size_t adapt_archive_entry_metadata(uint8_t kind, const char *path, uint64_t size, const char *target,
                                    const adapt_metadata *metadata, uint8_t *out, size_t cap) {
    size_t n = adapt_archive_entry(kind, path, size, target, NULL, 0);
    if (n == 0 || !metadata_valid(metadata)) {
        return 0;
    }
    if (out != NULL && cap >= n + ADAPT_METADATA_BYTES) {
        (void)adapt_archive_entry(kind, path, size, target, out, cap);
        (void)adapt_metadata_write(metadata, out + n);
    }
    return n + ADAPT_METADATA_BYTES;
}

static void put_u32(uint8_t *out, uint32_t v) {
    put_integer(out, v, 4);
}

/* Whether `extents` obey the rules: ascending, none empty, none
 * overlapping, none past `size`. */
static int extents_valid(const adapt_extent *extents, uint32_t count, uint64_t size) {
    uint64_t end = 0;
    uint32_t i;
    if (count > ADAPT_ARCHIVE_EXTENTS_MAX || (count > 0 && extents == NULL)) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (extents[i].length == 0 || extents[i].offset < end || extents[i].offset > size - extents[i].length ||
            extents[i].length > size) {
            return 0;
        }
        end = extents[i].offset + extents[i].length;
    }
    return 1;
}

size_t adapt_archive_entry_full(uint8_t kind, const char *path, uint64_t size, const char *target,
                                const adapt_extent *extents, uint32_t extent_count, const adapt_metadata *metadata,
                                uint8_t *out, size_t cap) {
    size_t path_len, target_len = 0, n, at;
    if (kind == ADAPT_ARCHIVE_DIRECTORY || kind == ADAPT_ARCHIVE_FILE || kind == ADAPT_ARCHIVE_SYMLINK) {
        return adapt_archive_entry_metadata(kind, path, size, target, metadata, out, cap);
    }
    if (kind != ADAPT_ARCHIVE_HARDLINK && kind != ADAPT_ARCHIVE_SPARSE) {
        return 0;
    }
    if (!adapt_archive_path_valid(path)) {
        return 0;
    }
    path_len = strlen(path);
    n = 1 + 2 + path_len;
    if (kind == ADAPT_ARCHIVE_HARDLINK) {
        if (!adapt_archive_path_valid(target) || strcmp(target, path) == 0) {
            return 0;
        }
        target_len = strlen(target);
        n += 2 + target_len;
    } else {
        if (!extents_valid(extents, extent_count, size) || !metadata_valid(metadata)) {
            return 0;
        }
        n += 8 + 4 + ADAPT_METADATA_BYTES;
    }
    if (out == NULL || cap < n) {
        return n;
    }
    out[0] = kind;
    put_u16(out + 1, path_len);
    memcpy(out + 3, path, path_len);
    at = 3 + path_len;
    if (kind == ADAPT_ARCHIVE_HARDLINK) {
        put_u16(out + at, target_len);
        memcpy(out + at + 2, target, target_len);
    } else {
        put_integer(out + at, size, 8);
        put_u32(out + at + 8, extent_count);
        (void)adapt_metadata_write(metadata, out + at + 12);
    }
    return n;
}

size_t adapt_archive_extent(const adapt_extent *extent, uint8_t *out, size_t cap) {
    if (extent == NULL || extent->length == 0) {
        return 0;
    }
    if (out != NULL && cap >= 16) {
        put_integer(out, extent->offset, 8);
        put_integer(out + 8, extent->length, 8);
    }
    return 16;
}

size_t adapt_archive_attributes(uint32_t count, uint8_t *out, size_t cap) {
    if (count > 65535u) {
        return 0;
    }
    if (out != NULL && cap >= 2) {
        put_u16(out, count);
    }
    return 2;
}

size_t adapt_archive_attribute(const char *name, const uint8_t *value, size_t len, uint8_t *out, size_t cap) {
    size_t name_len, n;
    if (name == NULL || (len > 0 && value == NULL)) {
        return 0;
    }
    name_len = strlen(name);
    if (name_len == 0 || name_len > ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX || len > ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX) {
        return 0;
    }
    n = 2 + name_len + 4 + len;
    if (out != NULL && cap >= n) {
        put_u16(out, name_len);
        memcpy(out + 2, name, name_len);
        put_u32(out + 2 + name_len, (uint32_t)len);
        if (len > 0) {
            memcpy(out + 6 + name_len, value, len);
        }
    }
    return n;
}

/* ------------------------------------------------------------------------
 * The unpacker
 * --------------------------------------------------------------------- */

typedef enum stage {
    STAGE_MAGIC,
    STAGE_KIND,
    STAGE_PATH_LEN,
    STAGE_PATH,
    STAGE_SIZE,
    STAGE_EXTENT_COUNT,
    STAGE_DATA,
    STAGE_TARGET_LEN,
    STAGE_TARGET,
    STAGE_METADATA,
    STAGE_EXTENT_OFFSET,
    STAGE_EXTENT_LENGTH,
    STAGE_ATTR_COUNT,
    STAGE_ATTR_NAME_LEN,
    STAGE_ATTR_NAME,
    STAGE_ATTR_VALUE_LEN,
    STAGE_ATTR_VALUE,
    STAGE_DONE
} stage;

struct adapt_archive_unpacker {
    adapt_archive_sink sink;
    stage stage;
    /* 1 through 4, from the magic's last byte */
    int version;
    /* bytes taken so far, the offset of the next one */
    uint64_t offset;
    /* the rule broken, or NULL; the offset of the byte that broke it */
    const char *problem;
    uint64_t problem_at;
    /* a sink's nonzero status, returned again to every later call */
    int failed;
    uint64_t entries;
    /* the entry being read */
    uint8_t kind;
    size_t field; /* bytes of the current fixed field taken */
    size_t path_len, path_at, component;
    char *path;
    size_t target_len, target_at;
    char *target;
    uint64_t size, remaining;
    /* a sparse entry's extents: how many, how many done, where the last ended */
    uint32_t extent_count, extent_index;
    uint64_t extent_offset, extent_length, extent_end;
    /* the entry's attributes: how many, how many done, the one being read
     * and the name of the previous, for the order */
    uint32_t attr_count, attr_index;
    size_t name_len, name_at, value_len, value_at;
    char name[ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 1];
    char previous_name[ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 1];
    uint8_t *value;
    int has_metadata;
    uint8_t metadata_bytes[ADAPT_METADATA_BYTES];
    adapt_metadata metadata;
    adapt_archive_metadata_fn metadata_callback;
    int root_attributes; /* parsing the version-4 prefix, not an entry */
    adapt_archive_root_attribute_fn root_attribute_callback;
};

adapt_archive_unpacker *adapt_archive_unpacker_new(adapt_archive_sink sink) {
    adapt_archive_unpacker *u = calloc(1, sizeof(*u));
    if (u == NULL) {
        return NULL;
    }
    u->path = malloc(ADAPT_ARCHIVE_PATH_MAX + 1);
    u->target = malloc(ADAPT_ARCHIVE_PATH_MAX + 1);
    u->value = malloc(ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX);
    if (u->path == NULL || u->target == NULL || u->value == NULL) {
        adapt_archive_unpacker_free(u);
        return NULL;
    }
    u->sink = sink;
    u->stage = STAGE_MAGIC;
    return u;
}

void adapt_archive_set_metadata_callback(adapt_archive_unpacker *u, adapt_archive_metadata_fn callback) {
    if (u != NULL) {
        u->metadata_callback = callback;
    }
}

void adapt_archive_set_root_attribute_callback(adapt_archive_unpacker *u, adapt_archive_root_attribute_fn callback) {
    if (u != NULL) {
        u->root_attribute_callback = callback;
    }
}

void adapt_archive_unpacker_free(adapt_archive_unpacker *u) {
    if (u == NULL) {
        return;
    }
    free(u->path);
    free(u->target);
    free(u->value);
    free(u);
}

/* Refuse the byte at `at`: the stream is malformed from here on. */
static int refuse(adapt_archive_unpacker *u, const char *problem, uint64_t at) {
    u->problem = problem;
    u->problem_at = at;
    return ADAPT_ARCHIVE_MALFORMED;
}

/* A sink's status, remembered. */
static int sink_failed(adapt_archive_unpacker *u, int rc) {
    u->failed = rc;
    return rc;
}

/* The path's last component ended at byte `at` (a '/' or the path's end). */
static int close_component(adapt_archive_unpacker *u, uint64_t at) {
    if (!component_ok(u->path + u->component, u->path_at - u->component)) {
        return refuse(u, "path names an empty, '.' or '..' component", at);
    }
    return 0;
}

/* Whether this entry carries a metadata record: every kind but a hard
 * link, from version 2 on. */
static int entry_has_metadata(const adapt_archive_unpacker *u) {
    return u->version >= 2 && u->kind != ADAPT_ARCHIVE_HARDLINK;
}

/* The entry is complete, attributes included: its metadata, if any, then
 * the next entry. */
static int entry_done(adapt_archive_unpacker *u) {
    int rc;
    u->stage = STAGE_KIND;
    if (u->root_attributes) {
        u->root_attributes = 0;
        return 0;
    }
    if (u->has_metadata && u->metadata_callback != NULL) {
        rc = u->metadata_callback(u->sink.context, u->path, u->kind, &u->metadata);
        if (rc != 0) {
            return sink_failed(u, rc);
        }
    }
    return 0;
}

/* The entry's bytes are complete: its attributes come next in version 3;
 * otherwise it is done. */
static int after_bytes(adapt_archive_unpacker *u) {
    if (u->version >= 3 && u->kind != ADAPT_ARCHIVE_HARDLINK) {
        u->field = 0;
        u->attr_count = 0;
        u->attr_index = 0;
        u->previous_name[0] = '\0';
        u->stage = STAGE_ATTR_COUNT;
        return 0;
    }
    return entry_done(u);
}

/* A file's or an extent's bytes are all in. */
static int data_done(adapt_archive_unpacker *u) {
    int rc;
    if (u->kind == ADAPT_ARCHIVE_SPARSE && ++u->extent_index < u->extent_count) {
        u->field = 0;
        u->extent_offset = 0;
        u->stage = STAGE_EXTENT_OFFSET;
        return 0;
    }
    rc = u->sink.file_close(u->sink.context);
    if (rc != 0) {
        return sink_failed(u, rc);
    }
    return after_bytes(u);
}

/* The entry's header is complete: hand it on, and move to its bytes or
 * the next stage. */
static int entry_ready(adapt_archive_unpacker *u) {
    int rc;
    u->entries++;
    switch (u->kind) {
        case ADAPT_ARCHIVE_DIRECTORY:
            rc = u->sink.directory(u->sink.context, u->path);
            return rc != 0 ? sink_failed(u, rc) : after_bytes(u);
        case ADAPT_ARCHIVE_SYMLINK:
            rc = u->sink.symlink(u->sink.context, u->path, u->target);
            return rc != 0 ? sink_failed(u, rc) : after_bytes(u);
        case ADAPT_ARCHIVE_HARDLINK:
            rc = u->sink.hardlink(u->sink.context, u->path, u->target);
            return rc != 0 ? sink_failed(u, rc) : entry_done(u);
        case ADAPT_ARCHIVE_SPARSE:
            rc = u->sink.sparse_open(u->sink.context, u->path, u->size, u->extent_count);
            if (rc != 0) {
                return sink_failed(u, rc);
            }
            u->extent_index = 0;
            u->extent_end = 0;
            if (u->extent_count == 0) {
                rc = u->sink.file_close(u->sink.context);
                return rc != 0 ? sink_failed(u, rc) : after_bytes(u);
            }
            u->field = 0;
            u->extent_offset = 0;
            u->stage = STAGE_EXTENT_OFFSET;
            return 0;
        default:
            rc = u->sink.file_open(u->sink.context, u->path, u->size);
            if (rc != 0) {
                return sink_failed(u, rc);
            }
            u->remaining = u->size;
            if (u->remaining == 0) {
                return data_done(u);
            }
            u->stage = STAGE_DATA;
            return 0;
    }
}

static int entry_header_ready(adapt_archive_unpacker *u) {
    u->has_metadata = entry_has_metadata(u);
    if (u->has_metadata) {
        u->field = 0;
        u->stage = STAGE_METADATA;
        return 0;
    }
    return entry_ready(u);
}

/* An extent's offset and length are in: check them against the rules,
 * announce the extent, and take its bytes. */
static int extent_ready(adapt_archive_unpacker *u, uint64_t at) {
    int rc;
    if (u->extent_length == 0) {
        return refuse(u, "extent is empty", at);
    }
    if (u->extent_offset < u->extent_end) {
        return refuse(u, "extents out of order or overlapping", at);
    }
    if (u->extent_length > u->size || u->extent_offset > u->size - u->extent_length) {
        return refuse(u, "extent past the file's length", at);
    }
    u->extent_end = u->extent_offset + u->extent_length;
    rc = u->sink.extent(u->sink.context, u->extent_offset, u->extent_length);
    if (rc != 0) {
        return sink_failed(u, rc);
    }
    u->remaining = u->extent_length;
    u->stage = STAGE_DATA;
    return 0;
}

/* An attribute's value is in: check the name's order, hand it on, and
 * take the next or finish the entry. */
static int attribute_ready(adapt_archive_unpacker *u, uint64_t at) {
    int rc;
    if (u->attr_index > 0 && strcmp(u->name, u->previous_name) <= 0) {
        return refuse(u, "attributes out of byte order or repeated", at);
    }
    if (u->root_attributes) {
        if (u->root_attribute_callback == NULL) {
            return refuse(u, "root attribute callback is not set", at);
        }
        rc = u->root_attribute_callback(u->sink.context, u->name, u->value, u->value_len);
    } else {
        rc = u->sink.attribute(u->sink.context, u->path, u->kind, u->name, u->value, u->value_len);
    }
    if (rc != 0) {
        return sink_failed(u, rc);
    }
    strcpy(u->previous_name, u->name);
    if (++u->attr_index < u->attr_count) {
        u->field = 0;
        u->name_len = 0;
        u->stage = STAGE_ATTR_NAME_LEN;
        return 0;
    }
    return entry_done(u);
}

int adapt_archive_unpack(void *unpacker, const uint8_t *buf, size_t len) {
    adapt_archive_unpacker *u = unpacker;
    size_t i = 0;
    if (u->problem != NULL) {
        return ADAPT_ARCHIVE_MALFORMED;
    }
    if (u->failed != 0) {
        return u->failed;
    }
    while (i < len) {
        uint8_t b = buf[i];
        uint64_t at = u->offset + i;
        int rc;
        switch (u->stage) {
            case STAGE_MAGIC:
                if (u->field == ADAPT_ARCHIVE_MAGIC_BYTES - 1 && (b >= '1' && b <= '4')) {
                    u->version = b - '0';
                } else if (b != (uint8_t)ADAPT_ARCHIVE_MAGIC[u->field]) {
                    return refuse(u, "signature is not an adapt archive's", at);
                }
                if (++u->field == ADAPT_ARCHIVE_MAGIC_BYTES) {
                    u->field = 0;
                    u->root_attributes = u->version == 4;
                    u->stage = u->root_attributes ? STAGE_ATTR_COUNT : STAGE_KIND;
                }
                i++;
                break;
            case STAGE_KIND:
                if (b == ADAPT_ARCHIVE_END) {
                    u->stage = STAGE_DONE;
                } else if (b == ADAPT_ARCHIVE_DIRECTORY || b == ADAPT_ARCHIVE_FILE || b == ADAPT_ARCHIVE_SYMLINK ||
                           (u->version >= 3 && (b == ADAPT_ARCHIVE_HARDLINK || b == ADAPT_ARCHIVE_SPARSE))) {
                    u->kind = b;
                    u->field = 0;
                    u->path_len = 0;
                    u->stage = STAGE_PATH_LEN;
                } else {
                    return refuse(u, "kind is not an entry's", at);
                }
                i++;
                break;
            case STAGE_PATH_LEN:
                u->path_len |= (size_t)b << (8 * u->field);
                if (++u->field == 2) {
                    if (u->path_len == 0) {
                        return refuse(u, "path is empty", at);
                    }
                    u->field = 0;
                    u->path_at = 0;
                    u->component = 0;
                    u->stage = STAGE_PATH;
                }
                i++;
                break;
            case STAGE_PATH:
                if (b == 0) {
                    return refuse(u, "path holds a NUL", at);
                }
                if (b == '/') {
                    rc = close_component(u, at);
                    if (rc != 0) {
                        return rc;
                    }
                    u->component = u->path_at + 1;
                }
                u->path[u->path_at++] = (char)b;
                i++;
                if (u->path_at == u->path_len) {
                    rc = close_component(u, at);
                    if (rc != 0) {
                        return rc;
                    }
                    u->path[u->path_at] = '\0';
                    u->field = 0;
                    if (u->kind == ADAPT_ARCHIVE_FILE || u->kind == ADAPT_ARCHIVE_SPARSE) {
                        u->size = 0;
                        u->stage = STAGE_SIZE;
                    } else if (u->kind == ADAPT_ARCHIVE_SYMLINK || u->kind == ADAPT_ARCHIVE_HARDLINK) {
                        u->target_len = 0;
                        u->stage = STAGE_TARGET_LEN;
                    } else {
                        rc = entry_header_ready(u);
                        if (rc != 0) {
                            return rc;
                        }
                    }
                }
                break;
            case STAGE_SIZE:
                u->size |= (uint64_t)b << (8 * u->field);
                i++;
                if (++u->field == 8) {
                    u->field = 0;
                    if (u->kind == ADAPT_ARCHIVE_SPARSE) {
                        u->extent_count = 0;
                        u->stage = STAGE_EXTENT_COUNT;
                    } else {
                        rc = entry_header_ready(u);
                        if (rc != 0) {
                            return rc;
                        }
                    }
                }
                break;
            case STAGE_EXTENT_COUNT:
                u->extent_count |= (uint32_t)b << (8 * u->field);
                i++;
                if (++u->field == 4) {
                    if (u->extent_count > ADAPT_ARCHIVE_EXTENTS_MAX) {
                        return refuse(u, "too many extents", at);
                    }
                    rc = entry_header_ready(u);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
            case STAGE_DATA: {
                size_t n = len - i;
                if ((uint64_t)n > u->remaining) {
                    n = (size_t)u->remaining;
                }
                rc = u->sink.file_data(u->sink.context, buf + i, n);
                if (rc != 0) {
                    return sink_failed(u, rc);
                }
                i += n;
                u->remaining -= n;
                if (u->remaining == 0) {
                    rc = data_done(u);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
            }
            case STAGE_TARGET_LEN:
                u->target_len |= (size_t)b << (8 * u->field);
                if (++u->field == 2) {
                    if (u->target_len == 0) {
                        return refuse(u, "link target is empty", at);
                    }
                    u->field = 0;
                    u->target_at = 0;
                    u->stage = STAGE_TARGET;
                }
                i++;
                break;
            case STAGE_TARGET:
                if (b == 0) {
                    return refuse(u, "link target holds a NUL", at);
                }
                u->target[u->target_at++] = (char)b;
                i++;
                if (u->target_at == u->target_len) {
                    u->target[u->target_at] = '\0';
                    if (u->kind == ADAPT_ARCHIVE_HARDLINK) {
                        if (!adapt_archive_path_valid(u->target)) {
                            return refuse(u, "hard link target is not an archive path", at);
                        }
                        if (strcmp(u->target, u->path) == 0) {
                            return refuse(u, "hard link names itself", at);
                        }
                    }
                    rc = entry_header_ready(u);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
            case STAGE_METADATA:
                u->metadata_bytes[u->field++] = b;
                i++;
                if (u->field == ADAPT_METADATA_BYTES) {
                    if (!adapt_metadata_read(u->metadata_bytes, &u->metadata)) {
                        return refuse(u, "invalid permission bits or timestamp nanoseconds", at);
                    }
                    rc = entry_ready(u);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
            case STAGE_EXTENT_OFFSET:
                u->extent_offset |= (uint64_t)b << (8 * u->field);
                i++;
                if (++u->field == 8) {
                    u->field = 0;
                    u->extent_length = 0;
                    u->stage = STAGE_EXTENT_LENGTH;
                }
                break;
            case STAGE_EXTENT_LENGTH:
                u->extent_length |= (uint64_t)b << (8 * u->field);
                i++;
                if (++u->field == 8) {
                    rc = extent_ready(u, at);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
            case STAGE_ATTR_COUNT:
                u->attr_count |= (uint32_t)b << (8 * u->field);
                i++;
                if (++u->field == 2) {
                    if (u->attr_count == 0) {
                        rc = entry_done(u);
                        if (rc != 0) {
                            return rc;
                        }
                    } else {
                        u->field = 0;
                        u->name_len = 0;
                        u->stage = STAGE_ATTR_NAME_LEN;
                    }
                }
                break;
            case STAGE_ATTR_NAME_LEN:
                u->name_len |= (size_t)b << (8 * u->field);
                i++;
                if (++u->field == 2) {
                    if (u->name_len == 0 || u->name_len > ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX) {
                        return refuse(u, "attribute name is empty or too long", at);
                    }
                    u->name_at = 0;
                    u->stage = STAGE_ATTR_NAME;
                }
                break;
            case STAGE_ATTR_NAME:
                if (b == 0) {
                    return refuse(u, "attribute name holds a NUL", at);
                }
                u->name[u->name_at++] = (char)b;
                i++;
                if (u->name_at == u->name_len) {
                    u->name[u->name_at] = '\0';
                    u->field = 0;
                    u->value_len = 0;
                    u->stage = STAGE_ATTR_VALUE_LEN;
                }
                break;
            case STAGE_ATTR_VALUE_LEN:
                u->value_len |= (size_t)b << (8 * u->field);
                i++;
                if (++u->field == 4) {
                    if (u->value_len > ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX) {
                        return refuse(u, "attribute value too long", at);
                    }
                    u->value_at = 0;
                    if (u->value_len == 0) {
                        rc = attribute_ready(u, at);
                        if (rc != 0) {
                            return rc;
                        }
                    } else {
                        u->stage = STAGE_ATTR_VALUE;
                    }
                }
                break;
            case STAGE_ATTR_VALUE: {
                size_t n = len - i;
                if (n > u->value_len - u->value_at) {
                    n = u->value_len - u->value_at;
                }
                memcpy(u->value + u->value_at, buf + i, n);
                u->value_at += n;
                i += n;
                if (u->value_at == u->value_len) {
                    rc = attribute_ready(u, u->offset + i - 1);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
            }
            case STAGE_DONE:
                return refuse(u, "bytes after the end of the archive", at);
        }
    }
    u->offset += len;
    return 0;
}

adapt_status adapt_archive_finish(const adapt_archive_unpacker *u, adapt_error *error) {
    adapt_status status = ADAPT_OK;
    uint64_t at = 0;
    if (u->problem != NULL) {
        status = ADAPT_ERROR_BAD_ARCHIVE;
        at = u->problem_at;
    } else if (u->stage != STAGE_DONE) {
        status = ADAPT_ERROR_BAD_ARCHIVE;
        at = u->offset;
    }
    if (error != NULL) {
        error->status = status;
        error->cause = ADAPT_OK;
        error->detail = at;
        error->limit = 0;
    }
    return status;
}

const char *adapt_archive_problem(const adapt_archive_unpacker *u, uint64_t *offset) {
    if (u->problem != NULL) {
        if (offset != NULL) {
            *offset = u->problem_at;
        }
        return u->problem;
    }
    if (u->stage != STAGE_DONE && u->failed == 0) {
        /* not refused, but not complete either: what finish() reports */
        if (offset != NULL) {
            *offset = u->offset;
        }
        return "archive cut short";
    }
    if (offset != NULL) {
        *offset = 0;
    }
    return NULL;
}

uint64_t adapt_archive_entries(const adapt_archive_unpacker *u) {
    return u->entries;
}
