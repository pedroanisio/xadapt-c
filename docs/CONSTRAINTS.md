<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14; the RUN codes, DLV-1, DLV-3 and DLV-4 and the FMT-6/FMT-8 wording 2026-09-15, with Claude Opus 5 (Claude Code), from the operator's decisions of that day. -->
# Constraints

What the codec is held to. Every constraint carries a code; cite the code
in commits, tests, reviews and design notes. Codes are `<category>-<n>`.

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
| `FMT-1` | Format   | **No external dependencies**        | Nothing outside the codec is needed to encode or decode: no library beyond the language's standard one, no external tool, no service. |
| `FMT-2` | Format   | **No shared knowledge**             | Nothing derived from data outside the input is assumed known to both sides in advance: no dictionary, no pretrained model, no table or transform learned from a corpus. A fixed rule of the format — the predictor and its hashes, the x86 branch transform — carries no data and is not shared knowledge. Everything the decoder needs is inside the container. |
| `FMT-3` | Format   | **The ultimate representation, each time** | Each encode is an absolute pursuit of the best representation of that input, and of that input alone, within the profile and budget the caller chose; nothing carried over from another input stands in for it. |
| `FMT-4` | Format   | **Universal input**                 | Any bytes are accepted: sorted files, text, documents, binaries, images, raw bits, CSV, PDF, EPUB, HTML, XML, CSS, code, and so on. |
| `FMT-5` | Format   | **One encode, one container**       | Encoding a file or a folder is a single operation, and its output is a single file/container. |
| `FMT-6` | Format   | **Decode from the container alone** | Decoding takes the single container that encode produced, and nothing else: by default a decoder allocates what the container's header declares and writes what it declares, and only a cap the caller sets on the command line (`--max-memory`, `--max-output`) may refuse a valid container. No cap applies unless given. |
| `FMT-7` | Format   | **Filesystem fidelity**             | Preserve timestamps, permissions, ACLs, extended attributes, symbolic/hard links, and sparse files with documented portability rules. |
| `FMT-8` | Format   | **Script and encoding agnostic**    | The model sees bytes, never characters: no text encoding is decoded, and nothing learned outside the input — no external dictionary, tokenizer or language model — is applied. A transform or dictionary the encoder builds from the input itself and carries in the container is allowed (it is `FMT-2`-clean), provided the bytes it does not match pass through unchanged. Every script and encoding is accepted and measured — the corpus holds Cyrillic, CJK and diacritic-heavy rows beside Latin. |
| `RUN-1` | Runtime  | **The tier is a decode promise**    | Encode's only memory setting is the tier (`-0` … `-11`); the file records the model it produced, and decoding needs that model's memory on every machine. A tier below the profile's smallest model is raised to it, never refused; `cadapt info` states what decoding a file needs. |
| `RUN-2` | Runtime  | **Streaming, both ways**            | Encode reads its input once and writes as it goes; decode reads the container once and writes each block as it is recovered. Working memory is the model, the window, one block's workspace and headroom — never the whole input or output — except where the caller asks for threads or a proof, which say what more they need. |
| `RUN-3` | Runtime  | **Nothing half-published**          | An output is staged beside its target and published by name only once complete and checked; an interrupted or failed run leaves no partial target and no staging leftovers. |
| `RUN-4` | Runtime  | **Refusals name their numbers**     | A refusal for memory, output size or budget says what was needed and what was allowed, before any output is written; exit codes are 0, 1 (codec or I/O failure) and 2 (usage). |
| `RUN-5` | Runtime  | **The same bytes on every machine** | The bytes a given profile, tier and input produce do not depend on the CPU, the instruction set chosen, the thread count or the platform; where two code paths exist they are held to the same output. |
| `DLV-1` | Delivery | **Every published version decodes forever** | A container format version, once on `main`, keeps its layout and its model: a file it wrote decodes as written by every later release. A change of layout or model is a new version number; unused numbers are refused. The encoder writes one live version per profile and variant. |
| `DLV-2` | Delivery | **Operational readiness**           | Expose metrics and diagnostic errors; provide reproducible builds, SBOMs, predictable licensing, security updates, and enterprise support. |
| `DLV-3` | Delivery | **General-purpose first**           | The codec is a general tool for enterprise use. enwik8 and enwik9 are benchmark rows with stated goals (17 MB; below 130 MB) — never the judge of a default, a policy or a merge. Defaults and levers are accepted on a mixed corpus (text, executables, images, records, source, already-compressed and small inputs); benchmark gains land as input-detected paths, as the x86 and BMP transforms do. |
| `DLV-4` | Delivery | **Provenance**                      | No code or material of incompatible licence or unclear intellectual-property provenance enters the tree: implementations are written from a technique's description, never from GPL or otherwise incompatible sources, and each landing records the origin of its idea (paper, book, post) so original work and prior art stay distinguishable. Test corpora are fetched by script, not vendored, unless their terms allow it. The tree is written to be audited and to support patent applications where applicable. |
| `SAF-1` | Safety   | **Fault containment**               | Detect corruption and truncation; use independently verifiable chunks, restart points, and explicit partial-recovery behavior. |
| `SAF-2` | Safety   | **Security**                        | Harden against malicious containers, decompression bombs, path traversal, and compression side channels; provide fuzzing evidence and independent audits. |
