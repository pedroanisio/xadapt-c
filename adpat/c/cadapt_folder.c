/*
 * cadapt_folder.c — the folder side of the command line: the walk of a
 * directory into an archive stream, the restore of one into a staged
 * directory, and the staged directory itself. See cadapt_folder.h.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 * Extracted from cadapt.c, 2026-09-17: the code is unchanged, its types
 * moved to the header, and the functions the commands call lost their
 * `static`.
 */
#include "cadapt_platform.h"

#include "cadapt_folder.h"

/* ------------------------------------------------------------------------
 * Folders: a directory walked into an archive stream on the way in, an
 * archive stream restored into a staged directory on the way out.
 * --------------------------------------------------------------------- */

/* `a/b` from `a` and `b`; `b` alone when `a` is empty. Malloc'd. */
static char *join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *p = malloc(na + nb + 2);
    if (p == NULL) {
        return NULL;
    }
    if (na == 0) {
        memcpy(p, b, nb + 1);
        return p;
    }
    memcpy(p, a, na);
    p[na] = '/';
    memcpy(p + na + 1, b, nb + 1);
    return p;
}

/* The kind of the entry at `path`, its size when a file, and its target
 * when a link (malloc'd); KIND_NONE for anything else, or -1 with errno
 * when it cannot be examined. A link is examined, not followed. */
static int examine(const char *path, uint64_t *size, char **target, identity *id) {
#ifdef _WIN32
    struct _stat64 st;
    (void)target; /* no links on Windows: none is packed */
    if (_stat64(path, &st) != 0) {
        return -1;
    }
    id->device = (uint64_t)st.st_dev;
    id->inode = (uint64_t)st.st_ino;
    id->shared = 0;
    if (st.st_mode & _S_IFDIR) {
        return KIND_DIRECTORY;
    }
    if (st.st_mode & _S_IFREG) {
        *size = (uint64_t)st.st_size;
        return KIND_FILE;
    }
    return KIND_NONE;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        return -1;
    }
    id->device = (uint64_t)st.st_dev;
    id->inode = (uint64_t)st.st_ino;
    id->shared = st.st_nlink > 1;
    if (S_ISDIR(st.st_mode)) {
        return KIND_DIRECTORY;
    }
    if (S_ISREG(st.st_mode)) {
        *size = (uint64_t)st.st_size;
        return KIND_FILE;
    }
    if (S_ISLNK(st.st_mode)) {
        size_t cap = 256;
        for (;;) {
            char *buf = malloc(cap);
            ssize_t n;
            if (buf == NULL) {
                errno = ENOMEM;
                return -1;
            }
            n = readlink(path, buf, cap);
            if (n < 0) {
                int e = errno;
                free(buf);
                errno = e;
                return -1;
            }
            if ((size_t)n < cap) {
                buf[n] = '\0';
                *target = buf;
                return KIND_SYMLINK;
            }
            free(buf);
            cap *= 2;
        }
    }
    return KIND_NONE;
#endif
}

int is_directory(const char *path) {
    uint64_t size;
    char *target = NULL;
    identity id;
    int kind = examine(path, &size, &target, &id);
    free(target);
    return kind == KIND_DIRECTORY;
}

/* The names in a directory, sorted by their bytes, each malloc'd; or -1
 * with errno. `.` and `..` are left out. */
static int compare_names(const void *a, const void *b) {
    const unsigned char *x = *(const unsigned char *const *)a, *y = *(const unsigned char *const *)b;
    size_t i;
    for (i = 0;; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
        if (x[i] == 0) {
            return 0;
        }
    }
}

static int list_directory(const char *path, char ***names, size_t *count) {
    char **list = NULL;
    size_t n = 0, cap = 0;
#ifdef _WIN32
    struct _finddata_t entry;
    intptr_t handle;
    char *pattern = join(path, "*");
    if (pattern == NULL) {
        errno = ENOMEM;
        return -1;
    }
    handle = _findfirst(pattern, &entry);
    free(pattern);
    if (handle == -1) {
        if (errno == ENOENT) {
            *names = NULL;
            *count = 0;
            return 0;
        }
        return -1;
    }
    do {
        const char *name = entry.name;
#else
    DIR *dir = opendir(path);
    struct dirent *entry;
    if (dir == NULL) {
        return -1;
    }
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
#endif
        char *copy;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (n == cap) {
            size_t grown = cap == 0 ? 16 : cap * 2;
            char **bigger = realloc(list, grown * sizeof(*list));
            if (bigger == NULL) {
                break;
            }
            list = bigger;
            cap = grown;
        }
        copy = malloc(strlen(name) + 1);
        if (copy == NULL) {
            break;
        }
        strcpy(copy, name);
        list[n++] = copy;
#ifdef _WIN32
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    }
    if (errno != 0) {
        int e = errno;
        size_t i;
        closedir(dir);
        for (i = 0; i < n; i++) {
            free(list[i]);
        }
        free(list);
        errno = e;
        return -1;
    }
    closedir(dir);
#endif
    if (n > 0) {
        qsort(list, n, sizeof(*list), compare_names);
    }
    *names = list;
    *count = n;
    return 0;
}

void folder_free(folder *f) {
    size_t i;
    for (i = 0; i < f->count; i++) {
        free(f->items[i].path);
        free(f->items[i].target);
        free(f->items[i].extents);
        attrs_free(&f->items[i].attrs);
    }
    free(f->items);
    free(f->inodes);
    attrs_free(&f->root_attrs);
    f->items = NULL;
    f->inodes = NULL;
    f->count = f->cap = f->inode_count = f->inode_cap = 0;
}

/* The inode table is a hash table — open addressing over (device, inode),
 * a power of two of slots never more than half full — so a snapshot tree
 * of a million hard-linked names costs a million lookups, not a million
 * scans of everything seen so far (the flat list it replaced was measured
 * quadratic: 0.3 s over 30,000 inodes, 6.3 s over 100,000). */
static size_t inode_slot(const identity *id, size_t mask) {
    uint64_t h = id->inode ^ (id->device * 0x9E3779B97F4A7C15ull);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return (size_t)h & mask;
}

/* The entry an inode already has, or -1. */
static long folder_inode(const folder *f, const identity *id) {
    size_t mask, i;
    if (f->inode_cap == 0) {
        return -1;
    }
    mask = f->inode_cap - 1;
    for (i = inode_slot(id, mask); f->inodes[i].index != 0; i = (i + 1) & mask) {
        if (f->inodes[i].id.device == id->device && f->inodes[i].id.inode == id->inode) {
            return (long)(f->inodes[i].index - 1);
        }
    }
    return -1;
}

static void inode_insert(seen_inode *slots, size_t mask, const identity *id, size_t index) {
    size_t i = inode_slot(id, mask);
    while (slots[i].index != 0) {
        i = (i + 1) & mask;
    }
    slots[i].id = *id;
    slots[i].index = index + 1;
}

