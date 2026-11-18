# Changelog

Release changes, compatibility breaks and security fixes. Installation and
migration instructions are in the [release notes](docs/release-notes/0.12.0.md).

## [Unreleased]

## [0.13.0] — 2026-11-18

The decoder supports container versions 19 to 91, excluding 55, 59, 60, 73
and 74. Files written by 0.12.0 decode unchanged. A max-profile file written
by this release carries version 86 to 91 and requires this release or a later
one to decode. Rebuild C callers against the matching header: `adapt_sizes`
and `adapt_version_info` gained the field `precise`.

### Added

- Container versions 86 to 88 (89 to 91 with metadata): the max-profile models
  of versions 72, 80 and 81, whose modeled blocks can pass the final probability
  to the arithmetic coder in 16 bits instead of 12. The encoder codes each block
  both ways in one pass and keeps the shorter payload, and it chooses the model
  as 0.12.0 does, so a max container is never larger than the one 0.12.0 writes
  for the same input and options. Every max encode that is neither adaptive nor
  BMP writes these versions. `cadapt info` reports the coder.
- `--threads n` with n of at least two runs the independent first-block model
  trials concurrently, within the memory already granted to the workers. The
  container bytes equal those of the sequential trials.

### Changed

- Encoding and decoding under the normal, mid and max profiles hash each
  byte's contexts once: the hashes computed for the prefetch are reused when
  the byte starts. The decoder prefetches a single candidate where recall
  names the byte. Containers of versions 19 to 85 are byte for byte unchanged.

### Fixed

- Release packaging validates the source inventory and installed payload,
  checks revision identity, and assembles platform-specific manifests with
  hashes for the published files.
- Source packages include the maintained validation tools and fixtures. Checks
  discover test suites, and source-export validation uses the CI timeout budget.
- Documentation states current memory requirements, exact supported format
  ranges, segmentation behavior and staging limits; CLI help removes obsolete
  performance comparisons.

## [0.12.0] — 2026-09-18

