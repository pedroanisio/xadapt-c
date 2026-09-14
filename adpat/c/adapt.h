/*
 * adapt.h — the adapt codec, format versions 13 through 18, in C99 with
 * nothing beyond the C standard library (and POSIX threads for the
 * encoder's proof, when built with ADAPT_THREADS).
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 * The format began as a port of the Rust crate in `../src`, integer for
 * integer: `container.rs` for the framing and `model.rs` for the
 * predictor. Versions 13 to 15 are defined by this implementation and
 * differ from the crate's 10 to 12 in four measured ways — an x86 branch
 * transform before the model, four blocks to a table line instead of two,
 * a context table twice the size for the same input, and a max profile
 * that adds contexts, run memories and per-context evidence in place of
 * the larger arbiter — so a file of theirs is refused here by version,
 * and the crate must be brought up to these before the two agree again.
 * The layout and names still follow the crate's so the two can be read
 * side by side; a change to any constant below is a new format version.
 *
 * Decoding: adapt_decode() holds a file in memory and returns its bytes
 * only after their checksum matched; adapt_decode_stream() pulls it through
 * a reader callback and pushes each block to a writer as it is recovered,
 * so that memory is the model and one block whatever the length — bytes it
 * has already written are not to be trusted until it returns ADAPT_OK.
 *
 * Encoding: adapt_encode() codes bytes held in memory into one file;
 * adapt_encode_stream() reads a stream once, a block at a time, and writes
 * as it goes. adapt_encode_verified() and adapt_encode_stream_verified()
 * prove the result decodes back to the input before returning it, with a
 * second model that decodes every block as it is written; adapt_verify_stream()
 * is the same proof after the fact, with one model, for a source that can
 * be read twice. Sizes come from adapt_sizes_for_input(), which fits the
 * model to the input's length and a memory budget as the crate does.
 *
 * Every failure is a typed status, and the details a message needs travel
 * in an adapt_error.
 */
#ifndef ADAPT_H
#define ADAPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The container version this reader implements: the normal profile.
 * Versions 10 to 12, the crate's, are refused like every version no file
 * was published in. */
#define ADAPT_FORMAT_VERSION 13u
/* The header's window byte carries this bit when a record period follows
 * the fixed header as ADAPT_PERIOD_BYTES more bytes; the period is the
 * distance the record and field contexts read across (a raw image's
 * scanline, a fixed-size record), at most ADAPT_PERIOD_MAX. */
#define ADAPT_PERIOD_FLAG 0x80u
#define ADAPT_PERIOD_BYTES 2u
#define ADAPT_PERIOD_MAX 4096u
/* The header's window byte carries this bit when the file is segmented:
 * cut into segments that each start the model afresh, a marker byte
 * (method 4) before every segment after the first and a table (method 5:
 * per segment its original and coded length as two u32, then the segment
 * count as a u32) before the trailer. A decoder reading a stream resets
 * its model at each marker and checks the table at the end; one holding a
 * file may read the table first and decode the segments at once. The
 * crate does so on threads; this port codes and decodes segments in order,
 * on one thread, to the same bytes. */
#define ADAPT_SEGMENT_FLAG 0x40u
/* The header's window byte carries this bit when every block's bytes went
 * through the x86 branch transform before modelling: the four-byte
 * relative target after each E8 (call) and E9 (jmp) opcode is rewritten
 * as a block-relative absolute address, so repeated calls to one function
 * become identical byte sequences. The encoder turns it on for an input
 * whose first block carries an ELF, MZ or Mach-O signature; the decoder
 * undoes it on every recovered block before the checksum. */
#define ADAPT_X86_FLAG 0x20u
/* Input bytes per segment when a caller asks for segments and no other
 * length: 16 MiB. */