static int folder_remember_inode(folder *f, const identity *id, size_t index) {
    if (2 * (f->inode_count + 1) > f->inode_cap) {
        size_t grown = f->inode_cap == 0 ? 64 : f->inode_cap * 2, i;
        seen_inode *bigger = calloc(grown, sizeof(*bigger));
        if (bigger == NULL) {
            return ENOMEM;
        }
        for (i = 0; i < f->inode_cap; i++) {
            if (f->inodes[i].index != 0) {
                inode_insert(bigger, grown - 1, &f->inodes[i].id, f->inodes[i].index - 1);
            }
        }
        free(f->inodes);
        f->inodes = bigger;
        f->inode_cap = grown;
    }
    inode_insert(f->inodes, f->inode_cap - 1, id, index);
    f->inode_count++;
    return 0;
}

/* Add an entry; takes the strings, the extents and the attributes.
 * Returns 0 or an errno. */
static int folder_add(folder *f, uint8_t kind, char *path, uint64_t size, char *target, adapt_extent *extents,
                      uint32_t extent_count, attrs *a, const adapt_metadata *metadata) {
    entry *e;
    size_t n;
    uint32_t i;
    if (f->count == f->cap) {
        size_t grown = f->cap == 0 ? 64 : f->cap * 2;
        entry *bigger = realloc(f->items, grown * sizeof(*bigger));
        if (bigger == NULL) {
            free(path);
            free(target);
            free(extents);
            attrs_free(a);
            return ENOMEM;
        }
        f->items = bigger;
        f->cap = grown;
    }
    n = adapt_archive_entry_full(kind, path, size, target, extents, extent_count, metadata, NULL, 0);
    if (n == 0) {
        free(path);
        free(target);
        free(extents);
        attrs_free(a);
        return ENAMETOOLONG;
    }
    e = &f->items[f->count++];
    e->kind = kind;
    e->path = path;
    e->size = size;
    e->target = target;
    e->metadata = *metadata;
    e->extents = extents;
    e->extent_count = extent_count;
    e->attrs = *a;
    memset(a, 0, sizeof(*a));
    f->stream_bytes += n;
    if (kind == KIND_FILE) {
        f->stream_bytes += size;
    } else if (kind == KIND_SPARSE) {
        for (i = 0; i < extent_count; i++) {
            f->stream_bytes += 16 + extents[i].length;
        }
    }
    if (kind != KIND_HARDLINK) {
        f->stream_bytes += attrs_bytes(&e->attrs);
    }
    return 0;
}

/* Walk the directory at root/rel, depth first, siblings in byte order,
 * each directory before what it holds. Returns 0, or an errno with
 * *culprit the path that failed (malloc'd) — an entry of a kind the
 * archive cannot hold is ENOTSUP. */
static int walk(folder *f, const char *rel, char **culprit) {
    char *dir = join(f->root, rel);
    char **names = NULL;
    size_t count = 0, i;
    int rc = 0;
    if (dir == NULL) {
        return ENOMEM;
    }
    if (list_directory(dir, &names, &count) != 0) {
        rc = errno;
        *culprit = dir;
        return rc;
    }
    free(dir);
    for (i = 0; i < count && rc == 0; i++) {
        char *path = join(rel, names[i]);
        char *full = path == NULL ? NULL : join(f->root, path);
        char *target = NULL;
        uint64_t size = 0;
        adapt_metadata metadata;
        adapt_extent *extents = NULL;
        uint32_t extent_count = UINT32_MAX;
        attrs a;
        identity id;
        int kind;
        memset(&a, 0, sizeof(a));
        if (path == NULL || full == NULL) {
            free(path);
            free(full);
            rc = ENOMEM;
            break;
        }
        rc = metadata_read_path(full, 1, &metadata);
        if (rc != 0) {
            *culprit = full;
            free(path);
            break;
        }
        kind = examine(full, &size, &target, &id);
        if (kind < 0) {
            rc = errno;
            *culprit = full;
            free(path);
            break;
        }
        if (kind == KIND_NONE) {
            rc = ENOTSUP;
            *culprit = full;
            free(path);
            break;
        }
        if (kind == KIND_FILE && id.shared) {
            /* a name an earlier entry's inode already has is a hard link
             * to that entry; the first name carries the bytes */
            long earlier = folder_inode(f, &id);
            if (earlier >= 0) {
                char *shared = malloc(strlen(f->items[earlier].path) + 1);
                if (shared == NULL) {
                    free(full);
                    free(path);
                    rc = ENOMEM;
                    break;
                }
                strcpy(shared, f->items[earlier].path);
                free(full);
                rc = folder_add(f, KIND_HARDLINK, path, 0, shared, NULL, 0, &a, &metadata);
                continue;
            }
            rc = folder_remember_inode(f, &id, f->count);
            if (rc != 0) {
                free(full);
                free(path);
                break;
            }
        }
        if (kind == KIND_FILE && size > 0) {
            rc = sparse_map(full, size, &extents, &extent_count);
            if (rc != 0) {
                *culprit = full;
                free(path);
                free(target);
                break;
            }
            if (extent_count != UINT32_MAX) {
                kind = KIND_SPARSE;
            }
        }
        rc = attrs_read(full, &a);
        if (rc != 0) {
            *culprit = full;
            free(path);
            free(target);
            free(extents);
            break;
        }
        free(full);
        if (kind == KIND_DIRECTORY) {
            char *again = malloc(strlen(path) + 1);
            if (again == NULL) {
                free(path);
                attrs_free(&a);
                rc = ENOMEM;
                break;
            }
            strcpy(again, path);
            rc = folder_add(f, KIND_DIRECTORY, path, 0, NULL, NULL, 0, &a, &metadata);
            if (rc == 0) {
                rc = walk(f, again, culprit);
            }
            free(again);
        } else {
            rc = folder_add(f, (uint8_t)kind, path, size, target, extents, kind == KIND_SPARSE ? extent_count : 0, &a,
                            &metadata);
        }
    }
    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    return rc;
}

/* Collect a folder's entries and the length of its stream. */
int folder_collect(folder *f, const char *root, char **culprit) {
    int rc;
    memset(f, 0, sizeof(*f));
    f->root = root;
    rc = attrs_read(root, &f->root_attrs);
    if (rc != 0) return rc;
    f->stream_bytes = ADAPT_ARCHIVE_MAGIC_BYTES + attrs_bytes(&f->root_attrs) + 1;
    return walk(f, "", culprit);
}

void packer_reset(packer *p, folder *f) {
    if (p->file != NULL) {
        fclose(p->file);
    }
    free(p->trailer);
    memset(p, 0, sizeof(*p));
    p->f = f;
    p->stage = PACK_MAGIC;
    memcpy(p->head, ADAPT_ARCHIVE_ROOT_MAGIC, ADAPT_ARCHIVE_MAGIC_BYTES);
    p->head_len = ADAPT_ARCHIVE_MAGIC_BYTES;
}

void packer_close(packer *p) {
    if (p->file != NULL) {
        fclose(p->file);
        p->file = NULL;
    }
    free(p->trailer);
    p->trailer = NULL;
}

/* Move to the next entry's header, or the end. */
static void packer_next(packer *p) {
    if (p->index == p->f->count) {
        p->head[0] = ADAPT_ARCHIVE_END;
        p->head_len = 1;
        p->head_at = 0;
        p->stage = PACK_END;
        return;
    }
    {
        const entry *e = &p->f->items[p->index];
        p->head_len = adapt_archive_entry_full(e->kind, e->path, e->size, e->target, e->extents, e->extent_count,
                                               &e->metadata, p->head, sizeof(p->head));
        p->head_at = 0;
        p->extent = 0;
        p->stage = PACK_HEADER;
    }
}

