# Constraints

These requirements define the supported product behavior. Stable codes connect
implementation, tests and compatibility decisions.

| Prefix | Category | Governs |
| ------ | -------- | ------- |
| `FMT`  | Format   | What a container is, what it needs, what it holds, and what the model may know. |
| `RUN`  | Runtime  | How encode and decode behave: budgets, streaming, bounds, access, fallback. |
| `SAF`  | Safety   | What happens on corrupt, truncated or hostile input. |
| `DLV`  | Delivery | What surrounds the codec: format versioning, SDK, builds, metrics, support. |

`FMT-1` to `FMT-6`, `RUN-1` and `DLV-1` are invariants — a change that
breaks one is wrong whatever it measures. The rest are required
capabilities. enwik8 and enwik9 are the benchmark, not the product: no
constraint is satisfied by a measurement on them alone (`DLV-3`).

| Code    | Category | Constraint                          | Required behavior |
| ------- | -------- | ----------------------------------- | ----------------- |
| `FMT-1` | Format | **No external codec dependency** | Encoding and decoding require no external compression library, tool, model or service. The implementation uses C99 and the standard library; optional threaded builds use POSIX threads. |
| `FMT-2` | Format   | **No shared knowledge**             | Nothing derived from data outside the input is assumed known to both sides in advance: no dictionary, no pretrained model, no table or transform learned from a corpus. A fixed rule of the format — the predictor and its hashes, the x86 branch transform — carries no data and is not shared knowledge. Everything the decoder needs is inside the container. |
| `FMT-3` | Format | **Input-specific modeling** | Prediction and transform dictionaries derive from the current input. Profiles and first-block selection use documented policies; `--adaptive auto` chooses the smallest complete output among its successful, budget-eligible candidates. No global optimum is promised. |
| `FMT-4` | Format   | **Universal input**                 | Any bytes are accepted: sorted files, text, documents, binaries, images, raw bits, CSV, PDF, EPUB, HTML, XML, CSS, code, and so on. |
| `FMT-5` | Format   | **One encode, one container**       | Encoding a file or a folder is a single operation, and its output is a single file/container. |
| `FMT-6` | Format | **Self-contained decoding** | The container declares its format, model and output length. Decode needs no external model. CLI memory and output caps are opt-in; SDK callers supply explicit limits. Malformed or unsupported containers are rejected independently of those caps. |
| `FMT-7` | Format   | **Filesystem fidelity**             | Preserve timestamps, permissions, ACLs, extended attributes, symbolic/hard links, and sparse files with documented portability rules. |
| `FMT-8` | Format | **Encoding-independent input** | The model operates on bytes without requiring a character encoding, tokenizer or external dictionary. Reversible transforms may construct dictionaries from the input itself. Multilingual corpus rows exercise several scripts; they do not establish performance for every language. |
| `RUN-1` | Runtime | **Memory tiers** | Encode selects a requested budget with -0 through -11 and raises requests below the profile minimum. The container records the selected model, whose decoder requirement may be lower than the requested budget; `cadapt info` reports it. |
| `RUN-2` | Runtime | **Bounded streaming** | Ordinary sequential encode/decode uses model, window and block workspace rather than buffering the whole input. Candidate search spools input and candidates to disk. Verification, segmentation, folder enumeration and metadata handling have the additional resource costs documented in the manual and archive reference. |
| `RUN-3` | Runtime | **Staged named outputs** | Named outputs are staged before publication. Catchable failures attempt cleanup and preserve existing targets until replacement succeeds. Uncatchable termination and cleanup failures can leave staging; a post-publication failure is reported explicitly. Standard output and library callbacks may receive a prefix before final validation. |
| `RUN-4` | Runtime | **Actionable failures** | Memory and output refusals report needed and allowed values. Header-declared limits are checked before model/result allocation; stream and archive limits are checked as data becomes available. Exit codes are 0 for success, 1 for codec or I/O failure, 2 for usage, and 128 plus the signal number for handled POSIX interruption. |
| `RUN-5` | Runtime | **Deterministic encoding** | Within a release, identical bytes, metadata, profile, tier and requested segmentation/settings produce identical containers across supported CPU paths and compilers. Worker scheduling does not change bytes. Changing requested thread count or segment size can change the model and framing. |
| `DLV-1` | Delivery | **Versioned compatibility** | Preserve decoding semantics for the current supported container set in `ADAPT_VERSIONS` and archive versions 1–4. Versions 13–15 require cadapt 0.11.0 as documented in the upgrade guide. Layout or predictor changes require a distinct version or policy field; unknown numbers are refused. |
| `DLV-2` | Delivery | **Operational support** | Provide typed diagnostics, release provenance, SBOMs and reproducible-build checks for the recorded toolchain. Licensing, security maintenance and community/commercial support follow LICENSE, SECURITY.md and SUPPORT.md. |
| `DLV-3` | Delivery | **General-purpose first** | Defaults and policy changes are evaluated on a mixed corpus covering text, executables, images, records, source, already-compressed data and small inputs. A gain on one benchmark does not establish suitability for a default. Input-detected optimizations must preserve all format and resource contracts. |
| `DLV-4` | Delivery | **Provenance** | Retain license and source attribution for code, algorithm references and permitted fixtures. Release manifests identify source revisions, toolchains and artifact hashes. Fetch external corpora through pinned manifests and preserve their usage terms. |
| `SAF-1` | Safety | **Corruption detection** | Validate framing, lengths, archive syntax and checksums. Report truncation or corruption without treating a partial result as successful. Segment restart boundaries do not provide a general damaged-archive recovery interface. |
| `SAF-2` | Safety | **Hostile-input handling** | Validate sizes and paths, enforce requested resource caps, and exercise rejection paths with regression tests, sanitizers and fuzzing. Authentication, encryption, constant-time operation and independent security certification are not provided; see SECURITY.md. |