#define ADAPT_SEGMENT_BYTES ((uint64_t)1 << 24)
/* Bytes of one segment's table entry, and of the count that ends the table. */
#define ADAPT_SEGMENT_ENTRY_BYTES 8u
#define ADAPT_SEGMENT_COUNT_BYTES 4u
/* The version a fast-profile file carries: the same container, the fast
 * model (two hashed contexts chosen per file, a direct order-1 memory,
 * the word run, recall, one arbiter), its context-set code an index into
 * the profile's own menu of ADAPT_FAST_SETS pairs. Version 7, the fast
 * profile of 0.7.0 with one fixed pair, and version 8, the fast profile
 * before segments, are refused like every version no file was published
 * in. */
#define ADAPT_FAST_VERSION 14u
/* The version a max-profile file carries: the normal model with four
 * more hashed contexts (the previous word with the current, the first and
 * third bytes back, the bytes that followed the order-2 context, the high
 * nibbles of the last four bytes), run memories on orders 1, 3, 4 and 6,
 * and three arbiter inputs per context instead of one; the same memory,
 * about twice the time, fewer bytes. `cadapt encode --max`. */
#define ADAPT_MAX_VERSION 15u
/* Versions 16--18 use the same predictors as 13--15 and append a
 * checksummed filesystem-metadata record to the header. Old streams keep
 * their original versions and remain readable. */
#define ADAPT_METADATA_VERSION 16u
#define ADAPT_METADATA_FAST_VERSION 17u
#define ADAPT_METADATA_MAX_VERSION 18u
#define ADAPT_METADATA_BYTES 28u
#define ADAPT_METADATA_HEADER_BYTES (ADAPT_METADATA_BYTES + 4u)
#define ADAPT_HEADER_MAX_BYTES (ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES + ADAPT_METADATA_HEADER_BYTES)

/* POSIX permission bits and signed Unix seconds plus nanoseconds. ctime
 * and creation time are filesystem-managed and are not reconstructed. */
typedef struct adapt_metadata {
    uint32_t mode;
    int64_t atime_seconds;
    uint32_t atime_nanoseconds;
    int64_t mtime_seconds;
    uint32_t mtime_nanoseconds;
} adapt_metadata;

/* Serialize/parse exactly ADAPT_METADATA_BYTES, returning 1 on success,
 * 0 for NULL arguments or invalid mode/nanosecond fields. */
int adapt_metadata_write(const adapt_metadata *metadata, uint8_t *out);
int adapt_metadata_read(const uint8_t *data, adapt_metadata *metadata);

/* The fixed eight-byte header identifies whether metadata follows. */
int adapt_header_has_metadata(const uint8_t *header);
/* Extend a valid legacy header into out, returning its full length or 0.
 * The input and output must not overlap. */
size_t adapt_header_add_metadata(const uint8_t *header, size_t len, const adapt_metadata *metadata,
                                 uint8_t *out, size_t cap);
/* The fast profile's context sets: order-2 and class, order-2 and
 * order-4, order-1 and order-2. */
#define ADAPT_FAST_SETS 3u
/* The fast profile's table, at most, as a log2 of blocks: 2 MiB. */
#define ADAPT_FAST_TABLE_LOG_MAX 17u
/* The header's table byte carries this bit when the container holds a
 * folder: its decoded bytes are an adapt archive (see below), one stream
 * that names every file and directory and holds every file's bytes, so a
 * folder is coded as one file and restored from it alone. */
#define ADAPT_ARCHIVE_FLAG 0x80u
/* Fixed header: signature, version, table-size byte (class code in bits 5
 * and 6, the archive flag in bit 7), recall-table size (context-set code
 * in its top three bits), window size. */