/* The entry's attribute trailer, built once and sent from `trailer`.
 * Returns 0 or an errno. */
static int packer_attrs(packer *p, const attrs *a) {
    size_t n = (size_t)attrs_bytes(a), at;
    uint32_t i;
    uint8_t *t = malloc(n);
    if (t == NULL) {
        p->error = ENOMEM;
        return ENOMEM;
    }
    at = adapt_archive_attributes(a->count, t, n);
    for (i = 0; i < a->count; i++) {
        at += adapt_archive_attribute(a->names[i], a->values[i], a->lens[i], t + at, n - at);
    }
    free(p->trailer);
    p->trailer = t;
    p->trailer_len = n;
    p->trailer_at = 0;
    p->stage = PACK_ATTRS;
    return 0;
}

/* Open the entry's file for its bytes. Returns 0 or an errno. */
static int packer_open(packer *p) {
    const entry *e = &p->f->items[p->index];
    char *full = join(p->f->root, e->path);
    if (full == NULL) {
        p->error = ENOMEM;
        return ENOMEM;
    }
    p->file = fopen(full, "rb");
    free(full);
    if (p->file == NULL) {
        p->error = errno;
        p->failed = e->path;
        return p->error != 0 ? p->error : EIO;
    }
    return 0;
}

/* The next extent of a sparse entry: its offset and length through
 * `head`, the file positioned at its bytes. Returns 0 or an errno. */
static int packer_extent(packer *p) {
    const entry *e = &p->f->items[p->index];
    const adapt_extent *x = &e->extents[p->extent];
    p->head_len = adapt_archive_extent(x, p->head, sizeof(p->head));
    p->head_at = 0;
    if (cadapt_fseek(p->file, (cadapt_off)x->offset, SEEK_SET) != 0) {
        p->error = errno;
        p->failed = e->path;
        return p->error != 0 ? p->error : EIO;
    }
    p->remaining = x->length;
    p->stage = PACK_EXTENT;
    return 0;
}

