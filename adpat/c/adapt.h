/*
 * adapt.h — the adapt codec, every container version ADAPT_VERSIONS names
 * (19 through 85), in C99 with nothing beyond the C standard library (and
 * POSIX threads for the encoder's proof and the segment workers, when
 * built with ADAPT_THREADS).
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 * Containers are self-contained. ADAPT_VERSIONS defines the accepted wire
 * formats; changing predictor arithmetic or framing requires an unambiguous
 * version or policy field. Versions 13-15 require cadapt 0.11.0 to decode.
 *
 * Decoding: adapt_decode() holds a file in memory and returns its bytes
 * only after their checksum matched; adapt_decode_stream() pulls it through
 * a reader callback and pushes each block to a writer as it is recovered,
 * so that memory is the model and one block whatever the length — bytes it
 * has already written are not to be trusted until it returns ADAPT_OK.
 * Ambiguous historical model2 files (58/61 below table_log 21) use
 * temporary disk for a consumed input prefix and tentative output, then
 * emit only after checksum verification; memory still holds one model.
 *
 * Encoding: adapt_encode() codes bytes held in memory into one file;
 * adapt_encode_stream() reads a stream once, a block at a time, and writes
 * as it goes. adapt_encode_verified() and adapt_encode_stream_verified()
 * prove the result decodes back to the input before returning it, with a
 * second model that decodes every block as it is written; adapt_verify_stream()
 * is the same proof after the fact, with one model, for a source that can
 * be read twice. Sizes come from adapt_sizes_for_input(), which fits the
 * model to the input's length and a memory budget.
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

/* ---- The contract ---------------------------------------------------------
 * What every declaration below is held to; the examples in examples/ and
 * tests/test_adapt_decode.c (test_published_versions, test_contract) are
 * the executable form.
 *
 * Ownership. A buffer the caller passes stays the caller's: `file`,
 * `data`, `header`, the callbacks' buffers and every struct argument are
 * read during the call and no pointer to them is kept. What a function
 * returns through `uint8_t **out` — adapt_decode(), adapt_encode(),
 * adapt_encode_verified() — is malloc'd and the caller's to release with
 * adapt_free(), which accepts NULL; on any status but ADAPT_OK they
 * leave `*out` NULL and `*out_len` 0, so adapt_free(*out) is always
 * safe. An adapt_archive_unpacker is the
 * caller's from adapt_archive_unpacker_new() to
 * adapt_archive_unpacker_free() (NULL accepted); the sink struct is
 * copied, its `context` handed back to every callback.
 *
 * Lifetimes. A pointer a callback receives — the buffer a reader fills,
 * the bytes a writer takes, an archive entry's path, target, attribute
 * name and value, a file's data — is valid for that call only; a sink
 * that needs it later copies it. adapt_sizes, adapt_limits, adapt_stats
 * and adapt_error are plain values with nothing allocated behind them.
 *
 * Threads. The library keeps no global state; any number of calls may
 * run at once on different threads, each with its own arguments. A
 * caller's callbacks are invoked only on the thread that made the call
 * — never on a worker adapt_encode_stream_threads() or
 * adapt_decode_threads() starts, never after the call returns, and never
 * two at once for one call. Nothing here is async-signal-safe.
 *
 * Errors. A function that can fail returns an adapt_status and, when
 * `error` is not NULL, writes it whole: ADAPT_OK with every number 0 on
 * success, so `error->status` alone may be tested; otherwise the status
 * and the numbers its message needs, which adapt_error_message() renders.
 * A required pointer that is NULL is ADAPT_ERROR_INVALID_ARGUMENT;
 * `error` and `stats` are never required; `data` may be NULL only with
 * `data_len` 0 (an empty input codes to ADAPT_HEADER_BYTES +
 * ADAPT_TRAILER_BYTES bytes). A status is final for the call: nothing
 * is retried and nothing partial is published — adapt_decode() holds its
 * result until the checksum matched, adapt_encode_verified() until the
 * proof held; a stream sink may have taken bytes before a failure was
 * found, and they are not to be trusted. `stats` is meaningful only on
 * ADAPT_OK.
 *
 * Limits. adapt_limits caps what a decode may take — `max_memory`
 * against adapt_decode_memory_bound(), `max_output` against the declared
 * length — refusing before anything is allocated, with the numbers in
 * `error` (RUN-4); adapt_default_limits() names the defaults. The sizes
 * the format allows, which adapt_sizes_valid() tests: table_log 10..26,
 * match_log 10..24, window_log 16..30, class_code 0..2, set_code 0..7,
 * period 0 or 1..ADAPT_PERIOD_MAX, table_frac 0..ADAPT_SIZING_FRAC_MAX.
 * A segment is a whole number of ADAPT_BLOCK_BYTES, one block to 2^32 - 1
 * bytes (adapt_segment_bytes_valid()); at most ADAPT_WORKERS_MAX workers
 * run; a thread count beyond what the budget holds is refused as the
 * budget's — the memory bounds saturate, they never wrap. An archive
 * path or link target is at most ADAPT_ARCHIVE_PATH_MAX bytes, an
 * attribute name ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX, a value
 * ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX, a sparse file
 * ADAPT_ARCHIVE_EXTENTS_MAX extents, and the unpacker's memory is
 * bounded by those, never by the archive's length.
 *
 * Compatibility. The interface is these C99 declarations, compiled and
 * linked statically (libadapt.a); it is source-stable within a major
 * version of the release `cadapt --version` reports (CHANGELOG.md):
 * a function keeps its name, arguments and meaning, a constant its
 * value, an enum member its number. Caller-allocated structs —
 * adapt_sizes, adapt_stats, adapt_version_info — may gain fields at
 * their end in a minor release; code compiled against an older header
 * is rebuilt, never binary-patched. Every container version in
 * ADAPT_VERSIONS decodes forever (DLV-1, docs/CONSTRAINTS.md), and the
 * decoder suite holds a vector for each one an encoder ever wrote.
 * Anything in adapt_model.h and the section headers it includes is
 * internal (SUPPORT.md).
 * ----------------------------------------------------------------------- */

/* Normal-profile container version. Accepted formats are enumerated below;
 * unknown versions are rejected before model allocation. */
#define ADAPT_FORMAT_VERSION 19u
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
 * file may read the table first and decode the segments at once, as
 * adapt_decode_threads() does and adapt_encode_stream_threads() codes
 * them, on threads, to the bytes the in-order coder writes. */
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
/* Fast-profile stream: two selected hashed contexts, direct order-1 memory,
 * word run, recall and one arbiter. The context-set code indexes this profile's
 * ADAPT_FAST_SETS menu. */