#define ADAPT_HEADER_BYTES 8u
/* Per-block header: method, raw length, payload length. */
#define ADAPT_BLOCK_HEADER_BYTES 9u
/* The trailer: the end method, the original length, its CRC-32. */
#define ADAPT_TRAILER_BYTES 13u
/* Input is coded in blocks of at most this many bytes. */
#define ADAPT_BLOCK_BYTES ((size_t)1 << 18)
/* Output the decoder produces without an explicit limit: 1 GiB. */
#define ADAPT_DEFAULT_MAX_OUTPUT ((uint64_t)1 << 30)
/* Default working-memory budget: 674 MiB, what xz -9 takes to compress,
 * and the decoder's default too, since a decoder holds the model the
 * encoder held. Memory buys the context table, and the table buys ratio at
 * no cost in time: on 100 MB of enwik8 a 64 MiB budget coded 21,221,772
 * bytes, 256 MiB 20,321,065, 674 MiB 19,943,347 (−6.0 %), all at the same
 * speed; the window mattered little (four times the window alone: −0.25 %).
 * Inputs under 8 MiB fit their model in 64 MiB and are unaffected. */
#define ADAPT_DEFAULT_MAX_MEMORY ((uint64_t)674 << 20)

/* Why a file could not be decoded. The names and order follow the crate's
 * `Error`; adapt_error_message() writes the same text it displays. */
typedef enum adapt_status {
    ADAPT_OK = 0,
    /* The operation would exceed the caller's working-memory budget. */
    ADAPT_ERROR_MEMORY_LIMIT,
    /* malloc refused a reservation; no output was published. */
    ADAPT_ERROR_ALLOCATION,
    /* The first four bytes are not the container's signature. */
    ADAPT_ERROR_BAD_MAGIC,
    /* The version byte names a format this reader does not implement. */
    ADAPT_ERROR_UNSUPPORTED_VERSION,
    /* A header field is outside the range the format allows. */
    ADAPT_ERROR_BAD_HEADER,
    /* The declared original length exceeds the caller's limit. */
    ADAPT_ERROR_TOO_LARGE,
    /* The file ends before the declared content does. */
    ADAPT_ERROR_TRUNCATED,
    /* A block header or payload is inconsistent with the format. */
    ADAPT_ERROR_BAD_BLOCK,
    /* A block names a method this reader does not implement. */
    ADAPT_ERROR_BAD_METHOD,
    /* Bytes follow the last block. */
    ADAPT_ERROR_TRAILING_BYTES,
    /* The recovered bytes do not match the stored CRC-32. */
    ADAPT_ERROR_CHECKSUM,
    /* The encoder's output did not decode back to its input: `cause` names
     * the decoding error when there was one, ADAPT_OK when the decode
     * succeeded and produced different bytes. */
    ADAPT_ERROR_VERIFICATION,
    /* The reader or writer callback failed. */
    ADAPT_ERROR_IO,
    /* A required pointer was NULL. */
    ADAPT_ERROR_INVALID_ARGUMENT,
    /* The decoded bytes of a folder are not a well-formed adapt archive:
     * `detail` is the offset of the byte that was refused. */
    ADAPT_ERROR_BAD_ARCHIVE
} adapt_status;

/* A status with the numbers its message needs: `required` and `limit` for
 * ADAPT_ERROR_MEMORY_LIMIT, `declared` and `limit` for ADAPT_ERROR_TOO_LARGE,
 * the version, the method, the count of trailing bytes, or the callback's
 * return value for ADAPT_ERROR_IO. For ADAPT_ERROR_VERIFICATION, `cause` is
 * the decoding error and `detail` and `limit` are its numbers. Fields a
 * status does not use are zero. */
typedef struct adapt_error {
    adapt_status status;
    adapt_status cause;
    uint64_t detail;
    uint64_t limit;
} adapt_error;

/* Read validated metadata from an extended header; legacy headers return
 * ADAPT_ERROR_BAD_HEADER. The header CRC covers the metadata and framing. */
adapt_status adapt_read_metadata(const uint8_t *header, size_t len, adapt_metadata *metadata, adapt_error *error);

/* Write the message the Rust crate displays for `error` into `buf`
 * (NUL-terminated, truncated to `len`); returns the length it wanted, as
 * snprintf does. */
int adapt_error_message(const adapt_error *error, char *buf, size_t len);

/* Caller-owned allocation policy: the largest decoded length, and the
 * working memory the model, its window, one block of workspace and 8 MiB
 * of headroom may take. The in-memory decoder also counts its result. */