int read_packer(void *context, uint8_t *buf, size_t len, size_t *got) {
    packer *p = context;
    size_t at = 0;
    while (at < len) {
        if (cancelled()) return p->error = EINTR;
        if (p->stage == PACK_DONE) {
            break;
        }
        if (p->stage == PACK_ATTRS || p->stage == PACK_ROOT_ATTRS) {
            size_t n = p->trailer_len - p->trailer_at;
            if (n > len - at) {
                n = len - at;
            }
            memcpy(buf + at, p->trailer + p->trailer_at, n);
            at += n;
            p->trailer_at += n;
            if (p->trailer_at < p->trailer_len) {
                break;
            }
            packer_close(p);
            if (p->stage == PACK_ATTRS) p->index++;
            packer_next(p);
            continue;
        }
        if (p->stage == PACK_MAGIC || p->stage == PACK_HEADER || p->stage == PACK_EXTENT || p->stage == PACK_END) {
            size_t n = p->head_len - p->head_at;
            const entry *e = p->index < p->f->count ? &p->f->items[p->index] : NULL;
            int rc = 0;
            if (n > len - at) {
                n = len - at;
            }
            memcpy(buf + at, p->head + p->head_at, n);
            at += n;
            p->head_at += n;
            if (p->head_at < p->head_len) {
                break;
            }
            if (p->stage == PACK_END) {
                p->stage = PACK_DONE;
            } else if (p->stage == PACK_MAGIC) {
                rc = packer_attrs(p, &p->f->root_attrs);
                p->stage = PACK_ROOT_ATTRS;
            } else if (p->stage == PACK_EXTENT) {
                p->stage = PACK_DATA;
            } else if (e == NULL) {
                /* packer_next() enters PACK_HEADER only with an entry */
                rc = EIO;
            } else if (e->kind == KIND_FILE) {
                rc = packer_open(p);
                p->remaining = e->size;
                p->stage = PACK_DATA;
            } else if (e->kind == KIND_SPARSE) {
                rc = packer_open(p);
                if (rc == 0) {
                    rc = e->extent_count == 0 ? packer_attrs(p, &e->attrs) : packer_extent(p);
                }
            } else if (e->kind == KIND_HARDLINK) {
                p->index++;
                packer_next(p);
            } else {
                rc = packer_attrs(p, &e->attrs);
            }
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        /* PACK_DATA: the file's or the extent's bytes, exactly as many as
         * were listed */
        {
            const entry *e = &p->f->items[p->index];
            size_t n = len - at;
            size_t r;
            int rc;
            if ((uint64_t)n > p->remaining) {
                n = (size_t)p->remaining;
            }
            r = n == 0 ? 0 : fread(buf + at, 1, n, p->file);
            if (r < n) {
                p->error = ferror(p->file) ? errno : 0;
                p->failed = e->path;
                p->changed = !ferror(p->file);
                return p->error != 0 ? p->error : EIO;
            }
            at += r;
            p->remaining -= r;
            if (p->remaining == 0) {
                if (e->kind == KIND_SPARSE && ++p->extent < e->extent_count) {
                    rc = packer_extent(p);
                    if (rc != 0) {
                        return rc;
                    }
                    continue;
                }
                /* the file must end where its listing said */
                if (e->kind == KIND_SPARSE) {
                    if (cadapt_fseek(p->file, 0, SEEK_END) != 0 || (uint64_t)cadapt_ftell(p->file) != e->size) {
                        p->failed = e->path;
                        p->changed = 1;
                        return EIO;
                    }
                } else if (fgetc(p->file) != EOF) {
                    p->failed = e->path;
                    p->changed = 1;
                    return EIO;
                }
                rc = packer_attrs(p, &e->attrs);
                if (rc != 0) {
                    return rc;
                }
            }
        }
    }
    *got = at;
    return 0;
}

/* The message for a folder's read failure. */
int packer_failure(const char *input, const packer *p) {
    char message[400];
    if (p->failed != NULL && p->changed) {
        snprintf(message, sizeof(message), "%s changed while it was being coded", p->failed);
    } else if (p->failed != NULL) {
        snprintf(message, sizeof(message), "%s: %s", p->failed, strerror(p->error != 0 ? p->error : EIO));
    } else {
        snprintf(message, sizeof(message), "%s", strerror(p->error != 0 ? p->error : EIO));
    }
    return other_error(input, message);
}

/* The sink that restores an archive under a staging directory. Every
 * path has passed the unpacker's syntax rules; filesystem parents still
 * have to be resolved without following an archive's symbolic links. */

/* Reserve apparent file bytes before creating a file or link. Holes and
 * hard-link aliases count too; subtraction avoids cumulative overflow. */
static int restore_reserve(restorer *r, uint64_t size) {
    if (size > r->max_output - r->restored_size) {
        return EFBIG;
    }
    r->restored_size += size;
    return 0;
}

int restore_fail(restorer *r, const char *path, int e, const char *why) {
    r->error = e;
    r->why = why;
    free(r->failed);
    r->failed = malloc(strlen(path) + 1);
    if (r->failed != NULL) {
        strcpy(r->failed, path);
    }
    return e != 0 ? e : EIO;
}

static int journal_write(FILE *file, const void *data, size_t len) {
    return fwrite(data, 1, len, file) == len ? 0 : (errno != 0 ? errno : EIO);
}

static int journal_read(FILE *file, void *data, size_t len) {
    return fread(data, 1, len, file) == len ? 0 : EIO;
}

static int journal_flush_record(restorer *r) {
    int rc = 0;
    if (r->pending_path != NULL) {
        rc = journal_write(r->journal, r->pending_path, r->pending.path_bytes);
        if (rc == 0) rc = journal_write(r->journal, &r->pending, sizeof(r->pending));
        free(r->pending_path);
        r->pending_path = NULL;
    }
    return rc;
}

static int journal_begin(restorer *r, const char *path) {
    size_t len = strlen(path) + 1;
    int rc = journal_flush_record(r);
    if (rc != 0) return rc;
    if (len > ADAPT_ARCHIVE_PATH_MAX + 1u) return ENAMETOOLONG;
    if (r->journal == NULL) {
        r->journal = tmpfile();
        if (r->journal == NULL) return errno != 0 ? errno : EIO;
    }
    memset(&r->pending, 0, sizeof(r->pending));
    r->pending.start = cadapt_ftell(r->journal);
    if (r->pending.start < 0) return errno != 0 ? errno : EIO;
    r->pending_path = malloc(len);
    if (r->pending_path == NULL) return ENOMEM;
    memcpy(r->pending_path, path, len);
    r->pending.path_bytes = (uint32_t)len;
    return 0;
}

static int journal_attribute(restorer *r, const char *name, const uint8_t *value, size_t len) {
    size_t name_len = strlen(name) + 1;
    uint32_t lengths[2];
    int rc;
    if (name_len > ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 1u || len > ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX ||
        r->pending.attributes == UINT32_MAX) return EINVAL;
    lengths[0] = (uint32_t)name_len;
    lengths[1] = (uint32_t)len;
    rc = journal_write(r->journal, lengths, sizeof(lengths));
    if (rc == 0) rc = journal_write(r->journal, name, name_len);
    if (rc == 0) rc = journal_write(r->journal, value, len);
    if (rc == 0) r->pending.attributes++;
    return rc;
}

#ifndef _WIN32
/* Return the entry's parent descriptor, rooted in our private staging
 * directory. Every component must be a real directory, never a symlink.
 * The caller owns both the descriptor and the storage holding *leaf. */
static int restore_parent(restorer *r, const char *path, char **storage, char **leaf) {
    char *part, *slash;
    int parent;
    long path_limit;
    *storage = NULL;
    if (!adapt_archive_path_valid(path)) {
        errno = EINVAL;
        return -1;
    }
    /* Keep the CLI's existing path-length bound while traversing by fd. */
    path_limit = fpathconf(r->root_fd, _PC_PATH_MAX);
    if (path_limit < 0) {
#ifdef PATH_MAX
        path_limit = PATH_MAX;
#else
        errno = ENOTSUP;
        return -1;
#endif
    }
    if (strlen(r->root) + 1 + strlen(path) >= (size_t)path_limit) {
        errno = ENAMETOOLONG;
        return -1;
    }
    *storage = strdup(path);
    if (*storage == NULL) {
        return -1;
    }
    parent = fcntl(r->root_fd, F_DUPFD_CLOEXEC, 3);
    part = *storage;
    while (parent >= 0 && (slash = strchr(part, '/')) != NULL) {
        int next, e;
        *slash = '\0';
        next = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        e = errno;
        close(parent);
        parent = next;
        errno = e;
        part = slash + 1;
    }
    *leaf = part;
    return parent;
}
#else
/* Archive separators are '/', including on Windows. Reject native
 * separators and drive/stream syntax before handing paths to the CRT. */
static int restore_windows_path(const char *path) {
    return adapt_archive_path_valid(path) && strchr(path, '\\') == NULL && strchr(path, ':') == NULL;
}
#endif

/* Deferred records use archive-relative paths on both platforms. */
static int restore_apply_metadata(restorer *r, const char *path, int symlink_entry, const adapt_metadata *m,
                                  const attrs *attributes) {
#ifdef _WIN32
    char *full = join(r->root, path);
    int rc = full == NULL ? ENOMEM : attributes != NULL && attributes->count ? ENOTSUP
                                    : m == NULL ? 0 : metadata_apply(full, symlink_entry, m);
    free(full);
    return rc;
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf), rc;
    if (parent < 0) {
        rc = errno;
    } else if (symlink_entry) {
        /* the link's own times, and its mode where the platform keeps one
         * (CADAPT_LINK_MODE); elsewhere the mode is the platform's */
        struct timespec times[2];
        struct stat st;
        rc = metadata_times(m, times);
        if (rc == 0 && utimensat(parent, leaf, times, AT_SYMLINK_NOFOLLOW) != 0) rc = errno;
#if CADAPT_LINK_MODE
        if (rc == 0 && fchmodat(parent, leaf, (mode_t)m->mode, AT_SYMLINK_NOFOLLOW) != 0) rc = errno;
#endif
        if (rc == 0 && fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) rc = errno;
        if (rc == 0) rc = metadata_check_stat(&st, m, CADAPT_LINK_MODE);
    } else {
        int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        uint32_t i;
        rc = fd < 0 ? errno : 0;
        for (i = 0; attributes != NULL && i < attributes->count && rc == 0; i++) {
            rc = attr_apply(fd, attributes->names[i], attributes->values[i], attributes->lens[i]);
        }
        if (rc == 0) {
            if (m != NULL) rc = metadata_apply_fd(fd, m);
            else if (fchmod(fd, (mode_t)r->directory_mode) != 0) rc = errno;
        }
        if (fd >= 0) close(fd);
    }
    if (parent >= 0) close(parent);
    free(storage);
    return rc;
#endif
}

static int restore_apply_attribute(restorer *r, const char *path, int symlink_entry, const char *name,
                                    const uint8_t *value, size_t len) {
#ifdef _WIN32
    (void)r;
    (void)path;
    (void)symlink_entry;
    return attr_apply(-1, name, value, len);
#else
    char *storage, *leaf;
    int parent, fd = -1, rc, flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    if (symlink_entry) {
#ifdef __APPLE__
        flags |= O_SYMLINK;
#else
        /* Linux does not allow the preserved user/ACL namespaces on links. */
        return ENOTSUP;
#endif
    }
    parent = restore_parent(r, path, &storage, &leaf);
    if (parent >= 0) fd = openat(parent, leaf, flags);
    rc = fd < 0 ? errno : attr_apply(fd, name, value, len);
    if (fd >= 0) close(fd);
    if (parent >= 0) close(parent);
    free(storage);
    return rc;
#endif
}

int restore_directory(void *context, const char *path) {
    restorer *r = context;
    int rc;
#ifdef _WIN32
    char *full;
    if (!restore_windows_path(path)) return restore_fail(r, path, EINVAL, "invalid archive path");
    full = join(r->root, path);
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    rc = _mkdir(full) == 0 ? 0 : errno;
    free(full);
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf);
    rc = parent < 0 ? errno : (mkdirat(parent, leaf, 0700) == 0 ? 0 : errno);
    /* The entry was just created inside the private wrapper. Grant the
     * owner access even with umask 0777; final modes wait for children. */
    if (rc == 0 && fchmodat(parent, leaf, 0700, 0) != 0) rc = errno;
    if (parent >= 0) close(parent);
    free(storage);
