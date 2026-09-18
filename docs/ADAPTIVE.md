# Adaptive predictors and BMP transformation

Part of [adapt](../README.md). The adaptive predictors are optional codec
profiles. Their state is reconstructed from the decoded prefix; they require
no external model or sidecar. [MODEL.md](MODEL.md) states the synchronization
and complete-container accounting invariants.

## Implemented algorithms

The chosen normal or max model always predicts and learns from the same prefix as
the adaptive experts. The mixture combines its final bit probability with:

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
switch probability 1/65536. The ideal real-arithmetic regret result does **not** automatically prove a bound
for this rounded
implementation. Actual arithmetic-coded container length is the acceptance
criterion. Sparse count halving and hashed collisions are also deliberate
departures from an unbounded independent KT model.

Every expert sees a shared causal history. No latent routing path privately
trains its own state. Stored/skipped bytes update the retained-state histogram
on both encoder and decoder, while predictor learning remains skipped as the
block method specifies. Segment markers reset all predictor state. The routing
descriptor necessarily lags a regime change by a block, and a bank can receive
poorly matched data during that lag.

## Reversible BMP24 transform

Eligibility is a BMP header declaring uncompressed 24-bit pixels, one plane,
an information header of at least 40 bytes, valid positive width, nonzero
signed height, and a declared file size covering the pixel extent. Top-down
and bottom-up layouts both work. Pixel offset, width and absolute row count
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

## Selection and resource accounting

`--adaptive auto` spools the source once and evaluates eligible candidates
sequentially. It selects the smallest complete container, including metadata,
framing and checksums, with the earlier candidate winning ties. This compares
the supported candidate menu, not every possible representation. `--verify`
checks the winning candidate before publication.

Adaptive state is charged to model memory before table and window sizing.
Only one candidate model is live at a time. Temporary disk holds the input
and candidate containers. The BMP descriptor uses three little-endian
32-bit fields for pixel offset, width and row count. Original-byte checksums
are checked after inverse transformation.

The supported format-version table is defined in [adapt.h](../adpat/c/adapt.h)
and described in the [format reference](../README.md#format). The compatibility commitment is defined in [SUPPORT.md](../SUPPORT.md). C clients initialize all sizing fields through the sizing
functions or zero-initialize the complete structure before assigning fields.

## Usage

```sh
make -C adpat/c
adpat/c/build/cadapt encode input --max --adaptive auto -9 --verify -o output.adpt
adpat/c/build/cadapt decode output.adpt -o restored
adpat/c/build/cadapt encode image.bmp --transform bmp --adaptive retained -o image.adpt
```

Encoder memory is selected with a tier. `--max-memory` is a decoder limit.
