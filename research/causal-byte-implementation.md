---
title: Causal byte adaptation and BMP24 container search
date: 2026-09-14
source_model: OpenAI GPT-6
tool: Codex
status: Experimental implementation; not an enterprise savings guarantee
---

# Causal byte adaptation and BMP24 container search

This worktree implements the causal calibration, sparse-context, retained-state
and complete-container selection hypotheses in
[the mathematical model](byte-only-mathematical-model.md), plus a concrete
reversible BMP24 transform. It keeps the existing normal/fast/max encoders as
defaults and preserves decoding of formats 19–24. It adds no runtime dependency,
learned corpus table, dictionary, tokenizer or sidecar. See the
[repository README](../README.md) and governing
[constraints](../docs/CONSTRAINTS.md).

The implementation is integrated into `main`. The frozen experiment branch
`research/causal-current` is based on source snapshot `6eb6e41`. The [baseline protocol](baseline-2026-09-14.md) records capture and
comparison settings. The original 64 MiB normal-profile results remain in
`research/causal-byte-adaptation`; they are historical, not the comparator here.

## Implemented algorithms

The chosen normal or max model always predicts and learns from the same prefix as
the new experts. The new layer combines its final bit probability with:

1. **Rates:** two probability calibration tables with update rates 1/32 and
   1/512. Each table has 16,384 hashed contexts and 33 probability bins.
   A context hashes the preceding raw byte and the current decoded bit prefix.
2. **Sparse:** two additional count predictors, using byte lags (1,3) and (2,4)
   together with the current bit prefix. Each has 16,384 hashed count pairs.
   Counts halve before increment when their sum reaches 512. The probability
   before quantization is `(n1 + 1/2)/(n0 + n1 + 1)`.
3. **Retained:** one additional calibrator drawn from four retained banks,
   each containing 2,048 contexts and 33 bins, updated at rate 1/64. After each
   completed 4,096-byte block, its byte histogram selects a bank for subsequent
   bits. Selection uses nearest L1 prototype, lowest-index tie breaking, and
   distance threshold 2,048. A novel descriptor uses a free bank or replaces
   the least recently selected bank. Prototypes remain fixed until replacement.
   Ages saturate rather than wrap.

Calibration bins are initialized lazily from the codec's algorithmically
generated squash table. They contain unsigned 16-bit probabilities. Learning
uses signed C99 division with truncation toward zero, avoiding dependence on
how a compiler shifts negative integers. Arithmetic coding consumes probabilities
clamped to 1…4095 on a denominator of 4096.

For each of the eight bit positions, expert weights begin uniformly and sum to
`W = 2^24`. With expert probability `p_i` and actual next bit `b`, the code uses:

\[
q = \operatorname{round}\left(\frac{\sum_i w_i p_i}{W}\right),\qquad
\ell_i = b p_i + (1-b)(4096-p_i),
\]

\[
v_i = \left\lfloor\frac{W w_i\ell_i}{\sum_j w_j\ell_j}\right\rfloor,
\qquad
w'_i = \left\lfloor\frac{65535v_i +
  \left\lfloor(W-v_i)/(K-1)\right\rfloor}{65536}\right\rfloor.
\]

After each rounding step its deficit is assigned to expert zero, the existing
base model. This preserves exact normalization and positive weights. Products
fit within unsigned 64-bit arithmetic. There are K=3, 5 or 6 experts.

This is a finite-precision implementation of the no-self-switch mixture with
switch probability 1/65536. The ideal real-arithmetic regret result in the
mathematical report does **not** automatically prove a bound for this rounded
implementation. Actual arithmetic-coded container length is the acceptance
criterion. Sparse count halving and hashed collisions are also deliberate
departures from an unbounded independent KT model.

Every expert sees a shared causal history. No latent routing path privately
trains its own state. Stored/skipped bytes update the retained-state histogram
on both encoder and decoder, while predictor learning remains skipped as the
block method specifies. Segment markers reset all predictor state. The routing
descriptor necessarily lags a regime change by a block, and a bank can receive
poorly matched data during that lag; no instant regime-detection claim is made.

All constants above were fixed before the benchmark results were inspected.
The fixtures generate inputs, not codec priors.

## Reversible BMP24 transform