#endif
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EEXIST ? "named twice in the archive" : NULL);
    }
    rc = journal_begin(r, path);
    return rc != 0 ? restore_fail(r, path, rc, "cannot defer directory metadata") : 0;
}

int restore_file_open(void *context, const char *path, uint64_t size) {
    restorer *r = context;
    int fd, e;
    if (restore_reserve(r, size) != 0) {
        return restore_fail(r, path, EFBIG, "restored file sizes exceed --max-output");
    }
#ifdef _WIN32
    char *full;
    if (!restore_windows_path(path)) return restore_fail(r, path, EINVAL, "invalid archive path");
    full = join(r->root, path);
    if (full == NULL) {
        return restore_fail(r, path, ENOMEM, NULL);
    }
    /* exclusive: an archive that names a file twice is refused */
    fd = _open(full, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    e = errno;
    free(full);
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf);
    fd = parent < 0 ? -1 : openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    e = errno;
    if (fd >= 0 && fchmod(fd, 0600) != 0) {
        e = errno;
        close(fd);
        fd = -1;
    }
    if (parent >= 0) close(parent);
    free(storage);
#endif
    if (fd < 0) {
        return restore_fail(r, path, e, e == EEXIST ? "named twice in the archive" : NULL);
    }
#ifdef _WIN32
    r->file = _fdopen(fd, "wb");
#else
    r->file = fdopen(fd, "wb");
#endif
    if (r->file == NULL) {
        int e = errno;
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return restore_fail(r, path, e, NULL);
    }
    free(r->failed);
    r->failed = malloc(strlen(path) + 1);
    if (r->failed != NULL) {
        strcpy(r->failed, path);
    }
    return 0;
}

int restore_file_data(void *context, const uint8_t *data, size_t len) {
    restorer *r = context;
    if (fwrite(data, 1, len, r->file) != len) {
        r->error = errno;
        return r->error != 0 ? r->error : EIO;
    }
    return 0;
}

#ifndef _WIN32
/* Metadata callbacks close the handle on newer archives. A remaining
 * handle belongs to a version 1 file, or an entry whose parsing failed. */
int finish_legacy_file(restorer *r, int apply) {
    int rc = 0;
    free(r->file_acl);
    r->file_acl = NULL;
    if (r->entry_fd >= 0) {
        if (apply && fchmod(r->entry_fd, (mode_t)r->file_mode) != 0) rc = errno;
        close(r->entry_fd);
        r->entry_fd = -1;
    }
    return rc;
}
#endif

int restore_file_close(void *context) {
    restorer *r = context;
    FILE *f = r->file;
    r->file = NULL;
#ifndef _WIN32
    /* ACLs can remove owner read permission. Reopening by name afterward
     * would reject a valid archive; keep this inode open until metadata. */
    r->error = finish_legacy_file(r, 1);
    if (r->error != 0) {
        fclose(f);
        return r->error;
    }
    r->entry_fd = fcntl(fileno(f), F_DUPFD_CLOEXEC, 3);
    if (r->entry_fd < 0) {
        r->error = errno;
        fclose(f);
        return r->error;
    }
#endif
    if (fclose(f) != 0) {
        r->error = errno;
        return r->error != 0 ? r->error : EIO;
    }
    free(r->failed);
    r->failed = NULL;
    return 0;
}

int restore_symlink(void *context, const char *path, const char *target) {
    restorer *r = context;
#ifdef _WIN32
    (void)target;
    return restore_fail(r, path, ENOTSUP, "a symbolic link is not restored on this platform");
#else
    char *storage, *leaf;
    int parent = restore_parent(r, path, &storage, &leaf);
    int rc = parent < 0 ? errno : (symlinkat(target, parent, leaf) == 0 ? 0 : errno);
    if (parent >= 0) close(parent);
    free(storage);
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EEXIST ? "named twice in the archive" : NULL);
    }
    return 0;
#endif
}

/* A hard link: the same inode under another name. Where the platform has
 * no link(), the target's bytes are copied instead, which the README
 * states. */
