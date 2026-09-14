# Self-contained compression: experiment specification

<!-- Authored with Codex / GPT-6, 2026-09-14. -->

This specification turns [the governing constraints](../docs/CONSTRAINTS.md)
into concrete, falsifiable experiments. The accompanying
[source audit](self-contained-source-audit.json) records the inspected source
hashes and evidence. This task has produced a design and source audit, not a
new compression implementation or measured improvement.

The research target is the smallest complete container found for the submitted
file or folder. Every reference must resolve inside that container or through
the fixed codec. Extraction must reproduce all original file bytes and the
directory structure. Encoding may inspect the entire input in multiple passes.

## What the live implementation already provides

The source audit identifies five relevant facts. These are source observations;
runtime acceptance of the concurrent folder implementation remains separate.

| ID | Observation | Consequence |
|---|---|---|
| F01 | Model configuration is selected from the first block; probabilities subsequently adapt. | A whole-input search can test whether a different configuration wins on the complete input. Configuration selection and probability adaptation must not be conflated. |
| F02 | The archive grammar has file, directory, and symbolic-link entries, with no explicit content-reference entry. | Explicit deduplication would require a new representation and matching decoder. |
| F03 | The CLI sorts directory names and compresses the serialized folder as one stream. | The baseline already shares adaptive state across files. Folder packing alone is not a new compression gain. |
| F04 | Executable-transform detection uses the first input block. | A mixed archive beginning with an archive header does not automatically receive useful transforms for each embedded executable. |
| F05 | The telemetry input preparation can extract a selected ZIP member. | An enwik8 run from a ZIP measures the extracted member. It cannot establish exact recompression of the ZIP container itself. |

The 64 MiB default, current profiles, streaming behavior, and lexical directory
ordering are implementation choices. `CONSTRAINTS.md` does not make them
requirements. Existing behavior remains a baseline against which changes are
measured, rather than an argument for preserving a particular architecture.

## Accounting and reconstruction contract

For every candidate, count the complete serialized container:

`C = framing + manifest + dictionaries/models + reconstruction recipes + coded residuals`

Shared structures are counted once within a container. Separate containers
each pay for the structures they need. A learned model is either serialized
in the container or deterministically rebuilt from the decoded history.
Anything needed to initialize that reconstruction must also be available.

Keep the codec fixed across evaluated inputs and record its executable size
and hash. An input-specific executable is not an acceptable way to move input
bytes outside the container. External model downloads, prior archives,
installed dictionaries, helper compressor executables, and network services
are unavailable to the delivered encoder/decoder. Research tools used to
prepare fixtures or evaluate baselines are not dependencies of the codec.

Treat file contents as bytes regardless of format validity. A specialized
transform may decline malformed or unsupported input; the general byte path
must remain available. Text encodings, line endings, padding, numeric spelling,
and unused format fields are part of the original bytes.

For folders, include names, relative paths, file lengths, and empty directories.
The current implementation also represents symbolic links; preserve and test
that existing behavior separately from regular-file restoration. Do not infer
unwritten requirements for permissions, ACLs, timestamps, random access, or
independent decoding of each member.

Any new bitstream requires an explicit format identity and a matching decoder.
Specify arithmetic precision, rounding, state updates, and initialization for
learned probability models; an unspecified dependency on hardware floating
point behavior defeats portable exact decoding.

## Experiment E1: establish the value of whole-input selection

**Question.** How much compressed size is lost by selecting a configuration
from the beginning of an input instead of evaluating complete candidates?

First compare complete outputs from the existing fast, normal, and max
profiles under equal resource budgets. Include several budgets to expose the
size/memory frontier, and separately report the unrestricted best candidate.
Use the complete input for every final comparison. A smaller sample is only
a search heuristic; it is not the measured result.

A subsequent experiment can expose the existing class/context-set choices
to controlled whole-input evaluation. Changing that internal selection path
requires implementation; the current CLI does not provide that control.

Use real mixed files and folders, plus synthetic controls with the same
regions in different orders. A large dependence on the first region is
evidence of an opportunity, not automatically a defect in a speed-oriented
baseline. Report existing stored-block fallbacks as part of every candidate.

