<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14. -->
# Constraints

What the codec is held to. Every constraint carries a code; cite the code
in commits, tests, reviews and design notes. Codes are `<category>-<n>`.

| Prefix | Category | Governs |
| ------ | -------- | ------- |
| `FMT`  | Format   | What a container is, what it needs, what it holds, and what the model may know. |
| `RUN`  | Runtime  | How encode and decode behave: budgets, streaming, bounds, access, fallback. |
| `SAF`  | Safety   | What happens on corrupt, truncated or hostile input. |
| `DLV`  | Delivery | What surrounds the codec: format versioning, SDK, builds, metrics, support. |

`FMT-1` to `FMT-6` are invariants — a change that breaks one is wrong
whatever it measures. The rest are required capabilities.

| Code    | Category | Constraint                          | Required behavior |
| ------- | -------- | ----------------------------------- | ----------------- |
| `FMT-1` | Format   | **No external dependencies**        | Nothing outside the codec is needed to encode or decode: no library beyond the language's standard one, no external tool, no service. |
| `FMT-2` | Format   | **No shared knowledge**             | Nothing derived from data outside the input is assumed known to both sides in advance: no dictionary, no pretrained model, no table or transform learned from a corpus. A fixed rule of the format — the predictor and its hashes, the x86 branch transform — carries no data and is not shared knowledge. Everything the decoder needs is inside the container. |
| `FMT-3` | Format   | **The ultimate representation, each time** | Each encode is an absolute pursuit of the best representation of that input, and of that input alone, within the profile and budget the caller chose; nothing carried over from another input stands in for it. |
| `FMT-4` | Format   | **Universal input**                 | Any bytes are accepted: sorted files, text, documents, binaries, images, raw bits, CSV, PDF, EPUB, HTML, XML, CSS, code, and so on. |
| `FMT-5` | Format   | **One encode, one container**       | Encoding a file or a folder is a single operation, and its output is a single file/container. |
| `FMT-6` | Format   | **Decode from the container alone** | Decoding takes the single container that encode produced, and nothing else: by default a decoder allocates what the container's header declares, and only a limit the caller sets on the command line may refuse a valid container. |
| `FMT-7` | Format   | **Filesystem fidelity**             | Preserve timestamps, permissions, ACLs, extended attributes, symbolic/hard links, and sparse files with documented portability rules. |
| `FMT-8` | Format   | **Script and encoding agnostic**    | The model sees bytes, never characters: no text encoding is decoded, no tokenizer, no dictionary, no language model. Every script and encoding is accepted and measured — the corpus holds Cyrillic, CJK and diacritic-heavy rows beside Latin. |
| `DLV-2` | Delivery | **Operational readiness**           | Expose metrics and diagnostic errors; provide reproducible builds, SBOMs, predictable licensing, security updates, and enterprise support. |
| `SAF-1` | Safety   | **Fault containment**               | Detect corruption and truncation; use independently verifiable chunks, restart points, and explicit partial-recovery behavior. |
| `SAF-2` | Safety   | **Security**                        | Harden against malicious containers, decompression bombs, path traversal, and compression side channels; provide fuzzing evidence and independent audits. |