int restore_hardlink(void *context, const char *path, const char *target) {
    restorer *r = context;
    int rc;
#ifdef _WIN32
    char *full = join(r->root, path);
    char *to = join(r->root, target);
    if (!restore_windows_path(path) || !restore_windows_path(target)) {
        free(full);
        free(to);
        return restore_fail(r, path, EINVAL, "invalid archive path");
    }
    if (full == NULL || to == NULL) {
        free(full);
        free(to);
        return restore_fail(r, path, ENOMEM, NULL);
    }
    {
        FILE *in = fopen(to, "rb"), *out = NULL;
        struct _stat64 st;
        uint8_t buf[65536];
        size_t n;
        rc = 0;
        if (in == NULL) {
            rc = errno;
        } else if (_fstat64(_fileno(in), &st) != 0) {
            rc = errno;
        } else if ((st.st_mode & _S_IFMT) != _S_IFREG || st.st_size < 0) {
            rc = EINVAL;
        } else {
            rc = restore_reserve(r, (uint64_t)st.st_size);
        }
        if (rc == 0) {
            int fd = _open(full, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
            out = fd < 0 ? NULL : _fdopen(fd, "wb");
            if (out == NULL) {
                rc = errno;
            }
        }
        while (rc == 0 && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                rc = errno != 0 ? errno : EIO;
            }
        }
        if (in != NULL && ferror(in)) {
            rc = EIO;
        }
        if (in != NULL) {
            fclose(in);
        }
        if (out != NULL && fclose(out) != 0 && rc == 0) {
            rc = errno;
        }
    }
    free(full);
    free(to);
#else
    char *storage, *leaf, *target_storage = NULL, *target_leaf;
    int parent = restore_parent(r, path, &storage, &leaf), source = -1;
    struct stat st;
    rc = parent < 0 ? errno : 0;
    if (rc == 0) {
        source = restore_parent(r, target, &target_storage, &target_leaf);
        if (source < 0) rc = errno;
        else if (fstatat(source, target_leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) rc = errno;
        else if (!S_ISREG(st.st_mode) || st.st_size < 0) rc = EINVAL;
        else {
            rc = restore_reserve(r, (uint64_t)st.st_size);
            if (rc == 0 && linkat(source, target_leaf, parent, leaf, 0) != 0) rc = errno;
        }
    }
    if (source >= 0) close(source);
    if (parent >= 0) close(parent);
    free(target_storage);
    free(storage);
#endif
    if (rc != 0) {
        return restore_fail(r, path, rc, rc == EFBIG ? "restored file sizes exceed --max-output" :
                            rc == EEXIST ? "named twice in the archive" : "cannot restore hard link");
    }
    return 0;
}

/* A sparse file: created empty at its apparent length, each extent
 * written at its offset. Where the filesystem keeps no holes, the bytes
 * between extents are zeros all the same. */
int restore_sparse_open(void *context, const char *path, uint64_t size, uint32_t extents) {
    restorer *r = context;
    int rc = restore_file_open(context, path, size);
    (void)extents;
    if (rc != 0) {
        return rc;
    }
#ifdef _WIN32
    rc = _chsize_s(_fileno(r->file), (long long)size) == 0 ? 0 : errno;
#else
    rc = ftruncate(fileno(r->file), (off_t)size) == 0 ? 0 : errno;
#endif
    if (rc != 0) {
        return restore_fail(r, path, rc, "cannot restore sparse file");
    }
    return 0;
}

int restore_extent(void *context, uint64_t offset, uint64_t length) {
    restorer *r = context;
    (void)length;
    if (cadapt_fseek(r->file, (cadapt_off)offset, SEEK_SET) != 0) {
        r->error = errno;
        return r->error != 0 ? r->error : EIO;
    }
    return 0;
}

/* One attribute of the entry just restored: set now, or, for a
 * directory, with its metadata once its children exist. */
int restore_attribute(void *context, const char *path, uint8_t kind, const char *name, const uint8_t *value,
                             size_t len) {
    restorer *r = context;
    int rc;
    if (kind == ADAPT_ARCHIVE_DIRECTORY) {
        if (r->pending_path == NULL || strcmp(r->pending_path, path) != 0) {
            return restore_fail(r, path, EINVAL, "directory attributes without an entry");
        }
        rc = journal_attribute(r, name, value, len);
        return rc != 0 ? restore_fail(r, path, rc, "cannot defer directory metadata") : 0;
    }
#ifndef _WIN32
    if (kind == ADAPT_ARCHIVE_FILE || kind == ADAPT_ARCHIVE_SPARSE) {
        if (strcmp(name, "system.posix_acl_access") == 0) {
            uint8_t *acl = malloc(len + 1);
            if (acl == NULL) return restore_fail(r, path, ENOMEM, NULL);
            memcpy(acl, value, len);
            free(r->file_acl);
            r->file_acl = acl;
            r->file_acl_len = len;
            return 0;
        }
        rc = attr_apply(r->entry_fd, name, value, len);
    } else
#endif
    {
        rc = restore_apply_attribute(r, path, kind == ADAPT_ARCHIVE_SYMLINK, name, value, len);
    }
    return rc != 0 ? restore_fail(r, path, rc, "cannot restore extended attribute") : 0;
}

/* Root attributes precede the children on the wire but take effect last. */
int restore_root_attribute(void *context, const char *name, const uint8_t *value, size_t len) {
    restorer *r = context;
    int rc = 0;
    if (r->pending_path == NULL) rc = journal_begin(r, "");
    if (rc == 0 && r->pending_path[0] != '\0') rc = EINVAL;
    if (rc == 0) rc = journal_attribute(r, name, value, len);
    return rc != 0 ? restore_fail(r, "root", rc, "cannot defer root metadata") : 0;
}

int restore_metadata(void *context, const char *path, uint8_t kind, const adapt_metadata *metadata) {
    restorer *r = context;
    int rc;
    if (kind == ADAPT_ARCHIVE_DIRECTORY) {
        if (r->pending_path == NULL || strcmp(r->pending_path, path) != 0) {
            return restore_fail(r, path, EINVAL, "directory metadata without an entry");
        }
        r->pending.metadata = *metadata;
        r->pending.has_metadata = 1;
        return 0;
    }
#ifndef _WIN32
    if (kind == ADAPT_ARCHIVE_FILE || kind == ADAPT_ARCHIVE_SPARSE) {
        rc = r->file_acl == NULL ? 0 : attr_apply(r->entry_fd, "system.posix_acl_access", r->file_acl, r->file_acl_len);
        free(r->file_acl);
        r->file_acl = NULL;
        if (rc == 0) rc = metadata_apply_fd(r->entry_fd, metadata);
        close(r->entry_fd);
        r->entry_fd = -1;
    } else
#endif
    {
        rc = restore_apply_metadata(r, path, kind == ADAPT_ARCHIVE_SYMLINK, metadata, NULL);
    }
    return rc != 0 ? restore_fail(r, path, rc, "cannot restore metadata") : 0;
}

/* Only a checksummed archive may replay the journal. The workspace is
 * bounded by the wire limits, independent of entry or attribute count. */
int finish_directory_metadata(restorer *r, int apply) {
    int rc = 0;
    cadapt_off end = 0;
    char path[ADAPT_ARCHIVE_PATH_MAX + 1u];
    char name[ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX + 1u];
    uint8_t value[ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX];
    if (apply && r->journal != NULL) {
        rc = journal_flush_record(r);
        if (rc == 0 && fflush(r->journal) != 0) rc = errno != 0 ? errno : EIO;
        if (rc == 0) {
            end = cadapt_ftell(r->journal);
            if (end < 0) rc = EIO;
        }
    }
    while (apply && rc == 0 && end > 0) {
        metadata_record record;
        cadapt_off path_at;
        uint32_t i;
#ifndef _WIN32
        int fd = r->root_fd;
        cadapt_off acl_at = -1;
        uint32_t acl_len = 0;
#endif
        if (end < (cadapt_off)sizeof(record) ||
            cadapt_fseek(r->journal, end - (cadapt_off)sizeof(record), SEEK_SET) != 0 ||
            journal_read(r->journal, &record, sizeof(record)) != 0) { rc = EIO; break; }
        path_at = end - (cadapt_off)sizeof(record) - record.path_bytes;
        if (record.path_bytes == 0 || record.path_bytes > sizeof(path) || record.start < 0 ||
            record.start > path_at || cadapt_fseek(r->journal, path_at, SEEK_SET) != 0 ||
            journal_read(r->journal, path, record.path_bytes) != 0 || path[record.path_bytes - 1] != '\0' ||
            strlen(path) + 1 != record.path_bytes || cadapt_fseek(r->journal, record.start, SEEK_SET) != 0) {
            rc = EIO; break;
        }
#ifndef _WIN32
        /* Retain the directory before an access ACL can remove our read
         * permission. Its timestamps/mode must use this same handle. */
        if (path[0] != '\0') {
            char *storage, *leaf;
            int parent = restore_parent(r, path, &storage, &leaf);
            fd = parent < 0 ? -1 : openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) rc = errno;
            if (parent >= 0) close(parent);
            free(storage);
        }
#endif
        for (i = 0; i < record.attributes && rc == 0; i++) {
            uint32_t lengths[2];
            cadapt_off at = cadapt_ftell(r->journal);
            if (at < 0 || path_at - at < (cadapt_off)sizeof(lengths) ||
                journal_read(r->journal, lengths, sizeof(lengths)) != 0 ||
                lengths[0] == 0 || lengths[0] > sizeof(name) || lengths[1] > sizeof(value) ||
                (uint64_t)(path_at - at - (cadapt_off)sizeof(lengths)) < (uint64_t)lengths[0] + lengths[1] ||
                journal_read(r->journal, name, lengths[0]) != 0 || name[lengths[0] - 1] != '\0' ||
                strlen(name) + 1 != lengths[0] || journal_read(r->journal, value, lengths[1]) != 0) {
                rc = EIO; break;
            }
#ifdef _WIN32
            rc = attr_apply(-1, name, value, lengths[1]);
#else
            if (strcmp(name, "system.posix_acl_access") == 0) {
                /* Replay this one value last without retaining a second
                 * attribute buffer. Its mode may remove owner write access. */
                acl_at = at + (cadapt_off)sizeof(lengths) + lengths[0];
                acl_len = lengths[1];
            } else {
                rc = attr_apply(fd, name, value, lengths[1]);
            }
#endif
        }
        if (rc == 0 && cadapt_ftell(r->journal) != path_at) rc = EIO;
#ifndef _WIN32
        if (rc == 0 && acl_at >= 0) {
            if (cadapt_fseek(r->journal, acl_at, SEEK_SET) != 0 ||
                journal_read(r->journal, value, acl_len) != 0) rc = EIO;
            else rc = attr_apply(fd, "system.posix_acl_access", value, acl_len);
        }
#endif
        if (rc == 0 && path[0] != '\0') {
#ifdef _WIN32
            rc = restore_apply_metadata(r, path, 0, record.has_metadata ? &record.metadata : NULL, NULL);
#else
            if (record.has_metadata) rc = metadata_apply_fd(fd, &record.metadata);
            else if (fchmod(fd, (mode_t)r->directory_mode) != 0) rc = errno;
#endif
        }
#ifndef _WIN32
        if (path[0] != '\0' && fd >= 0) close(fd);
#endif
        if (rc != 0) restore_fail(r, path[0] ? path : "root", rc,
                                  path[0] ? "cannot restore directory metadata or attributes" : "cannot restore root extended attribute");
        end = record.start;
    }
    if (r->journal != NULL && fclose(r->journal) != 0 && rc == 0 && apply) rc = errno != 0 ? errno : EIO;
    r->journal = NULL;
    free(r->pending_path);
    r->pending_path = NULL;
    if (rc != 0 && r->error == 0) restore_fail(r, "metadata journal", rc, "cannot read deferred metadata");
    return rc;
}

