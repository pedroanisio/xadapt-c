/*
 * cadapt_folder.h — the folder side of the command line: a directory walked
 * into an archive stream on the way in (folder, packer), an archive stream
 * restored into a staged directory on the way out (restorer), and the
 * staged output both halves of the command line publish.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 * cadapt.c owns command dispatch, streams and file staging. This interface
 * exposes folder packing/restoration and uses the command layer's cancellation
 * check and error reporting services declared below.
 */
#ifndef CADAPT_FOLDER_H
#define CADAPT_FOLDER_H

/* cadapt_platform.h first, always: see cadapt_fs.h. */
#include "cadapt_platform.h"

#include "adapt.h"
#include "cadapt_fs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* CLI exit codes shared by command and filesystem operations. */
enum { EXIT_OTHER = 1, EXIT_USAGE = 2 };

/* The staged output: a file beside the target, published by name only
 * when the work is complete, removed otherwise. */
typedef struct staged {
    char *path;
    FILE *file;
    unsigned mode; /* final mode for a file without recorded metadata */
#ifndef _WIN32
    /* Folders live inside a private wrapper whose mode never changes.
     * Publication moves "contents" out through guard_fd, not s->path. */
    int parent_fd, guard_fd, root_fd;
    int directory; /* contents is a directory or a regular output file */
    char *target_name;
#endif
} staged;

/* What a directory entry is: the kinds an archive holds, or none. */
enum {
    KIND_NONE = 0,
    KIND_DIRECTORY = ADAPT_ARCHIVE_DIRECTORY,
    KIND_FILE = ADAPT_ARCHIVE_FILE,
    KIND_SYMLINK = ADAPT_ARCHIVE_SYMLINK,
    KIND_HARDLINK = ADAPT_ARCHIVE_HARDLINK,
    KIND_SPARSE = ADAPT_ARCHIVE_SPARSE
};

/* A file's identity on its filesystem, for the hard links among a
 * folder's files: two names with the same identity share one inode. */
typedef struct identity {
    uint64_t device, inode;
    int shared; /* the inode has more than one name */
} identity;

/* One entry of the folder being coded. */
typedef struct entry {
    char *path; /* the archive path */
    char *target; /* a link's target, or the path a hard link shares */
    uint64_t size;
    uint8_t kind;
    adapt_metadata metadata;
    /* a sparse file's data extents */
    adapt_extent *extents;
    uint32_t extent_count;
    attrs attrs;
} entry;

/* An inode seen with more than one name, and the entry that carries it —
 * a slot of the walker's inode table, the entry's position plus one so
 * that 0 marks an empty slot. */
typedef struct seen_inode {
    identity id;
    size_t index;
} seen_inode;

typedef struct folder {
    const char *root;
    attrs root_attrs;
    entry *items;
    size_t count, cap;
    seen_inode *inodes;
    size_t inode_count, inode_cap;
    /* the bytes the archive stream takes, for the sizes and the report */
    uint64_t stream_bytes;
} folder;

/* The reader that serialises a folder: the magic, then each entry's
 * header and its file's bytes, then the end. A file that no longer holds
 * the bytes it was listed with fails the read: the archive would lie. */
typedef struct packer {
    folder *f;
    size_t index; /* the next entry */
    /* the header, an extent's offset and length and the end go out
     * through `head`, the attribute trailer through `trailer`; a file's
     * or an extent's bytes are read straight from the file */
    enum { PACK_MAGIC, PACK_ROOT_ATTRS, PACK_HEADER, PACK_EXTENT, PACK_DATA, PACK_ATTRS, PACK_END, PACK_DONE } stage;
    uint8_t head[ADAPT_ARCHIVE_MAGIC_BYTES + 1 + 2 + ADAPT_ARCHIVE_PATH_MAX + 2 + ADAPT_ARCHIVE_PATH_MAX + 12 +
                 ADAPT_METADATA_BYTES];
    size_t head_len, head_at;
    uint8_t *trailer; /* the entry's attribute trailer, malloc'd */
    size_t trailer_len, trailer_at;
    FILE *file;
    uint64_t remaining;
    uint32_t extent; /* the next extent of a sparse entry */
    int error; /* errno of the failure */
    const char *failed; /* the entry's path, or NULL */
    int changed; /* the failure: the file's bytes differed from its listing */
} packer;

/* A private, temporary journal retains attributes on disk. Each record
 * ends in a fixed footer so replay needs only one bounded attribute and
 * visits children before parents, without an in-memory directory index. */
typedef struct metadata_record {
    cadapt_off start;
    uint32_t path_bytes, attributes;
    int has_metadata;
    adapt_metadata metadata;
} metadata_record;

typedef struct restorer {
    const char *root;
#ifndef _WIN32
    int root_fd, entry_fd; /* retain the current file through ACL and metadata callbacks */
    unsigned file_mode, directory_mode; /* final modes for version 1 entries */
    uint8_t *file_acl; /* access ACL waits until other file attributes are set */
    size_t file_acl_len;
#endif
    FILE *journal;
    metadata_record pending;
    char *pending_path;
    FILE *file;
    uint64_t restored_size, max_output;
    int error; /* errno of the failure */
    char *failed; /* the entry's path, malloc'd, or NULL */
    const char *why; /* a failure the errno does not name, or NULL */
} restorer;

/* The walk in: a folder listed, then read as one archive stream. */
int is_directory(const char *path);
int folder_collect(folder *f, const char *root, char **culprit);
void folder_free(folder *f);
void packer_reset(packer *p, folder *f);
int read_packer(void *context, uint8_t *buf, size_t len, size_t *got);
void packer_close(packer *p);
int packer_failure(const char *input, const packer *p);

/* The restore out: the archive decoder's callbacks (adapt.h, "Archives")
 * and what the decode command calls around them. */
int restore_directory(void *context, const char *path);
int restore_file_open(void *context, const char *path, uint64_t size);
int restore_file_data(void *context, const uint8_t *data, size_t len);
int restore_file_close(void *context);
int restore_symlink(void *context, const char *path, const char *target);
int restore_hardlink(void *context, const char *path, const char *target);
int restore_sparse_open(void *context, const char *path, uint64_t size, uint32_t extents);
int restore_extent(void *context, uint64_t offset, uint64_t length);
int restore_attribute(void *context, const char *path, uint8_t kind, const char *name, const uint8_t *value,
                             size_t len);
int restore_metadata(void *context, const char *path, uint8_t kind, const adapt_metadata *metadata);
int restore_root_attribute(void *context, const char *name, const uint8_t *value, size_t len);
int restore_fail(restorer *r, const char *path, int e, const char *why);
int finish_legacy_file(restorer *r, int apply);
int finish_directory_metadata(restorer *r, int apply);

/* The staged directory: opened beside the target, published or discarded. */
int staged_dir_open(staged *s, const char *target, int force);
void staged_dir_discard(staged *s);
int staged_dir_publish(staged *s, const char *target, int force);
#ifndef _WIN32
int staged_guard_open(staged *s, const char *target, int force, int directory);
void staged_dir_release(staged *s);
#endif

/* From cadapt.c: whether the user interrupted the command, and the report
 * of an error against a path, which is the exit status. */
int cancelled(void);
int other_error(const char *path, const char *message);
#ifdef _WIN32
int file_exists(const char *path);
#endif

#endif /* CADAPT_FOLDER_H */