typedef struct adapt_limits {
    uint64_t max_output;
    uint64_t max_memory;
} adapt_limits;

/* ADAPT_DEFAULT_MAX_OUTPUT and ADAPT_DEFAULT_MAX_MEMORY. */
adapt_limits adapt_default_limits(void);

/* The sizes a header records: 2^table_log context blocks, 2^match_log
 * recall entries, a 2^window_log-byte window, the class-history code
 * (0 none, 1 eight bytes, 2 twenty-one) and the context-set code (0 every
 * context, 1..7 a set that leaves some out; see ADAPT_CONTEXT_SETS); and
 * the profile, 1 for the fast profile (written as ADAPT_FAST_VERSION, its
 * set code indexing the fast menu of ADAPT_FAST_SETS), 0 for the normal
 * one (ADAPT_FORMAT_VERSION). adapt_sizes_for_input chooses the normal
 * profile; adapt_sizes_fast turns sizes to the fast one, capping the
 * table as the profile requires, as `adapt encode --fast` does. */
typedef struct adapt_sizes {
    uint8_t table_log;
    uint8_t match_log;
    uint8_t window_log;
    uint8_t class_code;
    uint8_t set_code;
    uint8_t fast;
    /* The record period in bytes, or 0; found by the encoder on the first
     * block's sample (adapt_detect_period), carried after the fixed header
     * when nonzero, always 0 under the fast profile. */
    uint16_t period;
    /* 1 when the file is cut into segments (ADAPT_SEGMENT_FLAG). Set by the
     * caller who asks for them; adapt_sizes_for_input leaves it 0. Every
     * profile may be segmented. */
    uint8_t segmented;
    /* 1 for the max profile (ADAPT_MAX_VERSION); never with `fast`.
     * adapt_sizes_max turns it on; adapt_sizes_for_input_max fits sizes
     * to a budget as adapt_sizes_for_input does, with `max` set. */
    uint8_t max;
    /* 1 when the blocks carry the x86 branch transform (ADAPT_X86_FLAG).
     * Chosen by the encoder from the first block; adapt_sizes_for_input
     * leaves it 0. Any profile may carry it. */
    uint8_t x86;
    /* 1 when the decoded bytes are an adapt archive, a folder
     * (ADAPT_ARCHIVE_FLAG). Set by the caller who codes one;
     * adapt_sizes_for_input leaves it 0. Any profile may carry it. */
    uint8_t archive;
} adapt_sizes;

/* Context sets a file may choose, by header code. */
#define ADAPT_CONTEXT_SETS 8u

/* The bytes the header of a file whose fixed header is `fixed` occupies:
 * fixed part, optional period, and metadata extension for versions 16--18.
 * `fixed` must contain at least ADAPT_HEADER_BYTES bytes. */
size_t adapt_header_len(const uint8_t *fixed);
/* Base byte-stream encoder header size, excluding filesystem metadata. */
size_t adapt_sizes_header_bytes(adapt_sizes sizes);
/* Validate the header at the start of `header`, `len` bytes of it available:
 * fixed part, optional period, and metadata including its checksum.
 * Fewer bytes than the header announces is ADAPT_ERROR_TRUNCATED. */
adapt_status adapt_read_header(const uint8_t *header, size_t len, adapt_sizes *sizes, adapt_error *error);
/* The record period of `sample` as the encoder finds it, or 0. */
uint16_t adapt_detect_period(const uint8_t *sample, size_t n);
/* Bytes the segment table of `count` segments occupies, its method byte
 * included. */
size_t adapt_segment_table_len(uint64_t count);
/* Whether `segment_bytes` is a length the encoder can cut: a whole number
 * of blocks, at least one, no longer than a table entry counts. */
int adapt_segment_bytes_valid(uint64_t segment_bytes);

/* Validate a trailer and return the original length and checksum it
 * declares. */
adapt_status adapt_read_trailer(const uint8_t *trailer, uint64_t *length, uint32_t *crc32, adapt_error *error);