#define ADAPT_FAST_VERSION 20u
/* The version a max-profile file carries: the normal model with ten more
 * hashed contexts (the previous word with the current, the first and
 * third bytes back, the bytes that followed the order-2 context, the high
 * nibbles of the last four bytes, the markup nesting, orders 5 and 8, the
 * case-folded word, the last three words, the word with the classes
 * before it), run memories on orders 1, 3, 4 and 6, and three arbiter
 * inputs per context instead of one. `cadapt encode --max`. */
#define ADAPT_MAX_VERSION 21u
/* Versions 22--24 use the same predictors as 19--21 and append a
 * checksummed filesystem-metadata record to the header. */
#define ADAPT_METADATA_VERSION 22u
#define ADAPT_METADATA_FAST_VERSION 23u
#define ADAPT_METADATA_MAX_VERSION 24u
/* Experimental causal profiles: rates, sparse and retained. Their metadata
 * variants add three, as the legacy profiles do. No external state. */
#define ADAPT_ADAPTIVE_VERSION 25u
#define ADAPT_ADAPTIVE_LAST_VERSION 27u
#define ADAPT_ADAPTIVE_METADATA_VERSION 28u
#define ADAPT_ADAPTIVE_METADATA_LAST_VERSION 30u
/* BMP24 residual transform with normal/rates/sparse/retained predictors.
 * Twelve little-endian bytes (pixel offset, width, row count) follow the
 * optional period. Metadata variants add four to the raw version. */
#define ADAPT_BMP_VERSION 31u
#define ADAPT_BMP_LAST_VERSION 34u
#define ADAPT_BMP_METADATA_VERSION 35u
#define ADAPT_BMP_METADATA_LAST_VERSION 38u
/* The same experiments over the max predictor, with distinct wire IDs. */
#define ADAPT_MAX_ADAPTIVE_VERSION 39u
#define ADAPT_MAX_ADAPTIVE_LAST_VERSION 41u
#define ADAPT_MAX_ADAPTIVE_METADATA_VERSION 42u
#define ADAPT_MAX_ADAPTIVE_METADATA_LAST_VERSION 44u
#define ADAPT_MAX_BMP_VERSION 45u
#define ADAPT_MAX_BMP_LAST_VERSION 48u
#define ADAPT_MAX_BMP_METADATA_VERSION 49u
#define ADAPT_MAX_BMP_METADATA_LAST_VERSION 52u
/* Max profile with compact side tables (53; 56 with metadata). The third
 * and fourth arbiters use 2^15 weight sets and the second estimation stage
 * uses 2^17 rows. Smaller fixed tables leave more budget for contexts.
 * Version 21 retains its full side-table layout regardless of table size. */
#define ADAPT_MAX_COMPACT_VERSION 53u
#define ADAPT_MAX_COMPACT_METADATA_VERSION 56u
#define ADAPT_COMPACT_TABLE_LOG 22u
/* Compact max model with a lean context set (54; 57 with metadata). It
 * omits order-8, third-word and word/class contexts. The version fixes this
 * set; version 53 retains its full context set. */
#define ADAPT_MAX_LEAN_VERSION 54u
#define ADAPT_MAX_LEAN_METADATA_VERSION 57u
/* Sized max profile (58; 61 with metadata). One sizing byte follows the
 * fixed header and optional period, before metadata. Bit 0 selects compact
 * side tables; bit 1 selects lean contexts. Bits 2-4 encode fraction f,
 * 0..7: (8 + f) << (table_log - 3) blocks, addressed by range multiply.
 * Bit 5 enables the text transform. Bit 6 selects initial arbiter weight
 * 1/32 rather than 1/4. Bit 7 selects model2: int16 first-layer weights,
 * rounded training, class/indirect/recall contexts, four-position recall
 * buckets and a fifth arbiter. Current model2 policies gate the indirect
 * and recall slots and fifth arbiter at table_log >= MODEL2_PRESSURE_LOG.
 * Versions 58/61 can contain either small-table policy; decoding uses
 * checksum-verified fallback. Versions 72/75 identify the pressure-gated
 * policy unambiguously. The version table and sizing validator define which
 * flag combinations each family accepts. */
#define ADAPT_MAX_SIZED_VERSION 58u
#define ADAPT_MAX_SIZED_METADATA_VERSION 61u
/* Pressure-gated model2 has its own wire identity. Versions 58/61 with
 * bit 7 were emitted with two predictors; the decoder checks both when
 * their small-table behavior differs, using temporary files and one model. */
#define ADAPT_MAX_PRESSURE_VERSION 72u
#define ADAPT_MAX_PRESSURE_METADATA_VERSION 75u
#define ADAPT_SIZING_BYTES 1u
#define ADAPT_SIZING_COMPACT 1u
#define ADAPT_SIZING_LEAN 2u
#define ADAPT_SIZING_FRAC_SHIFT 2u
#define ADAPT_SIZING_FRAC_MAX 7u
#define ADAPT_SIZING_TEXT 32u
#define ADAPT_SIZING_LOW_START 64u
#define ADAPT_SIZING_MODEL2 128u
/* The text transform (adapt_text.h) ahead of the normal, fast, max,
 * compact and lean models: versions 62 to 66 in that order, 67 to 71 with
 * metadata (ADAPT_TEXT_METADATA_OFFSET more; 58 to 61 are left to the
 * sized max profile, whose sizing byte has room for a text bit). The
 * payload is the input
 * with every word both sides have seen three times replaced, from then
 * on, by a code of one to three bytes at or above 0x80; each side builds
 * the same book of words as it goes, so nothing is sent ahead and
 * nothing is agreed in advance (FMT-2, FMT-8: the model still sees only
 * bytes, and a fixed rule of the format carries no data). The decoder
 * undoes it as it writes; the trailer's length and checksum are the
 * original's. The encoder takes it when the first block looks like text
 * (text_detect) or when requested (adapt_sizes.text).
 * Never with the adaptive predictors, the BMP transform or the x86
 * transform. A text version names a power-of-two table; with a table
 * fraction the transform rides the sized version's sizing byte instead
 * (ADAPT_SIZING_TEXT), so the two are taken together. Each side's book,
 * index and buffers are under 3 MiB (TEXT_STATE_BYTES), part of what
 * HEADROOM allows for. */