The decoder supports container versions 19 to 85, excluding 55, 59, 60, 73
and 74. Versions 13–15 from 0.11.0 require that release to decode. Rebuild C
callers against the matching header: public sizing/statistics structs gained
fields. See [upgrading](docs/release-notes/0.12.0.md#upgrading-from-0110).

### Added

- Mid profile; additional max and normal predictor policies, causal text
  transformation, optional adaptive predictors and reversible BMP24 transformation.
- Memory tiers -0 through -11, automatic/context-first/window-first allocation,
  lean context selection and model-selection diagnostics through --explain.
- Folder containers with modes, timestamps, links, sparse extents, extended
  attributes and supported ACLs. Archive version 4 includes root attributes;
  versions 1–3 remain readable.
- Segmented parallel encoding/decoding, optional round-trip proof, decoder
  memory/output caps, typed API errors and executable C API examples.
- Install/uninstall targets, source and binary packages, SPDX SBOMs, build
  reproduction and release manifests; Linux/macOS CI and an informational
  Windows build. Sanitizer, fuzz, branch-coverage, mixed-corpus and memory gates.

### Changed

- Encoder memory is selected by tier; --max-memory is a decoder-only cap.
- Round-trip proof is opt-in with --verify; --no-verify remains accepted.
- Container model/version dispatch is defined by ADAPT_VERSIONS in adapt.h.
  Sized max policies use explicit version and sizing fields. Small-table
  58/61 model2 containers decode through checksum-verified predictor fallback.
- Normal and max model selection, table sizing and prediction loops improve
  compression and throughput on the reference corpus. The release notes retain
  the dated comparison and regression baselines; results vary by input.

### Fixed

- Restore macOS timestamps before 1970 and symbolic-link modes correctly.
- Saturate threaded memory-bound calculations so overflowing worker counts
  cannot bypass resource limits.
- Report output I/O failures against the output path. Handle failed attribute
  allocation and invalid packer/version-table states without NULL dereferences.
- Restore Linux read-only ACLs after other attributes, including folder roots.
- Decode valid empty segmented streams while rejecting malformed tables and
  checksums; preserve fractional max sizing through fast-profile conversion.
- Restore folders under restrictive umasks and preserve read-only root modes.
- Reject files changed during folder packing and inaccessible folder entries.
- Avoid duplicate concurrent builds in the check target; build complete
  exported codec sources for benchmark packaging.
- Index hard links by device/inode and write regular-file blocks without
  pipe-sized splitting. Pipe cancellation checks remain bounded.
- Avoid NULL pointer arithmetic on empty inputs and release test allocations.

### Security

- Preserve existing targets when forced folder publication fails. Use atomic
  rename or exchange instead of deleting the destination first; refuse
  unsupported replacement and retain previous entries if cleanup fails.
- Put POSIX file outputs inside private staging wrappers and retain their
  file and directory handles through metadata, publication and cleanup.
  Replacing a staging name cannot publish an unrelated file, change its
  metadata or remove the replacement during rollback.
- Journal deferred directory and root metadata to temporary disk instead
  of retaining every attribute in RAM. Replay after checksum validation
  keeps memory bounded, preserves ACL/umask ordering and fails closed on
  temporary-storage errors.

- Anchor POSIX folder restoration, metadata, cleanup and publication to
  retained directory handles inside a private wrapper. Replacing the staging
  pathname cannot redirect writes or delete unrelated files during rollback.
- Remove inherited Linux access/default ACLs from folder roots before creating
  children, and from staged files before applying final permissions. Preserve
  archived ACLs without adding permissions inherited from the destination.
- Publish folders without `--force` using atomic no-replace operations, so
  a destination created during decoding is preserved. Unsupported platforms
  refuse the operation instead of using a racy existence check.
- Clean up staged file and folder outputs on POSIX `SIGINT`, `SIGTERM`,
  `SIGHUP`, `SIGPIPE` and `SIGXFSZ`, including blocked pipe I/O and file-size
  limits during encoding, decoding and sparse-file creation. Signal handlers only
  notify the command thread; cleanup runs through the ordinary failure path.
- Store CPU dispatch selection in each model, removing concurrent writes to
  shared dispatch state. Add model-isolation and concurrent prediction tests.
- Add CLI regression coverage for explicit text transforms with fractional
  context tables, already supported by the sizing-byte text flag.
- Keep POSIX output staging files at mode `0600` during processing and
  verification; apply recorded or umask-derived permissions only after success.
- Return worker failures safely when threaded codec callers pass `error=NULL`.
- Preserve folder-root extended attributes and Linux access/default ACLs in
  `ADPTARC4`. Restore root attributes after children; reject restoration failures
  and SDK readers without a root-attribute callback. Existing archive versions
  remain readable; older decoders reject the new archive magic.
- Enforce `decode --max-output` on the combined logical sizes of restored
  files, including sparse holes and each hard-link alias. Refuse an entry
  before creating or extending it when the remaining budget is too small,
  and discard the staging tree on refusal.
- Restore modeled-path coverage in the decoder fuzzer: allow a 128 MiB
  model working-memory bound and reserve 32 KiB separately for output.
  Regression tests run the real harness on modeled normal, fast and max
  fixtures; `make -C adpat/c fuzz-smoke` runs mutations on build-local copies
  of the committed vectors.
- Refuse archive entries whose parent components are symbolic links, and
  hard-link targets outside the extraction root or of a non-file kind.
  POSIX extraction resolves parents through directory descriptors with
  symlink following disabled and keeps the staging root private. A failed
  checksum can no longer leave files outside the extraction directory.
- Enforce the decoder's memory budget before buffering a segmented input.
  Inputs that cannot fit alongside a decoder, and pipes, use streaming
  decoding. The library counts retained input when it falls back to one
  worker as well as when it decodes in parallel.

## [0.11.0] — 2026-09-14

Container format versions 13 (normal), 14 (fast) and 15 (max): an x86
branch transform before the model for machine code, four blocks to a table
line instead of two, a context table twice the size for the same input, and
a max profile that adds contexts, run memories and per-context evidence.
Versions 10–12 are refused. Encoding telemetry (`bench/`) and the enwik8
corpus fixture.

## [0.10.0] — 2026-09-12 (untagged)

The C codec as a standalone repository: the port of the Rust crate's
container format (versions 10–12) and predictor, the `cadapt` command line,
the C test suites on committed vectors, and the gcov coverage gate.
