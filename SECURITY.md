# Security

cadapt treats encoded containers and archive entries as untrusted input.
Resource limits and staging protect callers from malformed data; successful
decoding does not establish the identity or trustworthiness of its author.

## Implemented protections

- The in-memory decoder validates framing before allocating the model and
  result. The streaming decoder validates the header first, then validates
  blocks as they arrive. Failures return typed `adapt_status` values and
  diagnostic details through `adapt_error_message()`.
- `--max-memory` bounds decoder working memory and `--max-output` bounds decoded
  bytes. Both are opt-in. Archive output limits also count logical sparse-file
  sizes and each hard-link alias, checked before creating or extending the
  corresponding entry. Use limits appropriate to the consumer's resources.
- Archive paths reject empty, dot, parent, NUL and absolute components. POSIX
  restoration resolves parents through retained directory handles without
  following symlinks. Restored symlinks cannot be parents of other entries;
  hard links must name regular files within the archive root. Windows also
  rejects native drive, backslash and alternate-stream path syntax.
- POSIX named outputs use private staging wrappers and retained file/directory
  handles for restoration, metadata, publication and rollback. Replacing a
  visible staging pathname cannot redirect those operations. Publication
  preserves an existing destination until the replacement operation succeeds.
- Deferred directory and root metadata use a disk journal with bounded working
  memory. Replay follows checksum and archive validation. The
  [restoration reference](adpat/c/ARCHIVE.md) describes ACL ordering and failure
  cases, including retained entries after a cleanup failure.
- Threaded decoding accounts for retained compressed input alongside the model.
  Pipes and files exceeding that buffer budget use streaming decoding.

C and CLI suites, sanitizer targets and [fuzz harnesses](adpat/c/fuzz/) exercise
these protections. Harness regression tests cover modeled normal, fast and max
inputs. The [pipeline](docs/release-pipeline.md) states where checks run; it is
not evidence that a particular revision has passed them.

## Limits of protection

Checksums detect corruption; they provide neither authentication nor encryption.
The codec makes no constant-time or compression-side-channel guarantee. Avoid
using compressed sizes as a confidentiality boundary.

A stream callback or standard output can receive a prefix before a later error.
Discard its result unless decoding completes successfully. Named outputs are
staged, but uncatchable termination, storage failure or a failed cleanup can
leave temporary entries. Memory caps cover codec allocations, not every process
or filesystem resource; search and metadata journals also require temporary disk.
The encoder reads the source paths selected by its caller and is not a sandbox
for those paths or command arguments.

## Supported versions

Security fixes target the latest release; older releases have no maintenance
commitment. Read [SUPPORT.md](SUPPORT.md) before upgrading: 0.11.0 containers
require conversion with that release, while the current supported format set
has a forward compatibility commitment.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Write to
**pedroanisio@faz.ai** with the subject `cadapt security`, and include:

- the version (`cadapt --version`) and platform;
- the smallest input that shows the problem — a container, a folder, a
  command line — and what happened (crash, hang, wrong bytes, a path written
  where it should not be, memory beyond the budget);
- whether you want credit, and under which name.

You will get an acknowledgement within **7 days** and an assessment within
**30 days**. A confirmed vulnerability is fixed in the next release, with a
regression test and a `CHANGELOG.md` entry that credits the reporter unless
they ask otherwise. We ask reporters to keep the details private until the
fix is released or **90 days** have passed, whichever comes first;
coordinated disclosure with the reporter is the default.

Security fixes are announced in [CHANGELOG.md](CHANGELOG.md) under
**Security**, and the release's build manifest and
[SBOM](sbom.spdx.json) are regenerated with it.
