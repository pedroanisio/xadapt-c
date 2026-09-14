# Changelog

<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14, with Claude Opus 5 (Claude Code). Seeded from the commit history. -->

All notable changes to the codec, in the shape of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). The version a binary reports
(`cadapt --version`) and the container format versions it reads and writes
are both named per release. Security fixes are listed under **Security**
and announced as [SECURITY.md](SECURITY.md) describes.

## [Unreleased]

The binary reports `0.12.0`; the tag is pending.

### Added
- Folders: a directory is coded as one container and restored byte for
  byte — every file, directory and symbolic link in its place — with
  permission bits and access/modification timestamps (nanoseconds on POSIX)
  for named files and folders, verified after application. Container
  versions 16, 17 and 18 carry the metadata; 13–15 stay readable.
- `FMT-7` in full: hard links restored as hard links, sparse files with
  their holes kept, extended attributes (`user.*`) and POSIX ACLs packed
  and restored with read-back verification, a directory's applied after
  its children. Archive version 3 (`ADPTARC3`); versions 1 and 2 decode.
  The portability rules are in the README.
- `FMT-8` measured: six corpus rows of about 2 MiB of Wikipedia text —
  English, Russian, Chinese, Japanese, Vietnamese, Czech — fetched
  reproducibly by `bench/fetch_scripts.py`, benchmarked in
  `research/scripts-measurement.md`, with Cyrillic and CJK test vectors.
- Memory presets `-0` … `-9` after xz's, `-9` (674 MiB) the default for
  encoding.
- `make coverage` (the branch-coverage gate, 80 % library / 60 % CLI),
  `make sanitize` (ASan, UBSan, LeakSanitizer), larger committed vectors
  pinning the class, context-set, period and segment choices, and CLI
  tests for every command's exact messages.
- Delivery (`DLV-2`): the MIT licence, an SPDX SBOM and a build manifest
  with `make reproduce`, this changelog, `SECURITY.md` and `SUPPORT.md`.
- A root README; `docs/CONSTRAINTS.md` states what the codec is held to.

### Changed
- `decode` allocates the model the container's header names; `--max-memory`
  is the only cap that can refuse a valid container (`FMT-6`).
- `encode` skips the round-trip proof unless `--verify` is given;
  `--no-verify` is still accepted.
- Staged outputs are published at the mode the umask leaves (0666 less the
  umask), not the staging file's private 0600.

### Fixed
- The in-memory readers no longer offset a NULL source for an empty input
  (undefined behaviour reported by UBSan).
- A test buffer leak in `test_x86_transform`.

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

Only `v0.11.0` is tagged so far; the git history under `git log` is the
record between versions.
