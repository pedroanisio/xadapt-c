# Support

<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14, with Claude Opus 5 (Claude Code). -->

Part of [adapt](README.md). What support exists for the codec, on what
terms, and how to ask for more — the support half of `DLV-2` in
[docs/CONSTRAINTS.md](docs/CONSTRAINTS.md), stated as it is rather than as
it might be.

## What is supported

- **Platforms.** Linux and macOS, built with GCC or Clang from
  [adpat/c/Makefile](adpat/c/Makefile); the tests, the sanitizer and
  coverage gates, the fuzz harness and the benchmark run there. Windows
  builds are provided for and named in the code but have not been
  validated; see the README's portability rules.
- **Versions.** The latest release on the main branch. The container
  format is versioned and every published version keeps decoding, so a
  file made by an older release is not stranded by an upgrade.
- **Interfaces.** The command line (`cadapt encode | decode | info`) and
  the C library interface in [adpat/c/adapt.h](adpat/c/adapt.h). Both are
  what the tests hold to; anything in `adapt_model.h` is internal.

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
`make reproduce` rebuilds and checks byte for byte, and a
[CHANGELOG.md](CHANGELOG.md).