#ifdef _WIN32
/* Remove a directory we created and everything under it. Only the staging
 * directory is ever handed here. */
static void remove_tree(const char *path) {
    char **names = NULL;
    size_t count = 0, i;
    (void)_chmod(path, _S_IREAD | _S_IWRITE);
    if (list_directory(path, &names, &count) == 0) {
        for (i = 0; i < count; i++) {
            char *full = join(path, names[i]);
            if (full != NULL) {
                if (is_directory(full)) {
                    remove_tree(full);
                } else {
                    remove(full);
                }
                free(full);
            }
            free(names[i]);
        }
        free(names);
    }
    _rmdir(path);
}

/* A staging directory beside the target, published by renaming. */
int staged_dir_open(staged *s, const char *target, int force) {
    size_t dir_len, i;
    const char *slash;
    int attempt;
    s->path = NULL;
    s->file = NULL;
    if (!force && file_exists(target)) {
        fprintf(stderr, "error: %s exists; pass --force to overwrite\n", target);
        return EXIT_OTHER;
    }
    slash = strrchr(target, '/');
#ifdef _WIN32
    {
        const char *back = strrchr(target, '\\');
        if (back != NULL && (slash == NULL || back > slash)) {
            slash = back;
        }
    }
#endif
    dir_len = slash == NULL ? 0 : (size_t)(slash - target) + 1;
    for (attempt = 0; attempt < 128; attempt++) {
        char name[64];
        int n = snprintf(name, sizeof(name), ".cadapt-%ld-%d.part", (long)cadapt_getpid(), attempt);
        char *path = malloc(dir_len + (size_t)n + 1);
        int rc;
        if (path == NULL || n < 0) {
            free(path);
            return other_error(target, "memory allocation failed");
        }
        for (i = 0; i < dir_len; i++) {
            path[i] = target[i];
        }
        memcpy(path + dir_len, name, (size_t)n + 1);
#ifdef _WIN32
        rc = _mkdir(path);
#else
        /* Keep other users out until all entries and metadata are final. */
        rc = mkdir(path, 0700);
#endif
        if (rc != 0) {
            int e = errno;
            free(path);
            if (e == EEXIST) {
                continue;
            }
            return other_error(target, strerror(e));
        }
        s->path = path;
        return 0;
    }
    return other_error(target, "cannot create a unique staging directory");
}

void staged_dir_discard(staged *s) {
    if (s->path != NULL) {
        remove_tree(s->path);
        free(s->path);
        s->path = NULL;
    }
}
#else
/* Descendants are protected by the private wrapper, including after their
 * recorded modes have been applied. Never recurse through the wrapper's
 * mutable pathname or through a symbolic link from the archive. */
static void remove_tree_fd(int fd) {
    DIR *directory;
    struct dirent *entry;
    int scan;
    (void)fchmod(fd, 0700);
    scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan < 0) return;
    directory = fdopendir(scan);
    if (directory == NULL) {
        close(scan);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct stat st;
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int child;
            /* No other user can replace descendants inside the wrapper.
             * Restore owner access when recorded directory mode was 000. */
            if (fchmodat(fd, name, 0700, 0) != 0) continue;
            child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0) {
                remove_tree_fd(child);
                close(child);
                (void)unlinkat(fd, name, AT_REMOVEDIR);
            }
        } else {
            (void)unlinkat(fd, name, 0);
        }
    }
    closedir(directory);
}