**Decision.** Retain a selector only if it chooses the smallest complete
candidate actually evaluated, including selector metadata. Record the cost of
all trials. This experiment measures selection opportunity; it cannot establish
the value of an unimplemented new predictor.

## Experiment E2: expose structure with reversible transforms

**Question.** Which transformations improve the final container after their
inverse information is included?

Evaluate transformations independently before combining them:

- Fixed-width record transposition and numerical differencing for suitable
  byte regions, including a literal remainder for incomplete records.
- CSV field separation, including a complete account of quoting, delimiters,
  spelling, and line endings. Parsing and ordinary reserialization are not
  exact inverses.
- Repeated lexical fragments for text, markup, CSS, and source, preserving
  whitespace, case, comments, and arbitrary byte escapes.
- Region-specific executable transforms, with offsets and reconstruction
  rules represented explicitly.

Each transformed candidate competes with the untransformed baseline under
the same accounting. Measure the transformed payload and inverse metadata
separately for diagnosis, but select using their complete final encoding.

**Decision.** A transformation is useful only where the complete result is
smaller and exact reconstruction succeeds. Generality comes from automatic
selection and a total byte-level fallback, not from assuming every format
benefits from one transformation.

## Experiment E3: recover redundancy inside compressed formats

**Question.** Can supported inner streams be recoded more efficiently while
preserving the exact original compressed bytes?

Begin with a narrowly specified family of DEFLATE streams and JPEG files.
For DEFLATE, reconstruction can require original block boundaries, coding
tables, match decisions, unused bits, and wrapper information. For JPEG,
preserve the information needed to reconstruct the original coding and
metadata, not merely the decoded pixels.

PDF, PNG, EPUB, ZIP-based documents, and other containers need exact outer
byte preservation as well. Unsupported or encrypted regions remain opaque
byte regions. Recursive inspection must have deterministic resource and
termination bounds, even if maximum compression profiles permit substantial
encoder work.

