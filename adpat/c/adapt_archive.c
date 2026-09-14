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

/* ------------------------------------------------------------------------
 * The unpacker
 * --------------------------------------------------------------------- */

typedef enum stage {
    STAGE_MAGIC,
    STAGE_KIND,
    STAGE_PATH_LEN,
    STAGE_PATH,
    STAGE_SIZE,
    STAGE_DATA,
    STAGE_TARGET_LEN,
    STAGE_TARGET,
    STAGE_DONE
} stage;

struct adapt_archive_unpacker {
    adapt_archive_sink sink;
    stage stage;
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
};

adapt_archive_unpacker *adapt_archive_unpacker_new(adapt_archive_sink sink) {
    adapt_archive_unpacker *u = calloc(1, sizeof(*u));
    if (u == NULL) {
        return NULL;
    }
    u->path = malloc(ADAPT_ARCHIVE_PATH_MAX + 1);
    u->target = malloc(ADAPT_ARCHIVE_PATH_MAX + 1);
    if (u->path == NULL || u->target == NULL) {
        adapt_archive_unpacker_free(u);
        return NULL;
    }
    u->sink = sink;
    u->stage = STAGE_MAGIC;
    return u;
}

void adapt_archive_unpacker_free(adapt_archive_unpacker *u) {
    if (u == NULL) {
        return;
    }
    free(u->path);
    free(u->target);
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

/* The entry's header is complete: hand it on, and move to its bytes or
 * the next entry. */
static int entry_ready(adapt_archive_unpacker *u) {
    int rc;
    u->entries++;
    if (u->kind == ADAPT_ARCHIVE_DIRECTORY) {
        rc = u->sink.directory(u->sink.context, u->path);
        if (rc != 0) {
            return sink_failed(u, rc);
        }
        u->stage = STAGE_KIND;
        return 0;
    }
    if (u->kind == ADAPT_ARCHIVE_SYMLINK) {
        rc = u->sink.symlink(u->sink.context, u->path, u->target);
        if (rc != 0) {
            return sink_failed(u, rc);
        }
        u->stage = STAGE_KIND;
        return 0;
    }
    rc = u->sink.file_open(u->sink.context, u->path, u->size);
    if (rc != 0) {
        return sink_failed(u, rc);
    }
    u->remaining = u->size;
    if (u->remaining == 0) {
        rc = u->sink.file_close(u->sink.context);
        if (rc != 0) {
            return sink_failed(u, rc);
        }
        u->stage = STAGE_KIND;
    } else {
        u->stage = STAGE_DATA;
    }
    return 0;
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
                if (b != (uint8_t)ADAPT_ARCHIVE_MAGIC[u->field]) {
                    return refuse(u, "signature is not an adapt archive's", at);
                }
                if (++u->field == ADAPT_ARCHIVE_MAGIC_BYTES) {
                    u->field = 0;
                    u->stage = STAGE_KIND;
                }
                i++;
                break;
            case STAGE_KIND:
                if (b == ADAPT_ARCHIVE_END) {
                    u->stage = STAGE_DONE;
                } else if (b == ADAPT_ARCHIVE_DIRECTORY || b == ADAPT_ARCHIVE_FILE || b == ADAPT_ARCHIVE_SYMLINK) {
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
                    if (u->kind == ADAPT_ARCHIVE_FILE) {
                        u->size = 0;
                        u->stage = STAGE_SIZE;
                    } else if (u->kind == ADAPT_ARCHIVE_SYMLINK) {
                        u->target_len = 0;
                        u->stage = STAGE_TARGET_LEN;
                    } else {
                        rc = entry_ready(u);
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
                    rc = entry_ready(u);
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
                    rc = u->sink.file_close(u->sink.context);
                    if (rc != 0) {
                        return sink_failed(u, rc);
                    }
                    u->stage = STAGE_KIND;
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
                    rc = entry_ready(u);
                    if (rc != 0) {
                        return rc;
                    }
                }
                break;
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