#define ADAPT_TEXT_VERSION 62u
#define ADAPT_TEXT_FAST_VERSION 63u
#define ADAPT_TEXT_MAX_VERSION 64u
#define ADAPT_TEXT_MAX_COMPACT_VERSION 65u
#define ADAPT_TEXT_MAX_LEAN_VERSION 66u
#define ADAPT_TEXT_LAST_VERSION 66u
#define ADAPT_TEXT_METADATA_OFFSET 5u
#define ADAPT_TEXT_METADATA_VERSION 67u
#define ADAPT_TEXT_METADATA_LAST_VERSION 71u
/* The mid profile (adapt_sizes.mid, `cadapt encode --mid`): the normal
 * profile's tables, tiers and estimation stages with seven hashed
 * contexts fixed by the profile (orders 2, 3, 4 and 6, word, record and
 * the class history; the width still chosen per file) and its first
 * arbiter alone as the verdict — no second, third, fourth or final
 * arbiter. Versions 76
 * (78 with metadata), 77 under the text transform (79 with metadata). */
#define ADAPT_MID_VERSION 76u
#define ADAPT_TEXT_MID_VERSION 77u
#define ADAPT_MID_METADATA_OFFSET 2u
#define ADAPT_MID_METADATA_VERSION 78u
#define ADAPT_TEXT_MID_METADATA_VERSION 79u
/* Keyed max profile: version 72 sizing with alternative second, third and
 * fourth arbiter keys. ADAPT_KEYS_TEXT uses recent byte classes, hashed class
 * history, and nesting/tag state. ADAPT_KEYS_BINARY uses the class history
 * and recall state. The encoder compares candidates on the first block;
 * that sample comparison does not guarantee a smaller complete file.
 * Versions 80/81 identify text/binary key policies, 82/83 their metadata forms. */
#define ADAPT_MAX_KEYS_VERSION 80u
#define ADAPT_MAX_KEYS_BINARY_VERSION 81u
#define ADAPT_KEYS_METADATA_OFFSET 2u
#define ADAPT_MAX_KEYS_METADATA_VERSION 82u
#define ADAPT_MAX_KEYS_BINARY_METADATA_VERSION 83u
#define ADAPT_KEYS_CLASSIC 0u
#define ADAPT_KEYS_TEXT 1u
#define ADAPT_KEYS_BINARY 2u
/* The normal profile's arbiters keyed the same way (ADAPT_NORMAL_KEYS_VERSION,
 * 84; 85 with metadata, ADAPT_NORMAL_KEYS_METADATA_OFFSET): the model of
 * version 19 with the key policies of 80 and 81 on its second, third and
 * fourth arbiters. The normal profile has no sizing byte and its text
 * transform is a version family of its own (62, 67), so this version
 * carries one policy byte after the fixed header and the period bytes:
 * bits 0 and 1 the key policy (ADAPT_POLICY_KEYS_MASK; 0 is never
 * written, the classic keys keep 19, 22, 62 and 67), bit 2 the text
 * transform (ADAPT_POLICY_TEXT), the rest 0. The encoder chooses the
 * policy as for the max profile (choose_keys). Never with `fast`, `mid`,
 * `adaptive` or `bmp`. */
#define ADAPT_NORMAL_KEYS_VERSION 84u
#define ADAPT_NORMAL_KEYS_METADATA_VERSION 85u
#define ADAPT_NORMAL_KEYS_METADATA_OFFSET 1u
/* The precise coder (86 to 88; 89 to 91 with metadata,
 * ADAPT_PRECISE_METADATA_OFFSET): the models of versions 72, 80 and 81,
 * in that order, with the final probability handed to the arithmetic
 * coder in sixteen bits instead of twelve. The estimation stages' cells
 * hold sixteen bits; versions 19 to 85 shift their interpolation down
 * to twelve before the coder sees it, which bounds a nearly certain bit
 * at 1/4096 of a bit however sure the model is. Here the two stages'
 * interpolations stay in sixteen bits, their rounded mean is taken three
 * parts to one of the final arbiter's twelve-bit verdict times sixteen,
 * the result is held to 1..65535, and the coder splits its interval by
 * sixteen bits. The model, its learning, the sizing byte and every
 * other field are those of 72, 80 and 81; only the number coded differs.
 * The encoder writes these for every max encode that is neither adaptive
 * nor BMP (adapt_sizes.precise). */
#define ADAPT_MAX_PRECISE_VERSION 86u
#define ADAPT_MAX_PRECISE_KEYS_VERSION 87u
#define ADAPT_MAX_PRECISE_KEYS_BINARY_VERSION 88u
#define ADAPT_PRECISE_METADATA_OFFSET 3u
#define ADAPT_MAX_PRECISE_METADATA_VERSION 89u
#define ADAPT_MAX_PRECISE_KEYS_METADATA_VERSION 90u
#define ADAPT_MAX_PRECISE_KEYS_BINARY_METADATA_VERSION 91u
/* Bits of the probability the coder splits its interval by. */
#define ADAPT_CODER_BITS 12u
#define ADAPT_CODER_BITS_PRECISE 16u
#define ADAPT_POLICY_BYTES 1u
#define ADAPT_POLICY_KEYS_MASK 3u
#define ADAPT_POLICY_TEXT 4u
#define ADAPT_TEXT_AUTO 0u
#define ADAPT_TEXT_ON 1u
/* adapt_sizes.contexts: the encoder's choice of the context set. */
#define ADAPT_CONTEXTS_AUTO 0u
#define ADAPT_CONTEXTS_LEAN 1u
#define ADAPT_TEXT_OFF 2u
/* Minimum known input length for automatic text-transform selection.
 * Below it, or for unknown-length streams, automatic selection is disabled
 * to avoid dictionary startup cost. Callers can request ADAPT_TEXT_ON explicitly. */
#define ADAPT_TEXT_MIN_BYTES ((uint64_t)8 << 20)

/* ---- the version table -------------------------------------------------
 * Every container version the decoder reads, in one place: what its byte
 * means. A row names the profile and the model bits the version fixes,
 * whether it carries the metadata record (and which stream version it is
 * the metadata form of — the offset between the two is the difference),
 * whether a sizing byte (ADAPT_SIZING_*) or a policy byte
 * (ADAPT_POLICY_*) follows the fixed header, and the adaptive mode or
 * BMP mode it names. The decoder's dispatch, the header-length and
 * metadata helpers, the command line's version prose, the tests and the
 * measurement tooling read this table; a new family is a new row (and a
 * pinned vector), never a new copy of the map. The encoder's choice of
 * which version to write for a shape stays in encode_header_version
 * (adapt_model.h): a policy, checked against this table by the tests. */
