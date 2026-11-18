# Engineering contracts and design rationale

Part of [adapt](README.md). These are the current compatibility and acceptance
contracts. [CONSTRAINTS.md](docs/CONSTRAINTS.md) defines the product requirements;
[adapt.h](adpat/c/adapt.h) defines the wire layout and API.

## Compatibility and deterministic encoding

The supported container versions listed in [SUPPORT.md](SUPPORT.md) remain
decodable; 0.11.0 files require the documented migration. Changes to predictor
arithmetic or layout require an unambiguous version or policy field. Encoder
selection may change while old decoding semantics remain fixed. Unused version
numbers are refused. Committed vectors pin published formats.

Versions 58/61 can identify two historical predictors for small tables. The
decoder tries them sequentially and checks the recovered checksum before
publishing bytes. One model is live at a time; tentative output uses temporary
storage. Versions 72/75 distinguish the current predictor without this fallback.
Versions 86–91 are the models of 72, 80 and 81 with the final probability
coded in 16 bits; versions 19–85 keep the 12-bit coder they were written with.

Integer rounding, update order, context hashes and policy bits are format
semantics. The baseline and accelerated instruction paths, compilers and
worker schedules must agree byte for byte for identical requested segmentation. No external learned dictionary or
language model is required to decode.

## Model selection and allocation

The normal profile uses keyed arbiters only when first-block comparison selects
them; its policy byte carries key selection and the text-transform flag.
The max profile records model and sizing policy in its version and sizing byte.
The mid profile uses seven contexts and one arbiter as a separate format family.
The lean context set omits expensive contexts explicitly; automatic selection
compares first-block coded size instead of inferring benefit from input names.

Automatic allocation grows the context table first when window-first sizing
leaves the table below its target for the input unit. A threaded segmented
input uses a segment as the sizing unit. Context-first and window-first remain
explicit choices. Unused budget can grow recall without changing its update
semantics. Compact side tables and fractional table sizing are recorded in
format policy, never inferred from host memory.

The text transform constructs its dictionary causally from the input. Encoder
and decoder see the same observations; no dictionary prelude or external data
is required. Transform and allocation flags must compose only as the format
defines them. BMP inversion preserves headers, padding and original-byte checksums.

## Memory and publication

Encoder memory is selected by tiers -0 through -11. A tier below the minimum
supported model is raised to that minimum. Decoder allocation comes from the
container; --max-memory and --max-output apply only when explicitly supplied.
Peak resident memory is held to the declared model requirement plus 16 MiB of
process headroom by the memory gate. Optional search/proof work documents its
extra memory and temporary-disk requirements. First-block model trials keep one
model live at a time; a threaded encode runs independent trials concurrently
within the memory its workers were granted, and writes the same bytes.

Named outputs are staged and published after validation. Stream callbacks and
standard output may receive a prefix before a later failure. Filesystem
metadata is replayed with bounded memory after syntax and checksum validation;
access ACLs are applied after other attributes. See [ARCHIVE.md](adpat/c/ARCHIVE.md).

## Regression acceptance

`make check` is held to 120 s of wall time by default. CHECK_SECONDS can be
raised for a slower validation host without weakening test assertions.
Library and CLI branch coverage floors are 80% and 60%, respectively.

Size floors never allow a larger container. The 10 % tolerance stays in force
for canonical CPU-second throughput floors. The corpus floor files stay in wall
seconds at their 25 % tolerance. Measurements use verified round trips;
canonical timing uses the median of repeated runs. A shared host compares the
candidate and baseline binaries in alternating runs; absolute timing requires
the reference host. Changes to floor values require measured justification.

The canonical comparison command is encode --max --allocation context
--contexts lean. Defaults are also checked on a mixed corpus and at the -6
max tier. A single benchmark does not establish a default's suitability.

General benchmark orchestration is provided separately by codecbench.
The maintained measurement tools and their tests in bench/ remain part of
this repository's engineering validation.