/* Bytes of class history the model's class context reads under `sizes`. */
uint32_t adapt_class_width(adapt_sizes sizes);

/* The contexts the set in `sizes` leaves out, as the crate's `info` names
 * them, comma-separated ("field, sparse, record, runs"); an empty string
 * for the full set. `buf` should hold at least 64 bytes. */
void adapt_dropped_contexts(adapt_sizes sizes, char *buf, size_t len);
/* The names of the two contexts a fast file's set runs, comma-separated,
 * into buf. */
void adapt_fast_contexts(adapt_sizes sizes, char *buf, size_t len);
/* `sizes` for the fast profile: the table capped at
 * ADAPT_FAST_TABLE_LOG_MAX, the set code cleared for the encoder to
 * choose. */
adapt_sizes adapt_sizes_fast(adapt_sizes sizes);
/* `sizes` for the max profile: the normal profile's sizes and choices
 * with `max` set, as `adapt encode --max` does. */
adapt_sizes adapt_sizes_max(adapt_sizes sizes);

/* Rough size in bytes of one model at these sizes, as the crate counts it. */
uint64_t adapt_model_bytes(adapt_sizes sizes);

/* Working-memory bound for decoding at these sizes: one model with its
 * window, one block of workspace and 8 MiB of headroom. A budget below it
 * is refused before anything is allocated. */
uint64_t adapt_decode_memory_bound(adapt_sizes sizes);

/* Working-memory bound for encoding at these sizes: the same. */
uint64_t adapt_encode_memory_bound(adapt_sizes sizes);

/* Working-memory bound for encoding with the verifier alongside: a second
 * model and window, its scratch block, and the queued blocks. */
uint64_t adapt_verified_memory_bound(adapt_sizes sizes);

/* Whether every size is one the format allows. */
int adapt_sizes_valid(adapt_sizes sizes);

/* The largest context table, as a log2 of blocks, an input of `length`
 * bytes grows into when the budget holds it and the window has its cap:
 * the sweet spot the length chooses below 8 MiB, then a block per eight
 * input bytes to 2^24 from 64 MiB — four times the crate's
 * `table_log_for`. Measured on text and tables, the table the crate chose
 * was the model's bottleneck: every context added to it was evicted
 * rather than learned; twice the table coded an 8 MiB text 1.6 % smaller
 * and four times the table, on 100 MB of it, 1.9 % smaller again. The
 * budget still bounds it: the default budget holds the sizes it did. */
uint8_t adapt_table_log_for(uint64_t length);

/* The sizes for an input of `length` bytes (`length_known` 0 when the
 * length is not known in advance, as on a pipe) under a memory budget, as
 * the crate chooses them: tables grow with the input up to their sweet
 * spot and shrink, largest first, until the budget holds them; then the
 * window takes what the budget leaves, capped at the input's own length;
 * then, on an input of 32 MiB or more whose window has reached that cap,
 * the context table grows past the sweet spot while the budget holds it
 * (adapt_table_log_for), so the default output is what it was and a
 * larger budget buys a larger table for a large input.
 * A pipe's sizes are chosen for two models whenever the budget holds two
 * at all, since only a second model can prove a pipe. The class code is
 * left 0: the encoder chooses it from the first block. Fails only when the
 * budget cannot hold the smallest model the format allows. */
adapt_status adapt_sizes_for_input(int length_known, uint64_t length, uint64_t max_memory, adapt_sizes *sizes,
                                   adapt_error *error);
/* adapt_sizes_for_input for the max profile: the same rule and the same
 * memory; the result has `max` set. */
adapt_status adapt_sizes_for_input_max(int length_known, uint64_t length, uint64_t max_memory, adapt_sizes *sizes,
                                       adapt_error *error);

/* The crate's threads (its `segments` module), for the sizes a file gets:
 * the most bytes one segment of `segment_bytes` can take in the file; the
 * working memory a run of `threads` needs, a model, one block of workspace
 * and one segment's coded bytes per thread and the headroom once; and the
 * sizes for an input of `length` bytes coded that way, each thread's model
 * fitted to a segment's length and the thread's share of the budget, or
 * ADAPT_ERROR_MEMORY_LIMIT naming the bytes the threads need. `cadapt
 * encode --threads` chooses a file's sizes with this and codes the
 * segments in order, to the bytes the crate's threads write. */
