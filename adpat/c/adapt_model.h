/*
 * adapt_model.h — the codec's internals, shared by adapt_decode.c and
 * adapt_encode.c: the constants, the bit-history state machine, the binary
 * arithmetic coder (both sides), the predictor, the checksum, the framing
 * rules and the block decoder. Not part of the public interface, which is
 * adapt.h.
 *
 * Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
 *
 * Everything here is `static inline`, except predict() and update(), which
 * are `static` and compiled per instruction set (ADAPT_X86_V3 below):
 * the per-bit path is called eight times a byte from both the encoder and
 * the decoder, and a call across translation units would cost what
 * inlining saves. Model arithmetic is integer-only: encoder and decoder
 * must agree on rounding, overflow and update order. Weight wrapping uses
 * unsigned arithmetic; negative right shifts require arithmetic semantics,
 * checked by a compile-time assertion below.
 */
#ifndef ADAPT_MODEL_H
#define ADAPT_MODEL_H

#include "adapt.h"
#include "adapt_adaptive.h"
#include "adapt_bmp.h"

#include <stdlib.h>
#include <string.h>

#ifdef ADAPT_SHADOW_CHECK
#include <stdio.h>
#endif

/* Normal/max prediction and update paths have baseline and x86-64-v3
 * instantiations under supported GCC/Clang builds. Each model selects its
 * function pair from available CPU features. Both paths use identical integer
 * semantics. ADAPT_NO_MULTIVERSION selects only baseline; fast stays scalar. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(ADAPT_NO_MULTIVERSION)
/* Each profile's predict() and update() are instantiated twice, the
 * second with the x86-64-v3 target and the constant `v3` set, so the
 * arbiters' dot product takes its hand-written AVX2 body (dot_v3) in
 * that one and the portable body in the other; ADAPT_PROFILE picks the
 * pair once per block from the CPU test in model_new(). Separate
 * instantiations keep AVX2 intrinsics out of the baseline function. */
#define ADAPT_X86_V3 1
#define ADAPT_TARGET_V3 __attribute__((target("arch=x86-64-v3")))
#include <immintrin.h>
#else
#define ADAPT_X86_V3 0
#define ADAPT_TARGET_V3
#endif

/* The model's tables live in one arena. On Linux it is an anonymous
 * mapping aligned to 2 MiB and advised MADV_HUGEPAGE: the context table,
 * the recall table and the APM are hashed or indexed at random over tens
 * of megabytes, which can exceed TLB coverage with 4 KiB pages.
 * With huge pages the same tables need a few dozen entries. The advice is
 * a hint the kernel may decline (transparent_hugepage set to `never`), in
 * which case the mapping behaves as ordinary memory; the bytes written
 * are the same either way. A translation unit that wants the mapping must
 * define _DEFAULT_SOURCE before its first include, as adapt_decode.c and
 * adapt_encode.c do; ADAPT_NO_HUGEPAGES, or any other platform, takes
 * the calloc path. */
#if defined(__linux__) && defined(_DEFAULT_SOURCE) && !defined(ADAPT_NO_HUGEPAGES)
#include <sys/mman.h>
#define ADAPT_ARENA_MMAP 1
#endif
#define ARENA_ALIGN ((size_t)2 << 20)

/* For the bodies of predict() and update(), which take the profile as a
 * constant and are instantiated twice: inlined into each instantiation
 * without fail, so the constant folds and each profile's path is its own
 * code. Other per-bit helpers use ordinary compiler inlining decisions. */
#if defined(__GNUC__) || defined(__clang__)
#define ADAPT_ALWAYS_INLINE __attribute__((always_inline))
#else
#define ADAPT_ALWAYS_INLINE
#endif

/* A hint to bring a line toward the core ahead of its use; nothing
 * without the builtin. */
#if defined(__GNUC__) || defined(__clang__)
#define ADAPT_PREFETCH(p) __builtin_prefetch((p))
#else
#define ADAPT_PREFETCH(p) ((void)0)
#endif

#define ADAPT_STATIC_ASSERT(cond, name) typedef char adapt_static_assert_##name[(cond) ? 1 : -1]

ADAPT_STATIC_ASSERT((-1) >> 1 == -1, right_shift_of_a_negative_is_arithmetic);
ADAPT_STATIC_ASSERT(sizeof(size_t) >= 4, size_t_holds_a_block);


#include "adapt_constants.h"
#include "adapt_states.h"
#include "adapt_coder.h"
#include "adapt_tables.h"
#include "adapt_model_state.h"
#include "adapt_model_byte.h"
#include "adapt_model_bit.h"
#include "adapt_model_new.h"
#include "adapt_blocks.h"
#include "adapt_crc32.h"

#endif /* ADAPT_MODEL_H */
