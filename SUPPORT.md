# Support

cadapt supports files, streams and folder archives through its command line and
C library. Community support is best effort; commercial support is available by
arrangement.

## Platforms and versions

Linux and macOS are supported with the toolchains described in the
[release pipeline](docs/release-pipeline.md). CI is configured for Linux GCC and
Clang and macOS Clang; the gate matrix distinguishes coverage, sanitizer and
installation checks. Windows has an informational MinGW build and is not a
validated release platform. Filesystem capabilities vary by platform; see
[archive restoration](adpat/c/ARCHIVE.md).

Fixes target the latest release and development on the main branch. Earlier
releases have no maintenance commitment. In particular, upgrading from 0.11.0
requires the [documented container migration](docs/release-notes/0.12.0.md#upgrading-from-0110).

## Compatibility

- **Containers:** the supported format set is 19–54, 56–58, 61–72 and 75–91,
  as defined by `ADAPT_VERSIONS` in [adapt.h](adpat/c/adapt.h). Decoding semantics
  for these formats are preserved in later releases; unknown numbers are
  rejected. Versions 13–15 require cadapt 0.11.0. Predictor or layout changes
  require an unambiguous version or policy field. Committed vectors test the
  formats for which encoded files exist.
- **Archives:** versions 1–4 remain readable; the CLI writes version 4.
- **C API:** the public declarations in `adapt.h` are source-stable within a
  major release. Function names, arguments and meanings, constant values and
  enum numbers remain stable. Caller-allocated structs can gain trailing fields
  in a minor release; rebuild callers against the matching header and static
  library. Internal model headers are not public interfaces.
- **Command line:** commands, options, standard-input/output notation and exit
  codes are stable within a major release. The report's first line is stable;
  `--explain` and subsequent diagnostic lines can change. The second word of
  `--version` identifies the release; its format-range annotation is informational.
- **Determinism:** identical input bytes, metadata and encoder settings in a
  given release produce identical containers across supported CPUs and compiler
  paths. Requested thread count and segment size are encoder settings; changing
  them may change the container. Available worker count and scheduling do not.
  A later release may select a different model while preserving old decoding.

## Getting help

Use the repository's issue tracker or **pedroanisio@faz.ai** for questions and
bugs. Include the release (`cadapt --version`), operating system, exact command,
error output and, when relevant, `cadapt info` output. Share a reproducing input
only when its contents are suitable for the report. Report vulnerabilities
privately using [SECURITY.md](SECURITY.md).

Changes are reviewed against [LOCKS.md](LOCKS.md) and the applicable
[quality and release gates](docs/release-pipeline.md).

## Commercial support

Response times, a named contact, integration help and commissioned work are
available by agreement with the author at **pedroanisio@faz.ai**. The MIT license
provides the software as is and does not imply a support agreement.

## Release provenance

Releases include an SPDX SBOM, NOTICE, source and binary checksums, and build,
payload and release manifests. The build manifest identifies the source
revision, toolchain and outputs; `make reproduce MANIFEST=…` rebuilds it with the
recorded settings and compares output bytes. Reproduction requires the matching
toolchain and environment. [Release packaging](docs/release-pipeline.md) explains
verification and the platform-specific download names.
