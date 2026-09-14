---
title: Current source compression baseline
date: 2026-09-14
source_model: OpenAI GPT-6
tool: Codex
---

# Current source compression baseline

The user requested the current checkout as the new comparison baseline.
Snapshot `6eb6e41` captures main HEAD `e2ce8ec` plus the uncommitted source,
tests and documentation present at **18:07:24 UTC**. All captured files were
hashed again against the main checkout after copying. The
[source manifest](current-baseline.json) records the complete file list and
hashes. The main working tree and index were not modified.

The snapshot was built with GCC 13.3.0 and the repository's strict C99 flags
(`-O3 -Wall -Wextra -Wpedantic -Werror -DADAPT_THREADS -pthread`). Its frozen
binary SHA-256 is
`bc77916e6fb66535e385d312b1a85cf908fef8fc536ab4dc087d133930ca51b1`.

The comparator is full enwik8: **100,000,000 bytes**, SHA-256
`2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8`.
Normal and max are measured at the default **706,740,224-byte** (674 MiB)
budget; max is also measured at **1,073,741,824 bytes** (1 GiB) to distinguish
predictor improvement from a larger context table. Every comparison uses the
same input, profile and memory budget and counts the entire container. Each
reported successful round trip must pass an independent decoder and exact-byte
comparison. Modes and nanosecond access/modification times are checked before
reading restored content. Encoding verification remains opt-in with `--verify`.

The experiment layers causal calibration, sparse byte contexts, retained
calibration banks and complete-container selection onto this frozen model.
It now composes with `--max`, and a reversible BMP24 candidate is included.
Plain normal, fast and max retain the baseline wire formats 19–24.
The experimental variants use 25–52, as specified in the
[implementation report](causal-byte-implementation.md).

`bench/adaptive_experiment.py --base-profile max --search-only` measures the
baseline and full automatic search. Each trial's complete size is logged;
only the winning search output receives a separate full decode in that mode.
Without `--search-only`, every candidate receives its own measured decode.
C and CLI tests exercise all variants and reject malformed input.

Earlier results on the older normal predictor at 64 MiB are historical and
cannot establish improvement over this source snapshot. A smaller output
obtained solely by increasing the memory budget is likewise not an algorithmic
gain. Measurements and conclusions will be recorded against this snapshot.

[Repository README](../README.md)