typedef struct adapt_version_info {
    uint8_t version;
    /* the stream version this is the metadata form of; itself for a stream */
    uint8_t stream;
    uint8_t metadata;
    uint8_t profile; /* ADAPT_PROFILE_* */
    uint8_t adaptive; /* adapt_sizes.adaptive: 1..3 an experimental predictor, the BMP mode 0..3 with `bmp` */
    uint8_t bmp, text, compact, lean, mid;
    /* the byte after the fixed header and the period: none, sizing, policy */
    uint8_t trailing; /* ADAPT_TRAILING_* */
    uint8_t keys;     /* ADAPT_KEYS_* fixed by the version (the policy byte's under ADAPT_TRAILING_POLICY) */
    uint8_t precise;  /* 1 when the coder takes the probability in sixteen bits (ADAPT_MAX_PRECISE_VERSION) */
} adapt_version_info;
#define ADAPT_PROFILE_NORMAL 0u
#define ADAPT_PROFILE_FAST 1u
#define ADAPT_PROFILE_MAX 2u
#define ADAPT_PROFILE_MID 3u
#define ADAPT_TRAILING_NONE 0u
#define ADAPT_TRAILING_SIZING 1u
#define ADAPT_TRAILING_POLICY 2u
#define ADAPT_VERSION_ROW(v, st, md, prof, ad, bm, tx, cp, ln, mi, tr, ky) {v, st, md, prof, ad, bm, tx, cp, ln, mi, tr, ky, 0}
/* the same row for a version of the sixteen-bit coder */
#define ADAPT_VERSION_ROW_PRECISE(v, st, md, prof, ad, bm, tx, cp, ln, mi, tr, ky) {v, st, md, prof, ad, bm, tx, cp, ln, mi, tr, ky, 1}
static const adapt_version_info ADAPT_VERSIONS[] = {
    /* the three profiles as streams and as named files or folders */
    ADAPT_VERSION_ROW(19, 19, 0, ADAPT_PROFILE_NORMAL, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(20, 20, 0, ADAPT_PROFILE_FAST, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(21, 21, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(22, 19, 1, ADAPT_PROFILE_NORMAL, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(23, 20, 1, ADAPT_PROFILE_FAST, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(24, 21, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    /* the experimental predictors over the normal profile, rates / sparse / retained */
    ADAPT_VERSION_ROW(25, 25, 0, ADAPT_PROFILE_NORMAL, 1, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(26, 26, 0, ADAPT_PROFILE_NORMAL, 2, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(27, 27, 0, ADAPT_PROFILE_NORMAL, 3, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(28, 25, 1, ADAPT_PROFILE_NORMAL, 1, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(29, 26, 1, ADAPT_PROFILE_NORMAL, 2, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(30, 27, 1, ADAPT_PROFILE_NORMAL, 3, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    /* the BMP24 transform over the normal profile, alone or with each predictor */
    ADAPT_VERSION_ROW(31, 31, 0, ADAPT_PROFILE_NORMAL, 0, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(32, 32, 0, ADAPT_PROFILE_NORMAL, 1, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(33, 33, 0, ADAPT_PROFILE_NORMAL, 2, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(34, 34, 0, ADAPT_PROFILE_NORMAL, 3, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(35, 31, 1, ADAPT_PROFILE_NORMAL, 0, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(36, 32, 1, ADAPT_PROFILE_NORMAL, 1, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(37, 33, 1, ADAPT_PROFILE_NORMAL, 2, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(38, 34, 1, ADAPT_PROFILE_NORMAL, 3, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    /* the same over the max profile */
    ADAPT_VERSION_ROW(39, 39, 0, ADAPT_PROFILE_MAX, 1, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(40, 40, 0, ADAPT_PROFILE_MAX, 2, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(41, 41, 0, ADAPT_PROFILE_MAX, 3, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(42, 39, 1, ADAPT_PROFILE_MAX, 1, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(43, 40, 1, ADAPT_PROFILE_MAX, 2, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(44, 41, 1, ADAPT_PROFILE_MAX, 3, 0, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(45, 45, 0, ADAPT_PROFILE_MAX, 0, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(46, 46, 0, ADAPT_PROFILE_MAX, 1, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(47, 47, 0, ADAPT_PROFILE_MAX, 2, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(48, 48, 0, ADAPT_PROFILE_MAX, 3, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(49, 45, 1, ADAPT_PROFILE_MAX, 0, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(50, 46, 1, ADAPT_PROFILE_MAX, 1, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(51, 47, 1, ADAPT_PROFILE_MAX, 2, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(52, 48, 1, ADAPT_PROFILE_MAX, 3, 1, 0, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    /* the max profile with compact side tables, and with the lean set as well */
    ADAPT_VERSION_ROW(53, 53, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 1, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(54, 54, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 1, 1, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(56, 53, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 1, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(57, 54, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 1, 1, 0, ADAPT_TRAILING_NONE, 0),
    /* the max profile sized to its budget: the sizing byte (58/61 historical, both model2 policies) */
    ADAPT_VERSION_ROW(58, 58, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, 0),
    ADAPT_VERSION_ROW(61, 58, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, 0),
    /* the text transform ahead of the normal, fast, max, compact and lean models */
    ADAPT_VERSION_ROW(62, 62, 0, ADAPT_PROFILE_NORMAL, 0, 0, 1, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(63, 63, 0, ADAPT_PROFILE_FAST, 0, 0, 1, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(64, 64, 0, ADAPT_PROFILE_MAX, 0, 0, 1, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(65, 65, 0, ADAPT_PROFILE_MAX, 0, 0, 1, 1, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(66, 66, 0, ADAPT_PROFILE_MAX, 0, 0, 1, 1, 1, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(67, 62, 1, ADAPT_PROFILE_NORMAL, 0, 0, 1, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(68, 63, 1, ADAPT_PROFILE_FAST, 0, 0, 1, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(69, 64, 1, ADAPT_PROFILE_MAX, 0, 0, 1, 0, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(70, 65, 1, ADAPT_PROFILE_MAX, 0, 0, 1, 1, 0, 0, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(71, 66, 1, ADAPT_PROFILE_MAX, 0, 0, 1, 1, 1, 0, ADAPT_TRAILING_NONE, 0),
    /* the second max model, pressure gated: the sizing byte with its bit 7 required */
    ADAPT_VERSION_ROW(72, 72, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, 0),
    ADAPT_VERSION_ROW(75, 72, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, 0),
    /* the mid profile, plain and under the text transform */
    ADAPT_VERSION_ROW(76, 76, 0, ADAPT_PROFILE_MID, 0, 0, 0, 0, 0, 1, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(77, 77, 0, ADAPT_PROFILE_MID, 0, 0, 1, 0, 0, 1, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(78, 76, 1, ADAPT_PROFILE_MID, 0, 0, 0, 0, 0, 1, ADAPT_TRAILING_NONE, 0),
    ADAPT_VERSION_ROW(79, 77, 1, ADAPT_PROFILE_MID, 0, 0, 1, 0, 0, 1, ADAPT_TRAILING_NONE, 0),
    /* the second max model with the arbiters keyed by the classes and nesting, or the classes and recall */
    ADAPT_VERSION_ROW(80, 80, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_TEXT),
    ADAPT_VERSION_ROW(81, 81, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_BINARY),
    ADAPT_VERSION_ROW(82, 80, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_TEXT),
    ADAPT_VERSION_ROW(83, 81, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_BINARY),
    /* the normal profile keyed the same way: the policy byte names the keys and the transform */
    ADAPT_VERSION_ROW(84, 84, 0, ADAPT_PROFILE_NORMAL, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_POLICY, 0),
    ADAPT_VERSION_ROW(85, 84, 1, ADAPT_PROFILE_NORMAL, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_POLICY, 0),
    /* the second max model, classic and keyed, with the sixteen-bit coder */
    ADAPT_VERSION_ROW_PRECISE(86, 86, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, 0),
    ADAPT_VERSION_ROW_PRECISE(87, 87, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_TEXT),
    ADAPT_VERSION_ROW_PRECISE(88, 88, 0, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_BINARY),
    ADAPT_VERSION_ROW_PRECISE(89, 86, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, 0),
    ADAPT_VERSION_ROW_PRECISE(90, 87, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_TEXT),
    ADAPT_VERSION_ROW_PRECISE(91, 88, 1, ADAPT_PROFILE_MAX, 0, 0, 0, 0, 0, 0, ADAPT_TRAILING_SIZING, ADAPT_KEYS_BINARY),
};
#define ADAPT_VERSION_COUNT (sizeof(ADAPT_VERSIONS) / sizeof(ADAPT_VERSIONS[0]))

/* The row of a version, or NULL for a version the decoder does not know. */
static inline const adapt_version_info *adapt_version_lookup(uint8_t version) {
    size_t i;
    for (i = 0; i < ADAPT_VERSION_COUNT; i++) {
        if (ADAPT_VERSIONS[i].version == version) return &ADAPT_VERSIONS[i];
    }
    return NULL;
}

/* The metadata form of a stream version, or 0 when it has none. */
static inline uint8_t adapt_version_with_metadata(uint8_t stream) {
    size_t i;
    for (i = 0; i < ADAPT_VERSION_COUNT; i++) {
        if (ADAPT_VERSIONS[i].metadata && ADAPT_VERSIONS[i].stream == stream) return ADAPT_VERSIONS[i].version;
    }
    return 0;
}
#define ADAPT_BMP_BYTES 12u
#define ADAPT_METADATA_BYTES 28u
#define ADAPT_METADATA_HEADER_BYTES (ADAPT_METADATA_BYTES + 4u)
#define ADAPT_HEADER_MAX_BYTES \
    (ADAPT_HEADER_BYTES + ADAPT_PERIOD_BYTES + ADAPT_BMP_BYTES + ADAPT_SIZING_BYTES + ADAPT_METADATA_HEADER_BYTES)
/* (the policy byte of the keyed normal profile takes the sizing byte's
 * place in that bound: no version carries both) */

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
/* Extend a valid header without metadata into out, returning its full length or 0.
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
/* Default SDK output limit returned by adapt_default_limits(): 1 GiB.
 * The CLI explicitly disables default decode caps unless options supply them. */
#define ADAPT_DEFAULT_MAX_OUTPUT ((uint64_t)1 << 30)
/* Default SDK working-memory limit and default CLI encoding budget: 674 MiB.
 * The CLI explicitly disables the default decode cap; SDK callers choose limits. */
#define ADAPT_DEFAULT_MAX_MEMORY ((uint64_t)674 << 20)

/* Operation status. adapt_error_message() formats diagnostic details. */
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
    ADAPT_ERROR_BAD_ARCHIVE,
    /* The text transform's stream is malformed: a prelude that is no
     * dictionary, a code past it, a flag or an escape that nothing
     * follows, or a stream that ends inside one. */
    ADAPT_ERROR_BAD_TEXT
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

/* Format the diagnostic for `error` into `buf`
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
    /* 1 for the mid profile (ADAPT_MID_VERSION): the normal profile's
     * tables and stages, seven fixed hashed contexts, one arbiter. Never
     * with `fast`, `max`, `adaptive` or `bmp`; set_code is 0 (the profile
     * names its set), compact, lean and model2 are 0. adapt_sizes_mid
     * turns normal sizes to it, as `adapt encode --mid` does. */
    uint8_t mid;
    /* The arbiters' key policy under the max and normal profiles:
     * ADAPT_KEYS_CLASSIC, ADAPT_KEYS_TEXT or ADAPT_KEYS_BINARY
     * (ADAPT_MAX_KEYS_VERSION, ADAPT_NORMAL_KEYS_VERSION). Chosen by the
     * encoder on the first block, read from the version or the policy
     * byte; 0 under `fast`, `mid`, `adaptive` and `bmp`. */
    uint8_t keys;
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
    /* 0 legacy; 1 fast/slow calibration; 2 adds sparse contexts; 3 adds
     * retained calibration. Composes with max; mutually exclusive with fast. */
    uint8_t adaptive;
    /* Request BMP24 transform on encode (parameters are detected); parsed
     * descriptor on decode. Exclusive with fast, archive and x86. */
    uint8_t bmp;
    uint32_t bmp_offset, bmp_width, bmp_rows;
    /* 1 when the max profile runs its compact side tables
     * (ADAPT_MAX_COMPACT_VERSION); never with `fast`, `adaptive` or `bmp`.
     * adapt_sizes_prioritize_context sets it from the budget (see
     * ADAPT_COMPACT_TABLE_LOG); a decoder sets it from the version, so an
     * older max file keeps its full tables. */
    uint8_t compact;
    /* 1 when the max model leaves out its lean set's three contexts (o8,
     * word3, wordc: ADAPT_MAX_LEAN_VERSION, or the sizing byte's
     * ADAPT_SIZING_LEAN). The context policy sets it with `compact` at a
     * table of ADAPT_COMPACT_TABLE_LOG blocks or fewer; ADAPT_CONTEXTS_LEAN
     * sets it with the full side tables too, for speed. A decoder sets it
     * from the version or the sizing byte. */
    uint8_t lean;
    /* How the encoder chooses the context set: ADAPT_CONTEXTS_AUTO (the
     * default, 0) by coding the first block under each candidate,
     * ADAPT_CONTEXTS_LEAN takes the lean set outright — set 3 (word and
     * field left out) and, under the max profile, `lean` (o8, word3,
     * wordc), reducing the number of contexts evaluated per bit;
     * the class-history width is still chosen. Never with `fast`. The
     * header records the set the encoder took; a decoder leaves this 0. */
    uint8_t contexts;
    /* The context table's fraction, 0 to 7 (ADAPT_SIZING_FRAC_MAX): it
     * holds (8 + table_frac) << (table_log - 3) blocks. Only the sized max
     * profile carries it (ADAPT_MAX_SIZED_VERSION); every other version
     * has 0, a power of two. adapt_sizes_prioritize_context sets it from
     * the budget's slack. */
    uint8_t table_frac;
    /* The text transform (versions 62 to 71): on encode, ADAPT_TEXT_AUTO
     * (the default, 0) takes it when the first block looks like text,
     * ADAPT_TEXT_ON always, ADAPT_TEXT_OFF never; the encoder records the
     * choice it made in the header, and a decoder sets ADAPT_TEXT_ON from
     * the version and 0 from any other. Never with `adaptive`, `bmp` or
     * `x86`. */
    uint8_t text;
    /* 1 when every arbiter starts a context's stretched probability at
     * the weight 1/32 rather than 1/4, so the weights are learned from
     * the input sooner than trusted (ADAPT_SIZING_LOW_START, the sized
     * max profile only); the encoder sets it under the text transform,
     * and a decoder sets it from the sizing byte. */
    uint8_t low_start;
    /* 1 for the second max model (ADAPT_SIZING_MODEL2, the sized max
     * profile only): the encoder sets it for every max encode that is
     * neither adaptive nor BMP, whatever the caller's value, and a decoder
     * from the sizing byte; see ADAPT_MAX_SIZED_VERSION. */
    uint8_t model2;
    /* 1 when the coder takes the probability in sixteen bits
     * (ADAPT_MAX_PRECISE_VERSION): the encoder sets it with `model2`,
     * whatever the caller's value, and a decoder from the version. Only
     * with `model2`. */
    uint8_t precise;
} adapt_sizes;

/* Blocks of the context table `sizes` describes: 2^table_log, or with a
 * fraction (8 + table_frac) << (table_log - 3). */
uint64_t adapt_table_blocks(adapt_sizes sizes);

/* Probe a BMP24 header without decoding text or modifying sizes on failure. */
int adapt_bmp_probe(const uint8_t *prefix, size_t length, adapt_sizes *sizes);

/* Fit an experimental profile, including its additional state, to budget.
 * mode is 0..3 (0 chooses the normal profile). */
adapt_status adapt_sizes_for_input_adaptive(int length_known, uint64_t length, uint64_t max_memory,
                                           unsigned mode, adapt_sizes *sizes, adapt_error *error);
adapt_status adapt_sizes_for_threads_adaptive(uint64_t length, uint64_t max_memory, uint64_t threads,
                                             uint64_t segment_bytes, unsigned mode,
                                             adapt_sizes *sizes, adapt_error *error);

/* Adaptive sizing over the max predictor; mode 0 preserves plain max. */
adapt_status adapt_sizes_for_input_max_adaptive(int length_known, uint64_t length, uint64_t max_memory,
                                               unsigned mode, adapt_sizes *sizes, adapt_error *error);
adapt_status adapt_sizes_for_threads_max_adaptive(uint64_t length, uint64_t max_memory, uint64_t threads,
                                                 uint64_t segment_bytes, unsigned mode,
                                                 adapt_sizes *sizes, adapt_error *error);

/* Context sets a file may choose, by header code. */
#define ADAPT_CONTEXT_SETS 8u

/* The bytes the header of a file whose fixed header is `fixed` occupies:
 * fixed part, optional period, sizing byte, BMP descriptor and filesystem metadata.
 * `fixed` must contain at least ADAPT_HEADER_BYTES bytes. */
size_t adapt_header_len(const uint8_t *fixed);
/* Header size written by the current encoder for resolved sizes (including
 * its chosen period), excluding filesystem metadata. For an existing file,
 * including older versions, use adapt_header_len() on its fixed header. */
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

/* The contexts omitted by the set selected in `sizes`; `info` names
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
/* `sizes` for the mid profile: the normal profile's sizes with `mid` set,
 * the set code cleared (the profile's set is fixed), the max profile's
 * flags cleared. */
adapt_sizes adapt_sizes_mid(adapt_sizes sizes);
/* `sizes` for the max profile: the normal profile's sizes and choices
 * with `max` set, as `adapt encode --max` does. */
adapt_sizes adapt_sizes_max(adapt_sizes sizes);

/* Estimated allocation in bytes of one model at these sizes. */
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

/* Context-table target for an input length, as log2(blocks). Growth is
 * bounded by the format maximum (2^26 blocks) and the caller's budget;
 * this target does not itself allocate memory. */
uint8_t adapt_table_log_for(uint64_t length);

/* Fit model sizes to the input length and memory budget. Set length_known
 * to 0 for an unknown-length stream. Tables and window are bounded by the
 * budget and input-dependent targets. Unknown-length input reserves room
 * for two models when possible so a streaming verifier can run. Class
 * selection is deferred to the encoder's first block. Returns
 * ADAPT_ERROR_MEMORY_LIMIT when even the minimum sizing request cannot fit. */
adapt_status adapt_sizes_for_input(int length_known, uint64_t length, uint64_t max_memory, adapt_sizes *sizes,
                                   adapt_error *error);
/* adapt_sizes_for_input for the max profile: the same rule and the same
 * memory; the result has `max` set. */
adapt_status adapt_sizes_for_input_max(int length_known, uint64_t length, uint64_t max_memory, adapt_sizes *sizes,
                                       adapt_error *error);

/* Segmented-encoding bounds and sizing: each worker needs a model, one
 * block workspace and segment output; shared headroom is counted once.
 * Models fit the requested worker count and segment length. Insufficient
 * budget returns ADAPT_ERROR_MEMORY_LIMIT with required bytes. Bounds
 * saturate on overflow. Scheduling the same segments with fewer active
 * workers preserves the encoded bytes. */
uint64_t adapt_segment_out_bound(uint64_t segment_bytes);
uint64_t adapt_threads_encode_memory_bound(adapt_sizes sizes, uint64_t threads, uint64_t segment_bytes);
adapt_status adapt_sizes_for_threads(uint64_t length, uint64_t max_memory, uint64_t threads, uint64_t segment_bytes,
                                     adapt_sizes *sizes, adapt_error *error);
/* adapt_sizes_for_threads for the max profile. */
adapt_status adapt_sizes_for_threads_max(uint64_t length, uint64_t max_memory, uint64_t threads, uint64_t segment_bytes,
                                         adapt_sizes *sizes, adapt_error *error);

/* Rebalance valid, already fitted sizes toward context capacity, then grow
 * the recall window with the remaining budget. All other fields are preserved.
 * Known input targets adapt_table_log_for(length); unknown input targets the
 * format maximum. Segmented known inputs target one segment and account for
 * every requested worker. Unknown input preserves the two-model reservation
 * used by adapt_sizes_for_input. Failure leaves *sizes unchanged. This changes
 * only existing header fields: no decoder policy or external state is needed.
 * Call before attaching transform flags whose workspace was reserved separately. */
adapt_status adapt_sizes_prioritize_context(int length_known, uint64_t length, uint64_t max_memory,
                                            uint64_t threads, uint64_t segment_bytes,
                                            adapt_sizes *sizes, adapt_error *error);

/* What a coding pass did: original bytes, container bytes, the checksum
 * of the original, and the segments the file has. */
typedef struct adapt_stats {
    uint64_t input_bytes;
    uint64_t output_bytes;
    uint32_t crc32;
    /* Segments the file has: 1 unless it is segmented. */
    uint32_t segments;
    /* Threads the segments were coded or decoded on: 1 unless
     * adapt_encode_stream_threads() or adapt_decode_threads() ran them at
     * once. */
    uint32_t workers;
    /* Under the text transform, the bytes the model coded (the
     * transformed input) and the words the book came to hold; both 0
     * without it. */
    uint64_t text_bytes;
    uint32_t text_words;
    /* What the encoder chose from the first block (choose_model in
     * adapt_encode.c): the class-history width and context set as header
     * codes, the arbiters' key policy (ADAPT_KEYS_*), the record period;
     * and the trials that decided them — how many sample codings, and
     * the bytes they coded. All 0 for a decode, and for an input the
     * model does not run on (empty, or incompressible at a glance). */
    uint8_t class_code, set_code, keys;
    /* the lean context set taken (adapt_sizes.lean), 1 or 0 */
    uint8_t lean;
    uint16_t period;
    uint32_t trials;
    uint64_t trial_bytes;
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

/* Decode a complete file held in memory, its segments decoded at once on
 * up to `threads` workers when the file is segmented — the table before
 * the trailer says where each segment's bytes are — and written in order
 * through `write`, the checksum over the whole checked against the
 * trailer at the end. A worker holds a model and the longest segment's
 * bytes (adapt_parallel_decode_memory_bound, which counts the file too);
 * the workers are as many of `threads` as `limits.max_memory` holds
 * (adapt_decode_workers), and with fewer than two, or a file of one
 * segment, adapt_decode_stream() runs over the memory. Built without
 * ADAPT_THREADS, one worker always. `stats` and `error` may be NULL. */
adapt_status adapt_decode_threads(const uint8_t *file, size_t file_len, adapt_limits limits, uint64_t threads,
                                  adapt_write_fn write, void *write_context, adapt_stats *stats, adapt_error *error);
uint64_t adapt_parallel_decode_memory_bound(adapt_sizes sizes, uint64_t workers, uint64_t segment_raw_max,
                                            uint64_t file_len);
uint64_t adapt_decode_workers(adapt_sizes sizes, uint64_t max_memory, uint64_t segment_raw_max, uint64_t file_len,
                              uint64_t threads);

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
 * segments precedes the trailer; parallel workers write the same bytes.
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

/* adapt_encode_stream_segments() with the segments coded at once, on up
 * to `threads` workers, when `sizes.segmented` is set: each segment goes
 * under a fresh model on its own thread, and the segments are written in
 * order, so the bytes are the in-order encoder's. With `verify` each
 * worker proves its segment — decodes its blocks under another fresh
 * model and compares — before it is written. A worker holds a model, a
 * segment's input and coded form and a block's workspace
 * (adapt_parallel_encode_memory_bound); the workers are as many of
 * `threads` as `max_memory` holds (adapt_encode_workers), and with fewer
 * than two the in-order encoder runs, proven alongside when `verify` is
 * set, which then needs adapt_verified_memory_bound. `stats->workers`
 * says how many ran. Built without ADAPT_THREADS, one worker always.
 * `stats` and `error` may be NULL. */
adapt_status adapt_encode_stream_threads(adapt_read_fn read, void *read_context, adapt_write_fn write, void *write_context,
                                         adapt_sizes sizes, uint64_t max_memory, uint64_t segment_bytes, uint64_t threads,
                                         int verify, adapt_stats *stats, adapt_error *error);
uint64_t adapt_parallel_encode_memory_bound(adapt_sizes sizes, uint64_t workers, uint64_t segment_bytes);
uint64_t adapt_encode_workers(adapt_sizes sizes, uint64_t max_memory, uint64_t segment_bytes, uint64_t threads);

/* The most workers either side runs segments on. */
#define ADAPT_WORKERS_MAX 64u

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
 * decoded bytes are an adapt archive: ADAPT_ARCHIVE_MAGIC (version 1),
 * ADAPT_ARCHIVE_METADATA_MAGIC (version 2) or ADAPT_ARCHIVE_FULL_MAGIC
 * (version 3), or ADAPT_ARCHIVE_ROOT_MAGIC (version 4, what the CLI
 * encoder writes), then entries, then one
 * ADAPT_ARCHIVE_END byte. An entry is a kind byte, a path (its length as
 * a little-endian u16, then its bytes: components joined by '/', relative
 * to the folder, none empty, none '.' or '..', no NUL) and, for a file,
 * its length as a little-endian u64 and then its bytes; for a symbolic
 * link, its target (u16 length, bytes). Version 2 appends a 28-byte
 * adapt_metadata record after the entry header, before file data; the
 * folder root's metadata lives in the outer container header.
 *
 * Version 3 (FMT-7) adds two kinds and one trailer to every entry:
 *   - a hard link (ADAPT_ARCHIVE_HARDLINK): the path, then the archive
 *     path of an earlier file or sparse-file entry (u16 length, bytes,
 *     the path rules), whose inode it shares; it carries no metadata or
 *     attributes of its own, having its target's;
 *   - a sparse file (ADAPT_ARCHIVE_SPARSE): the path, its apparent
 *     length as a u64, the number of data extents as a u32 (at most
 *     ADAPT_ARCHIVE_EXTENTS_MAX), the metadata record, then each extent
 *     as its offset and length (two u64) followed by its bytes; extents
 *     come in ascending order, none empty, none overlapping, none past
 *     the length, and the bytes between them are holes;
 *   - after an entry's bytes (after the metadata for a directory or a
 *     link), its extended attributes: a u16 count, then per attribute a
 *     u16 name length (1 to ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX), the name
 *     (no NUL), a u32 value length (at most
 *     ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX) and the value. Names come in
 *     byte order, none twice. POSIX ACLs travel here as the attributes
 *     the platform stores them in (system.posix_acl_access and
 *     system.posix_acl_default on Linux).
 * Version 4 adds the root directory's attribute list immediately after
 * the magic, in the same count/name/value layout as an entry's trailer.
 * Entry layouts remain those of version 3. The root is not an entry and
 * is not counted by adapt_archive_entries(); its mode and timestamps
 * still live in the outer container header. A root-attribute callback
 * must be registered to consume a nonempty list; otherwise unpack fails.
 *
 * Entries come depth first, siblings in byte order of their names, a
 * directory before what it holds, so identical contents, metadata,
 * links, holes and attributes give identical archive bytes. An empty
 * directory is an entry like any other; the folder itself is not one.
 * The unpacker checks every rule and hands the entries to a sink of
 * callbacks as their bytes arrive — each extent before its bytes, each
 * attribute as it completes — so a folder of any size is restored with
 * bounded parser memory: one path, one link target, one attribute. A
 * filesystem sink may retain directory metadata until children are
 * restored; this bookkeeping is additional.
 * --------------------------------------------------------------------- */

#define ADAPT_ARCHIVE_MAGIC "ADPTARC1"
#define ADAPT_ARCHIVE_METADATA_MAGIC "ADPTARC2"
#define ADAPT_ARCHIVE_FULL_MAGIC "ADPTARC3"
#define ADAPT_ARCHIVE_ROOT_MAGIC "ADPTARC4"
#define ADAPT_ARCHIVE_MAGIC_BYTES 8u
#define ADAPT_ARCHIVE_END 0u
#define ADAPT_ARCHIVE_DIRECTORY 1u
#define ADAPT_ARCHIVE_FILE 2u
#define ADAPT_ARCHIVE_SYMLINK 3u
#define ADAPT_ARCHIVE_HARDLINK 4u
#define ADAPT_ARCHIVE_SPARSE 5u
/* The most bytes a path or a link target may take. */
#define ADAPT_ARCHIVE_PATH_MAX 65535u
/* The most data extents a sparse entry may list, the longest attribute
 * name, and the longest attribute value (Linux's XATTR_SIZE_MAX). */
#define ADAPT_ARCHIVE_EXTENTS_MAX 65535u
#define ADAPT_ARCHIVE_ATTRIBUTE_NAME_MAX 255u
#define ADAPT_ARCHIVE_ATTRIBUTE_VALUE_MAX 65536u

/* One data extent of a sparse file: where its bytes start and how many. */
typedef struct adapt_extent {
    uint64_t offset;
    uint64_t length;
} adapt_extent;

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
 * metadata is in the outer container header. Every archive version decodes. */
size_t adapt_archive_entry_metadata(uint8_t kind, const char *path, uint64_t size, const char *target,
                                    const adapt_metadata *metadata, uint8_t *out, size_t cap);

/* A version-3 entry's header: for a hard link, `target` is the earlier
 * entry's archive path and `metadata` is ignored; for a sparse file,
 * `size` is the apparent length and `extents` the `extent_count` data
 * extents, checked against the rules above; the bytes of each extent
 * follow its own offset and length, which adapt_archive_extent() writes.
 * Returns the header's length, or 0 when a rule is broken. */
size_t adapt_archive_entry_full(uint8_t kind, const char *path, uint64_t size, const char *target,
                                const adapt_extent *extents, uint32_t extent_count, const adapt_metadata *metadata,
                                uint8_t *out, size_t cap);
/* One extent's offset and length, as written before its bytes: 16 bytes. */
size_t adapt_archive_extent(const adapt_extent *extent, uint8_t *out, size_t cap);
/* An entry's attribute trailer: the count, then each attribute by
 * adapt_archive_attribute(); 0 when a name or value is too long. */
size_t adapt_archive_attributes(uint32_t count, uint8_t *out, size_t cap);
size_t adapt_archive_attribute(const char *name, const uint8_t *value, size_t len, uint8_t *out, size_t cap);

/* What an unpacker does with each entry: `directory` for a directory,
 * `file_open` then `file_data` for each of the file's bytes as they come
 * and `file_close` once its length is reached (a file of length 0 is
 * opened and closed with no data), `symlink` for a link; and, for a
 * version-3 archive, `hardlink` for a hard link, `sparse_open` for a
 * sparse file followed by `extent` before each extent's `file_data` and
 * `file_close` at the end, and `attribute` for each extended attribute
 * once the entry's bytes are complete. Every callback must be set. Each
 * returns 0, or a nonzero status the unpacker returns as its own. */
typedef struct adapt_archive_sink {
    void *context;
    int (*directory)(void *context, const char *path);
    int (*file_open)(void *context, const char *path, uint64_t size);
    int (*file_data)(void *context, const uint8_t *data, size_t len);
    int (*file_close)(void *context);
    int (*symlink)(void *context, const char *path, const char *target);
    int (*hardlink)(void *context, const char *path, const char *target);
    int (*sparse_open)(void *context, const char *path, uint64_t size, uint32_t extents);
    int (*extent)(void *context, uint64_t offset, uint64_t length);
    int (*attribute)(void *context, const char *path, uint8_t kind, const char *name, const uint8_t *value,
                     size_t len);
} adapt_archive_sink;

typedef struct adapt_archive_unpacker adapt_archive_unpacker;
/* Called after an entry's bytes are complete (after file_close for files).
 * Directory metadata must be applied after their children are restored. */
typedef int (*adapt_archive_metadata_fn)(void *context, const char *path, uint8_t kind, const adapt_metadata *metadata);

/* Version-4 root attributes arrive before entries. A filesystem sink must
 * defer applying them until all children have been restored, then apply
 * the root mode/timestamps from the outer header. The callback uses the
 * sink's context and returns 0 or an I/O status, like entry callbacks. */
typedef int (*adapt_archive_root_attribute_fn)(void *context, const char *name, const uint8_t *value, size_t len);

/* An unpacker for one archive, or NULL when memory is short. */
adapt_archive_unpacker *adapt_archive_unpacker_new(adapt_archive_sink sink);
void adapt_archive_set_metadata_callback(adapt_archive_unpacker *u, adapt_archive_metadata_fn callback);
/* NULL is allowed only when the archive has no root attributes. Versions
 * 1--3 do not invoke this callback. Missing support never drops attributes. */
void adapt_archive_set_root_attribute_callback(adapt_archive_unpacker *u, adapt_archive_root_attribute_fn callback);
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

/* Entries handed to the sink so far: directories, files, links, hard
 * links and sparse files. */
uint64_t adapt_archive_entries(const adapt_archive_unpacker *u);

#ifdef __cplusplus
}
#endif

#endif /* ADAPT_H */