uint64_t adapt_segment_out_bound(uint64_t segment_bytes);
uint64_t adapt_threads_encode_memory_bound(adapt_sizes sizes, uint64_t threads, uint64_t segment_bytes);
adapt_status adapt_sizes_for_threads(uint64_t length, uint64_t max_memory, uint64_t threads, uint64_t segment_bytes,
                                     adapt_sizes *sizes, adapt_error *error);
/* adapt_sizes_for_threads for the max profile. */
adapt_status adapt_sizes_for_threads_max(uint64_t length, uint64_t max_memory, uint64_t threads, uint64_t segment_bytes,
                                         adapt_sizes *sizes, adapt_error *error);

/* What a coding pass did: original bytes, container bytes, the checksum
 * of the original, and the segments the file has. */
typedef struct adapt_stats {
    uint64_t input_bytes;
    uint64_t output_bytes;
    uint32_t crc32;
    /* Segments the file has: 1 unless it is segmented. */
    uint32_t segments;
} adapt_stats;

/* Read up to `len` bytes into `buf` and store how many in `*got`: 0 only
 * at the end of the input (a short read elsewhere is read again). Return
 * 0, or a nonzero value that becomes the ADAPT_ERROR_IO detail. */
typedef int (*adapt_read_fn)(void *context, uint8_t *buf, size_t len, size_t *got);

/* Write all `len` bytes of `buf`. Return 0, or a nonzero value that becomes
 * the ADAPT_ERROR_IO detail. */
typedef int (*adapt_write_fn)(void *context, const uint8_t *buf, size_t len);

/* Decode a file pulled from `read` into `write` within `limits`: header,
 * blocks, trailer, nothing after it. The model is allocated at the first
 * block that needs one, after the budget check; bytes reach `write` block
 * by block, so a damaged file is discovered when its damage is read, and
 * the checksum is checked last. `stats` and `error` may be NULL. */
adapt_status adapt_decode_stream(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                 adapt_limits limits, adapt_stats *stats, adapt_error *error);

/* Decode a complete file held in memory: the framing is validated end to
 * end before the model or the result is allocated, the declared length is
 * refused above `limits.max_output` before anything else, and the bytes
 * are stored in `*out` (malloc'd; release with adapt_free) only after their
 * checksum matched. `error` may be NULL. */
adapt_status adapt_decode(const uint8_t *file, size_t file_len, adapt_limits limits, uint8_t **out, size_t *out_len,
                          adapt_error *error);

/* Release a result of adapt_decode(). */
void adapt_free(uint8_t *bytes);

/* Validate the header and trailer of a complete file and return the
 * declared original length, without reading blocks or verifying the
 * checksum. */
adapt_status adapt_declared_len(const uint8_t *file, size_t file_len, uint64_t *length, adapt_error *error);

/* Encode `read` into `write` in one pass under `sizes` (from
 * adapt_sizes_for_input, or any valid triple; the class code is chosen
 * here from the first block) and `max_memory`, without proving the result.
 * `stats` and `error` may be NULL. */
adapt_status adapt_encode_stream(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                 adapt_sizes sizes, uint64_t max_memory, adapt_stats *stats, adapt_error *error);

/* adapt_encode_stream() with the segment length spelled out: when
 * `sizes.segmented` is set, every `segment_bytes` of input (a whole number
 * of blocks) starts a fresh model after a marker byte, and the table of
 * segments precedes the trailer; the crate's threads write the same bytes.
 * adapt_encode_stream() cuts ADAPT_SEGMENT_BYTES. */
adapt_status adapt_encode_stream_segments(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                          void *write_context, adapt_sizes sizes, uint64_t max_memory,
                                          uint64_t segment_bytes, adapt_stats *stats, adapt_error *error);