void staged_dir_release(staged *s) {
    if (s->root_fd >= 0) close(s->root_fd);
    if (s->guard_fd >= 0) {
        struct stat held, named;
        const char *name = strrchr(s->path, '/');
        name = name == NULL ? s->path : name + 1;
        /* A moved wrapper can be left empty. Do not chase it or touch a
         * replacement. The final unlink itself never follows a symlink. */
        if (fstat(s->guard_fd, &held) == 0 &&
            fstatat(s->parent_fd, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
            held.st_dev == named.st_dev && held.st_ino == named.st_ino) {
            (void)unlinkat(s->parent_fd, name, AT_REMOVEDIR);
        }
        close(s->guard_fd);
    }
    if (s->parent_fd >= 0) close(s->parent_fd);
    s->root_fd = s->guard_fd = s->parent_fd = -1;
    free(s->target_name);
    s->target_name = NULL;
    free(s->path);
    s->path = NULL;
}

void staged_dir_discard(staged *s) {
    if (s->root_fd >= 0) {
        if (s->directory) remove_tree_fd(s->root_fd);
        (void)unlinkat(s->guard_fd, "contents", s->directory ? AT_REMOVEDIR : 0);
    }
    staged_dir_release(s);
}

int staged_guard_open(staged *s, const char *target, int force, int directory) {
    const char *slash = strrchr(target, '/');
    size_t dir_len = slash == NULL ? 0 : (size_t)(slash - target) + 1;
    char *parent_path = malloc(dir_len + 2);
    int attempt, rc = 0;
    struct stat st;
    s->directory = directory;
    s->path = NULL;
    s->file = NULL;
    s->parent_fd = s->guard_fd = s->root_fd = -1;
    s->target_name = strdup(target + dir_len);
    if (parent_path == NULL || s->target_name == NULL) {
        rc = ENOMEM;
    } else {
        if (dir_len != 0) memcpy(parent_path, target, dir_len);
        else parent_path[0] = '.';
        parent_path[dir_len != 0 ? dir_len : 1] = '\0';
        s->parent_fd = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (s->parent_fd < 0) rc = errno;
    }
    free(parent_path);
    if (rc == 0 && !force) {
        if (fstatat(s->parent_fd, s->target_name, &st, AT_SYMLINK_NOFOLLOW) == 0) rc = EEXIST;
        else if (errno != ENOENT) rc = errno;
    }
    for (attempt = 0; rc == 0 && attempt < 128; attempt++) {
        char name[64];
        mode_t mask;
        int made;
        snprintf(name, sizeof(name), ".cadapt-%ld-%d.part", (long)cadapt_getpid(), attempt);
        s->path = malloc(dir_len + strlen(name) + 1);
        if (s->path == NULL) {
            rc = ENOMEM;
            break;
        }
        memcpy(s->path, target, dir_len);
        strcpy(s->path + dir_len, name);
        /* This command has not started workers yet. Owner access must not
         * depend on umask; the wrapper stays 0700 until it is removed. */
        mask = umask(0);
        made = mkdirat(s->parent_fd, name, 0700);
        rc = made == 0 ? 0 : errno;
        umask(mask);
        if (rc == EEXIST) {
            free(s->path);
            s->path = NULL;
            rc = 0;
            continue;
        }
        if (rc != 0) break;
        s->guard_fd = openat(s->parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (s->guard_fd < 0) rc = errno;
        else if (fstat(s->guard_fd, &st) != 0) rc = errno;
        else if (st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) rc = EACCES;
        if (rc != 0) {
            if (s->guard_fd >= 0) close(s->guard_fd);
            s->guard_fd = -1;
            break;
        }
        if (directory) {
            mask = umask(0);
            made = mkdirat(s->guard_fd, "contents", 0700);
            rc = made == 0 ? 0 : errno;
            umask(mask);
            if (rc != 0) break;
            s->root_fd = openat(s->guard_fd, "contents", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } else {
            s->root_fd = openat(s->guard_fd, "contents", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        }
        if (s->root_fd < 0) {
            rc = errno;
            (void)unlinkat(s->guard_fd, "contents", directory ? AT_REMOVEDIR : 0);
        } else {
            rc = clear_inherited_acl(s->root_fd);
            if (rc == 0 && fchmod(s->root_fd, directory ? 0700 : 0600) != 0) rc = errno;
            if (rc == 0 && !directory) {
                int fd = dup(s->root_fd);
                s->file = fd < 0 ? NULL : fdopen(fd, "wb");
                if (s->file == NULL) {
                    rc = errno;
                    if (fd >= 0) close(fd);
                }
            }
        }
        if (rc == 0) return 0;
    }
    if (rc == 0) rc = EEXIST;
    staged_dir_discard(s);
    return other_error(target, rc == EEXIST ? "exists; pass --force to overwrite"
                                           : strerror(rc));
}
int staged_dir_open(staged *s, const char *target, int force) {
    return staged_guard_open(s, target, force, 1);
}

#endif

/* POSIX rename() may replace a directory created after the initial check.
 * Refuse safely when the platform cannot make the no-replace operation atomic. */
static int staged_dir_rename(staged *s, const char *target, int force, int *exchanged) {
    *exchanged = 0;
#ifdef _WIN32
    (void)force;
    return rename(s->path, target); /* Windows rename refuses existing names. */
#else
    (void)target;
    if (force) {
        struct stat previous;
        int rc;
        /* Rename already replaces an empty directory atomically. A file
         * needs an exchange because rename cannot replace unlike types. */
        rc = renameat(s->guard_fd, "contents", s->parent_fd, s->target_name);
        if (rc == 0 || (errno != ENOTDIR && errno != EISDIR)) return rc;
        if (fstatat(s->parent_fd, s->target_name, &previous, AT_SYMLINK_NOFOLLOW) != 0) return -1;
        if (S_ISDIR(previous.st_mode)) {
            errno = EEXIST; /* the destination changed; keep the new directory */
            return -1;
        }
#if defined(__linux__) && defined(RENAME_EXCHANGE)
        rc = renameat2(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_EXCHANGE);
#elif defined(__APPLE__) && defined(RENAME_SWAP)
        rc = renameatx_np(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_SWAP);
#else
        errno = ENOTSUP;
        rc = -1;
#endif
        if (rc == 0) *exchanged = 1;
        return rc;
    }
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    return renameat2(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_NOREPLACE);
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    return renameatx_np(s->guard_fd, "contents", s->parent_fd, s->target_name, RENAME_EXCL);
#else
    errno = ENOTSUP;
    return -1;
#endif
#endif
}

/* Publish the staged directory as `target`: with --force an existing file
 * or empty directory is replaced; a directory with contents is not, so no
 * bytes of the user's are ever removed here. */
int staged_dir_publish(staged *s, const char *target, int force) {
    int exchanged;
#ifndef _WIN32
    struct stat root;
    mode_t root_mode;
    int restore_mode;
#endif
    if (cancelled()) {
        staged_dir_discard(s);
        return other_error(target, "interrupted");
    }
#ifndef _WIN32
    if (fstat(s->root_fd, &root) != 0) {
        int e = errno;
        staged_dir_discard(s);
        return other_error(target, strerror(e));
    }
    root_mode = root.st_mode & 07777;
    restore_mode = !(root_mode & S_IWUSR);
    /* Moving a directory between parents can require write permission on
     * the directory itself. Grant it only to its owner, who already can
     * chmod it, then restore the validated mode through the held handle. */
    if (restore_mode && fchmod(s->root_fd, root_mode | S_IWUSR) != 0) {
        int e = errno;
        staged_dir_discard(s);
        return other_error(target, strerror(e));
    }
#endif
    if (staged_dir_rename(s, target, force, &exchanged) != 0) {
        int e = errno;
        staged_dir_discard(s);
        if (!force && (e == ENOSYS || e == EINVAL || e == ENOTSUP)) {
            return other_error(target, "atomic directory publication is unavailable; pass --force to allow replacement");
        }
        if (force && (e == ENOSYS || e == EINVAL || e == ENOTSUP)) {
            return other_error(target, "atomic folder replacement is unavailable; existing target preserved");
        }
        if (force && e == ENOTEMPTY) return other_error(target, "exists and is not empty; remove it first");
        if (force && e == EEXIST) return other_error(target, "existing directory changed; target preserved");
        return other_error(target, e == EEXIST || e == ENOTEMPTY ? "exists; pass --force to overwrite" : strerror(e));
    }
#ifdef _WIN32
    free(s->path);
    s->path = NULL;
#else
    if (restore_mode) {
        int e = 0;
        if (fchmod(s->root_fd, root_mode) != 0 || fstat(s->root_fd, &root) != 0) e = errno;
        else if ((root.st_mode & 07777) != root_mode) e = ENOTSUP;
        if (e != 0) {
            char message[512];
            /* Publication already succeeded. Recursive rollback here
             * could remove files another process added to the output. */
            snprintf(message, sizeof(message), "folder published but cannot restore root mode: %s%s%s", strerror(e),
                     exchanged ? "; previous target retained in staging wrapper " : "", exchanged ? s->path : "");
            staged_dir_release(s);
            return other_error(target, message);
        }
    }
    if (exchanged && unlinkat(s->guard_fd, "contents", 0) != 0) {
        char message[512];
        /* Never recurse here: a concurrent writer may have changed the
         * destination to a directory before the exchange. Preserve it. */
        snprintf(message, sizeof(message), "folder published; previous target retained in staging wrapper %s: %s",
                 s->path, strerror(errno));
        staged_dir_release(s);
        return other_error(target, message);
    }
    staged_dir_release(s);
#endif
    return 0;
}