Eligibility is a BMP header declaring uncompressed 24-bit pixels, one plane,
an information header of at least 40 bytes, valid positive width, nonzero
signed height, and a declared file size covering the pixel extent. Top-down
and bottom-up layouts both work. Width, absolute row count and pixel offset
are carried as three little-endian 32-bit values: **12 counted bytes**.
The row stride is `4 * ceil(3 * width / 4)`, matching the BMP layout described
in [Microsoft's BITMAPINFOHEADER documentation](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader).
The codec does not call a Windows image API.

For each complete BGR pixel inside a codec block:

\[
c_x=(B_x-G_x,\ G_x,\ R_x-G_x)\pmod{256},\qquad
y_x=c_x-c_{x-1}\pmod{256}.
\]

The predecessor is zero at the first complete pixel of each row/block
intersection. Decoding adds the predecessor back, then reconstructs
`B = c[0] + G` and `R = c[2] + G`, modulo 256. Each step has an explicit
inverse, so their composition is bijective. Headers, padding, trailing bytes,
and pixels split across codec blocks pass unchanged. This avoids a cross-block
pixel buffer and keeps the transform invertible under segmentation as well.

The original BMP header is preserved in the payload, including reserved fields.
The descriptor duplicates only the information needed to locate transformed
bytes during decoding. Original-byte CRC and optional round-trip verification
run after inversion. A transform cannot discard visually insignificant data.
This implements one format-specific candidate; it does not decompress PNG/JPEG
or rewrite PDF/EPUB structures.

## Container selection, memory and wire compatibility

`--adaptive auto` reads the source once into a temporary file and runs eligible
candidates sequentially. For ordinary byte streams and folders, the menu is
the base profile, rates, sparse and retained. `--max --adaptive auto` applies
all three expert configurations to the max predictor. For BMP24 input it also
includes those four profiles with the BMP transform. Fast remains a separate
profile outside this experimental menu.

Every candidate includes its header, optional period, transform descriptor,
filesystem metadata and CRC, block framing, segment table and trailer. The
smallest total wins, with the earlier candidate winning ties. The base
candidate uses the existing known/unknown-length sizing rule. For every
successful search:

\[
|C_{\mathrm{selected}}| = \min_{c\in\mathcal C_{\mathrm{eligible}}}|C_c|
\le |C_{\mathrm{base}}|.
\]

This guarantee concerns the enumerated menu and the current codec. It does not
assert the shortest computable description among every possible compressor.
Candidate failures due to an insufficient memory budget are reported and
skipped; allocation, source and destination I/O failures abort. Selection runs
no decode unless `--verify` is present. With that flag, it checks the winner
against the spooled source using one model before copying it to the destination.
This also supports verified pipe input/output at a one-model budget.

Only one encoder or decoder model is live during search. Additional adaptive
state is `sizeof(adaptive_model)` = **2,837,272 bytes** on the tested C99 builds,
the same allocation for all three modes so the first experiment does not mix
state-size tuning with expert comparisons. Memory sizing charges this allocation
before selecting table/window sizes. BMP reserves another 262,144 encoder bytes
for original-byte verification. Decoding transforms its block in place. Search
uses a 16 KiB copy buffer and stdio buffers within the existing 8 MiB headroom;
temporary disk holds the source plus at most two candidate containers. Existing
folder-entry bookkeeping remains additional to the codec memory budget.

| Predictor | Raw stream | With filesystem metadata |
|---|---:|---:|
| Normal / fast / max, unchanged | 19 / 20 / 21 | 22 / 23 / 24 |
| Rates / sparse / retained over normal | 25 / 26 / 27 | 28 / 29 / 30 |
| BMP + normal / rates / sparse / retained | 31 / 32 / 33 / 34 | 35 / 36 / 37 / 38 |
| Rates / sparse / retained over max | 39 / 40 / 41 | 42 / 43 / 44 |
| BMP + max / rates / sparse / retained | 45 / 46 / 47 / 48 | 49 / 50 / 51 / 52 |

BMP descriptors follow the optional two-byte record period and precede
filesystem metadata. Metadata versions include the extended-header checksum.
The maximum header grows from 42 to 54 bytes. Unsupported versions are rejected.
Old decoders cannot read new profiles, while the new decoder reads all listed
versions without an encoder flag or external data. `adapt_sizes` gains fields;
C library clients must rebuild and initialize it through the sizing functions
or initialize all fields to zero before assigning supported values.

## Running and reproducing the experiment

```sh
make -C adpat/c
adpat/c/build/cadapt encode input --max --adaptive auto --max-memory 706740224 --verify -o output.adpt
adpat/c/build/cadapt decode output.adpt -o restored
adpat/c/build/cadapt encode image.bmp --transform bmp --adaptive retained -o image.adpt
```

The ordinary telemetry collector accepts `rates`, `sparse`, `retained`, `auto`
and the four `*-bmp` profiles, understands all new headers and charges the BMP
descriptor to framing. For the full experiment, build the baseline revision
in a separate checkout, preserve its binary, then run:

```sh
python3 -B bench/adaptive_experiment.py \
  --baseline /path/to/baseline-cadapt \
  --baseline-revision 6eb6e41 \
  --output bench/results/causal-full --memory 706740224 --base-profile max
```

The runner freezes both binaries, hashes inputs and tools, and records GNU time
totals plus Linux `/proc` samples. Each encode is followed by a separate decode
with the candidate codec. It compares every restored byte and checks mode plus
nanosecond access/modification times before reading restored files. Each
candidate receives the same source metadata. The output directory must be new.
`--quick` explicitly substitutes a 65,536-byte enwik8 prefix; it is not a full
corpus result.

The default full matrix contains 108 encode/decode pairs: full enwik8; generated
Latin/diacritic, Cyrillic, CJK UTF-8 and CJK UTF-16LE inputs; CSV, XML, HTML, CSS,
source and binary bytes; random bytes; empty input; gradient/noise BMP; PNG,
PDF, EPUB; recurring regimes; and a mixed folder. The generated rows are
controlled probes and are not a representative enterprise corpus. Repeated
content across individual files, the regime fixture and the folder means a
byte-weighted aggregate must not be interpreted as an enterprise storage mix.

## Validation and limits

Validation covers old committed vectors, all new wire profiles, exact replay,
weight normalization, memory refusal, optional verification, whole-container
selection, stored and segmented paths, BMP padding and block boundaries, and
folder restoration. The coverage gate explicitly requires and counts the two
new headers; its floors remain 80% for the library and 60% for the CLI.

FMT-1 through FMT-6 and the byte-only modeling rule in FMT-8 govern the new
paths. The frozen baseline includes the FMT-7 archive implementation for ACLs,
extended attributes, hard links and sparse extents; its documented platform
rules still apply. This experiment does not establish measured generalization to every script or encoding from
the finite multilingual probes. Greater than 30% enterprise cost savings must
be assessed on a representative workload, including encoding cost, decoding
latency, temporary disk and memory. A gain on a synthetic image is not that
evidence.

## Current-source measurements and allocation

The [baseline report of 2026-09-14](baseline-2026-09-14.md) records the full
enwik8, controlled fixtures and natural holdouts against snapshot `6eb6e41`.
At matched max/1 GiB settings, all three added predictors slightly increase
enwik8 size, so automatic selection returns the baseline. The BMP transform
wins strongly on the synthetic gradient and loses on all three natural-photo
BMP probes. These outcomes limit the claims supported by the implementation.

A separate resource-allocation experiment found that at the default 674 MiB
cap, a 512 MiB context table with a 64 MiB window beats the original 256 MiB
table with a 128 MiB window. `--allocation context` in `dfe239e` exposes a
general table-first sizing rule, followed by window growth; it does not encode
corpus-specific sizes or data. The public `adapt_sizes_prioritize_context`
helper preserves a valid fitted profile, checks the full applicable memory
bound and leaves caller state unchanged on failure. It is applied to every
auto candidate when requested. The policy itself is not automatically searched
against window priority. The normal profile IDs and decoder remain sufficient.


For a fitted single model, let `T` and `W` be table/window base-2 logs, `M`
the fixed recall-index log, and `R` the other profile state and workspace.
Normal/max working memory has the form

\[
B(T,W)=16\,2^T+2^W+4\,2^M+R.
\]

The context policy chooses the lexicographically largest `(T, W)` under the
applicable budget, the length-derived table target, and the input/window caps,
starting at the already fitted table size. It maximizes table capacity first
and window capacity second. Verified pipes account for two models and their
queue; known segmented inputs account for every requested worker. The code
uses the existing exact bound functions for these cases. It never claims that
maximizing `(T, W)` minimizes coded length. On this enwik8 input the measured
container gain corresponds to **0.02136952 fewer bits per source byte**.