/* Encode `read` into `write` while a second model decodes every block as
 * it is written and compares the bytes with the source, so the file is
 * proven as it is made. Fails with ADAPT_ERROR_MEMORY_LIMIT when the
 * budget does not hold two models (adapt_verified_memory_bound); a failed
 * proof is ADAPT_ERROR_VERIFICATION, and the bytes written before it are
 * not to be trusted. */
adapt_status adapt_encode_stream_verified(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                          void *write_context, adapt_sizes sizes, uint64_t max_memory,
                                          adapt_stats *stats, adapt_error *error);

/* adapt_encode_stream_verified() with the segment length spelled out. */
adapt_status adapt_encode_stream_verified_segments(adapt_read_fn read, void *read_context, adapt_write_fn write,
                                                   void *write_context, adapt_sizes sizes, uint64_t max_memory,
                                                   uint64_t segment_bytes, adapt_stats *stats, adapt_error *error);

/* Prove an encoded stream decodes to its source with one model: `encoded`
 * is decoded block by block and compared with `source` as both are read.
 * Every fault is ADAPT_ERROR_VERIFICATION. */
adapt_status adapt_verify_stream(adapt_read_fn source, void *source_context, adapt_read_fn encoded,
                                 void *encoded_context, uint64_t max_memory, adapt_stats *stats, adapt_error *error);

/* Encode `data` into one file (malloc'd into `*out`; release with
 * adapt_free), reserving room for the matching in-memory decoder's result
 * when possible. If that result plus the smallest model exceed the budget,
 * the smallest model is used; decode with a larger budget or a stream sink.
 * Does not prove the result. `error` may be NULL. */
adapt_status adapt_encode(const uint8_t *data, size_t data_len, adapt_limits limits, uint8_t **out, size_t *out_len,
                          adapt_error *error);

/* adapt_encode(), and the proof: alongside when the budget holds two
 * models, otherwise after encoding with the model's memory released. A
 * failed proof is ADAPT_ERROR_VERIFICATION, never a file. */
adapt_status adapt_encode_verified(const uint8_t *data, size_t data_len, adapt_limits limits, uint8_t **out,
                                   size_t *out_len, adapt_error *error);

/* CRC-32 (IEEE 802.3) of `data`, as the trailer stores it. */
uint32_t adapt_crc32(const uint8_t *data, size_t len);

/* ------------------------------------------------------------------------
 * Archives: a folder as one stream
 *
 * A folder is coded as a container flagged ADAPT_ARCHIVE_FLAG whose
 * decoded bytes are an adapt archive: ADAPT_ARCHIVE_MAGIC (version 1) or
 * ADAPT_ARCHIVE_METADATA_MAGIC (version 2), then entries,
 * then one ADAPT_ARCHIVE_END byte. An entry is a kind byte, a path (its
 * length as a little-endian u16, then its bytes: components joined by '/',
 * relative to the folder, none empty, none '.' or '..', no NUL) and, for
 * a file, its length as a little-endian u64 and then its bytes; for a
 * symbolic link, its target (u16 length, bytes). Version 2 appends a
 * 28-byte adapt_metadata record after the entry header, before file data;
 * the folder root's metadata lives in the outer container header.
 * Entries come depth first,
 * siblings in byte order of their names, a directory before what it
 * holds, so identical contents and metadata give identical archive bytes.
 * An empty directory is an entry like any other; the folder itself is
 * not one. The unpacker checks every rule and hands the entries to a sink
 * of callbacks as their bytes arrive, so a folder of any size is restored
 * with bounded parser memory. A filesystem sink may retain directory
 * metadata until children are restored; this bookkeeping is additional.
 * --------------------------------------------------------------------- */