The [Lepton paper](https://arxiv.org/abs/1704.06192) establishes exact JPEG
recompression as a practical mechanism. [Precomp's documentation](https://github.com/schnaader/precomp-cpp/blob/master/readme.txt)
describes exposing supported compressed streams with bit-identical
reconstruction. These are mechanism references, not proposed runtime tools
or evidence that their published gains transfer to this codec.

**Decision.** Retain the recoding only if its payload plus all reconstruction
information beats coding the original bytes. Compare original archive bytes
to reconstructed archive bytes. Comparing extracted members alone is invalid.

## Experiment E4: learn explicit structure within one input

**Question.** Can a dictionary or grammar capture dependencies that the
existing adaptive predictor misses, after paying for the learned structure?

Learn repeated byte fragments or bounded grammar rules from the entire input.
Serialize the required rules and encode references, literals, and exceptions.
Evaluate alternative dictionary sizes and pruning thresholds by actual total
encoded size. A rule's raw frequency is not its net benefit: it changes the
downstream symbol distribution and may compete with another rule.

Compare learned representations against the same untransformed baseline and
against the E1 selector. Separate gains on repetitive controls from gains on
real held-out files. Do not assume fewer intermediate bytes yield a smaller
arithmetic-coded container.

**Decision.** Retain rules only when the complete representation wins. This
experiment establishes a model-cost discipline before adding a more expensive
neural model.

## Experiment E5: compact input-specific prediction

**Question.** Does input-specific training capture additional dependencies
economically enough to improve the complete container?

Compare three approaches: synchronized online adaptation; a stored model
trained on the complete input; and a stored bootstrap model followed by
synchronized adaptation. The [DZip method](https://arxiv.org/html/1911.03572v2)
provides a directly relevant example of the third approach without additional
training data. Its implementation dependencies are not part of this design.

Measure quantized or otherwise compressed model storage together with coded
data. Test increasing model capacity and select on actual container size.
Publish encoder and decoder memory and compute separately. If the model
supplies entropy-decoding probabilities, the decoder must execute it. A
model used only to discover an explicit reconstruction recipe need not be
present at decode time once that recipe is sufficient.

Training on the complete submitted input is allowed. Evaluate the codec's
generalization on previously unused producers and data collections, while
allowing its unchanged encoding procedure to adapt to each new input.

**Decision.** Reject a model whose stronger predictions do not repay its
serialized cost. A favorable model-excluded payload is not acceptance.

## Experiment E6: joint folder optimization

**Question.** How much extra redundancy can be removed beyond the existing
shared-stream archive baseline?

Compare exact duplicate references, bounded delta references between similar
content, grouping by statistical similarity, and shared learned structures.
Every dependency stays within the container. Select an internal decoding
order and represent the mapping back to original paths.

Measure each mechanism independently before combinations. A repeated file
already benefits from the baseline predictor's recall and contexts; explicit
deduplication must improve the final bytes beyond that. Small-file grouping
must include manifest and group-membership costs. Maintain a straightforward
whole-folder restore path and record its resource use.

**Decision.** Accept only improvements over the current complete folder
container, with identical reconstructed file and directory inventories.

## Corpus and validation design

Use two distinct sets: synthetic controls that isolate mechanisms, and real
inputs reserved for evaluating generalization. Keep related versions and
near-duplicate producers together when assigning development and evaluation
sets. Enwik8 is one text/XML fixture, not the development objective.

Coverage must include text, documents, source, HTML/XML/CSS, CSV, executables,
raw arrays/images, JPEG/PNG, PDF, EPUB, compressed containers, arbitrary bits,
empty inputs, and mixed folders. Include tiny, medium, and large examples:
model overhead behaves differently at each scale. Record incomplete format
coverage explicitly; no current corpus manifest exists for this specification.

For each evaluated candidate, retain:

- Input identity, byte count, hashes, and folder inventory where applicable.
- Codec source/build identity where available, executable hash, configuration,
  and all transform/model choices needed to understand the measurement.
- Complete container byte count and component accounting that sums to it.
- Exact decode result, original/reconstructed hashes, and inventory comparison.
- Encoding cost including all search/training trials, decoding cost, and peak
  memory. Distinguish cold and warmed model behavior if it affects results.
- Failures, timeouts, resource exhaustion, and unsupported transforms; none
  disappear from aggregate reporting.

For standalone acceptance, copy only the decoder and container to a clean
environment and extract with no network or original input accessible. Compare
the extracted result against the original outside that environment. Folder
tests include empty directories, empty files, duplicate names in different
directories, and the existing symbolic-link behavior. Byte tests include
malformed supported formats and arbitrary data.

Encoding verification remains the user's existing optional CLI choice.
Research acceptance still requires an independently checked exact round trip;
this specification does not change the CLI's verification default.

## Baselines, success, and interpretation

Use a freshly pinned current codec baseline and suitable strong standalone
competitors. Preserve the older enwik8 measurements in the existing review as
historical evidence tied to their saved binary; do not present them as the
current working-tree result. Compare equal budgets and best-size configurations
separately so a gain purchased with more resources stays visible.

The direct size metric is `1 - candidate_container_bytes / baseline_container_bytes`.
More than 30% reduction requires a ratio below 0.70; a twofold reduction requires a
ratio at or below 0.50. Both refer to complete containers. Report per-input and
per-format results as well as a byte-weighted aggregate, with the weighting
specified in advance. An aggregate cannot imply a guarantee for every input.

Successive reductions multiply on the remaining bytes. Improvements from
overlapping mechanisms cannot be added. Fewer stored bytes support the broader
enterprise-cost objective but do not independently establish a proportional
billing reduction.

The practical research priority is E1 as a measured control, E2/E3 to expose
hidden structure, then E4/E5 to model it, with E6 measuring the additional
opportunity available for folders. An experiment that fails its size test is
still a useful result and must not be promoted solely because its intermediate
representation or predictor appears more sophisticated.

The model-plus-data accounting follows the
[minimum description length principle](https://arxiv.org/abs/math/0406077).
The particular experiment sequence is a design recommendation, not a result
proved by the cited literature. No new compression percentage is claimed by
this specification.
