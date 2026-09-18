# Support

<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14, with Claude Opus 5 (Claude Code). -->

Part of [adapt](README.md). What support exists for the codec, on what
terms, and how to ask for more — the support half of `DLV-2` in
[docs/CONSTRAINTS.md](docs/CONSTRAINTS.md), stated as it is rather than as
it might be.

## What is supported

- **Platforms.** Linux and macOS, built with GCC or Clang from
  [adpat/c/Makefile](adpat/c/Makefile); the tests, the sanitizer and
  coverage gates, the fuzz harness and the benchmark run there, and the
  release pipeline ([docs/release-pipeline.md](docs/release-pipeline.md))
  builds, tests and packages on both on every push and tag. Windows
  builds are provided for and named in the code and compile in an
  informational job that gates nothing; they have not been validated.
  See the README's portability rules.
- **Versions.** The latest release on the main branch. The container
  format is versioned and every published version keeps decoding, so a
  file made by an older release is not stranded by an upgrade.
- **Interfaces.** The command line (`cadapt encode | decode | info`) and
  the C library interface in [adpat/c/adapt.h](adpat/c/adapt.h). Both are
  what the tests hold to; anything in `adapt_model.h` and the section
  headers it includes is internal.

## Compatibility

What a release promises to the release after it, stated as the tests hold
it:

- **Containers.** Every container version once on `main` decodes forever
  (`DLV-1`): the decoder reads versions 19 to 85, `adapt.h`'s
  `ADAPT_VERSIONS` names each one, and `tests/test_adapt_decode.c`
  (`test_published_versions`) decodes a pinned file of every version an
  encoder ever wrote — the metadata forms derived from their stream's
  vector through the public `adapt_header_add_metadata()`. Two numbers, 65
  and 70, have no file: the encoder that carried them refused the shape
  and the family was retired the same day; the rows stay so the numbers
  are recognized and never reused. Unused numbers (10–18, 55, 59, 60, 73,
  74) are refused. A change of layout or model is a new number; the
  encoder writes one live version per profile and variant.
- **Archives.** Archive versions 1 to 4 (`ADPTARC1` to `ADPTARC4`) are
  read forever; the command line writes version 4.
- **The C interface.** The declarations in `adapt.h`, linked statically,
  are source-stable within a major release (`cadapt --version`,
  semantic versioning in `CHANGELOG.md`): a function keeps its name,
  arguments and meaning, a constant its value, an enum member its number.
  Caller-allocated structs (`adapt_sizes`, `adapt_stats`,
  `adapt_version_info`) may gain fields at their end in a minor release —
  code compiled against an older header is rebuilt. The contract at the
  top of `adapt.h` (ownership, lifetimes, threads, errors, limits) is held
  by `adpat/c/examples/contract.c`, which `make check` runs.
- **The command line.** `cadapt encode | decode | info`, their options,
  the exit codes 0 / 1 / 2 (`RUN-4`), `-` for standard input and output,
  and the report's first line (`<in> bytes -> <out> bytes (<ratio>), …`)
  are stable within a major release; an option is added, never removed
  or given another meaning, within one. `--version` prints
  `cadapt <release> (format versions <first> to <last>)`; the release is
  the stable part, the parenthetical is informational. `--explain` and
  the lines after the report's first are diagnostics, free to change.
- **Bytes.** A given release, profile, tier and input produce the same
  container on every machine (`RUN-5`); a later release may produce a
  smaller one, never a different layout under the same version number.

## Community support

Best effort, with no service level promised:

- **Questions and bug reports:** the repository's issue tracker where it is
  hosted, or **pedroanisio@faz.ai**. Include `cadapt --version`, the
  platform, the command line, and — for a file that misbehaves — the output
  of `cadapt info <file>` and, if you can share it, the file.
- **Security problems:** not the issue tracker; see
  [SECURITY.md](SECURITY.md).
- **Contributing:** the working rules are in [CLAUDE.md](CLAUDE.md); every
  change passes `make check` (tests, coverage floors) and `make sanitize`.

## Commercial and enterprise support

Support with a response time, a named contact, integration help, or work
on the codec's roadmap is available **by arrangement with the author**:
write to **pedroanisio@faz.ai** with what you need. No such agreement is
implied by the MIT licence, which provides the software as is.

## Provenance

Every release carries an SPDX SBOM ([sbom.spdx.json](sbom.spdx.json)), a
build manifest ([build-manifest.json](build-manifest.json)) that
`make reproduce` rebuilds and checks byte for byte, a `NOTICE` generated
from the SBOM, `SHA256SUMS` over every artifact, a release manifest, and a
[CHANGELOG.md](CHANGELOG.md); `make release` makes them from an export of
the tagged revision ([docs/release-pipeline.md](docs/release-pipeline.md)).