#define ADAPT_ARCHIVE_MAGIC "ADPTARC1"
#define ADAPT_ARCHIVE_METADATA_MAGIC "ADPTARC2"
#define ADAPT_ARCHIVE_MAGIC_BYTES 8u
#define ADAPT_ARCHIVE_END 0u
#define ADAPT_ARCHIVE_DIRECTORY 1u
#define ADAPT_ARCHIVE_FILE 2u
#define ADAPT_ARCHIVE_SYMLINK 3u
/* The most bytes a path or a link target may take. */
#define ADAPT_ARCHIVE_PATH_MAX 65535u

/* Whether a path may name an archive entry: 1 to ADAPT_ARCHIVE_PATH_MAX
 * bytes, components joined by '/', none empty, '.' or '..', no NUL. */
int adapt_archive_path_valid(const char *path);

/* Write an entry's header — everything before a file's bytes — into
 * `out` when `cap` holds it, and return its length; 0 when the kind is
 * not an entry's, or the path or the link target is not valid. `size` is
 * a file's length, `target` a link's target, ignored for the other kinds.
 * The archive's start is ADAPT_ARCHIVE_MAGIC, its end one
 * ADAPT_ARCHIVE_END byte; a caller emits those itself. */
size_t adapt_archive_entry(uint8_t kind, const char *path, uint64_t size, const char *target, uint8_t *out, size_t cap);
/* Version 2 appends metadata after each version-1 entry header. Its root
 * metadata is in the outer container header. Both archive versions decode. */
size_t adapt_archive_entry_metadata(uint8_t kind, const char *path, uint64_t size, const char *target,
                                    const adapt_metadata *metadata, uint8_t *out, size_t cap);

/* What an unpacker does with each entry: `directory` for a directory,
 * `file_open` then `file_data` for each of the file's bytes as they come
 * and `file_close` once its length is reached (a file of length 0 is
 * opened and closed with no data), `symlink` for a link. Each returns 0,
 * or a nonzero status the unpacker returns as its own. */
typedef struct adapt_archive_sink {
    void *context;
    int (*directory)(void *context, const char *path);
    int (*file_open)(void *context, const char *path, uint64_t size);
    int (*file_data)(void *context, const uint8_t *data, size_t len);
    int (*file_close)(void *context);
    int (*symlink)(void *context, const char *path, const char *target);
} adapt_archive_sink;

typedef struct adapt_archive_unpacker adapt_archive_unpacker;
/* Called after an entry's bytes are complete (after file_close for files).
 * Directory metadata must be applied after their children are restored. */
typedef int (*adapt_archive_metadata_fn)(void *context, const char *path, uint8_t kind, const adapt_metadata *metadata);

/* An unpacker for one archive, or NULL when memory is short. */
adapt_archive_unpacker *adapt_archive_unpacker_new(adapt_archive_sink sink);
void adapt_archive_set_metadata_callback(adapt_archive_unpacker *u, adapt_archive_metadata_fn callback);
void adapt_archive_unpacker_free(adapt_archive_unpacker *u);

/* Feed the next bytes of the archive, in order, in any pieces: an
 * adapt_write_fn, so a decoder writes straight into it. Returns 0; the
 * sink's status when a callback failed; or ADAPT_ARCHIVE_MALFORMED when
 * the bytes broke a rule, after which every further call returns the
 * same and adapt_archive_problem() says where. */
int adapt_archive_unpack(void *unpacker, const uint8_t *buf, size_t len);
#define ADAPT_ARCHIVE_MALFORMED (-1)

/* After the last byte: ADAPT_OK when the archive ended where the bytes
 * did — the end byte seen, no file cut short, nothing after —
 * ADAPT_ERROR_BAD_ARCHIVE otherwise, with the offset in `error`. */
adapt_status adapt_archive_finish(const adapt_archive_unpacker *u, adapt_error *error);

/* The rule the bytes broke, or NULL when none did; and the offset of the
 * byte refused. */
const char *adapt_archive_problem(const adapt_archive_unpacker *u, uint64_t *offset);

/* Entries handed to the sink so far: directories, files and links. */
uint64_t adapt_archive_entries(const adapt_archive_unpacker *u);

#ifdef __cplusplus
}
#endif

#endif /* ADAPT_H */
