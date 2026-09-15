# Security

<!-- Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-14, with Claude Opus 5 (Claude Code). -->

Part of [adapt](README.md). This is the security policy `DLV-2` in
[docs/CONSTRAINTS.md](docs/CONSTRAINTS.md) asks for: what is defended, which
versions are maintained, how to report a vulnerability, and what happens
then.

## What the codec defends against

The threat the codec is built around is a **hostile container**: bytes that
claim to be a file or folder and are not. The decoders treat every input as
untrusted:

- The in-memory decoder validates the whole framing before it allocates a
  model or a result; the stream decoder allocates the model only after the
  header passed and refuses each damaged block when its damage is read.
- Every fault is a typed status with a message (`adapt_status`,
  `adapt_error_message()`); nothing is silently coerced.
- A folder's archive is checked rule by rule at the byte that breaks one,
  and no entry can name a path outside the directory being restored (no
  empty, `.` or `..` component, no NUL, no absolute path). Restoration
  happens under a staging directory that is removed on any failure. On
  POSIX the staging root stays private during extraction, and each entry's
  parent is opened relative to it with symlink following disabled at every
  component. Symbolic links may be restored but cannot serve as parents;
  hard-link sources must resolve to regular files inside that root. Windows
  also rejects native backslash and drive/stream syntax in archive paths.
- Memory is bounded by what the container's header names (at most about
  1.2 GiB by the format's own limits) and by `--max-memory`; output by
  `--max-output`. For folders, that output cap applies both to the decoded
  archive stream and to the combined logical sizes of restored files,
  including sparse holes and each hard-link alias. The sink checks this
  cumulative limit before creating, extending or linking a file.
- Threaded decoding reserves the container buffer before reading it and
  counts retained input even when only one worker fits. A file that would
  exceed the input-buffer budget, or a pipe of unknown size, uses streaming
  decoding without that buffer.
- The two decoders are fuzzed against each other under AddressSanitizer and
  UndefinedBehaviorSanitizer ([adpat/c/fuzz/](adpat/c/fuzz/)), and the test
  suites run clean under both plus LeakSanitizer (`make sanitize`).
  A regression runs the actual fuzz harness on modeled normal, fast and
  max fixtures so a stale memory cap cannot silently exclude those paths.

Out of scope: the encoder run on untrusted *source* files is not a
security boundary (it reads what it is given), and the command line trusts
its own arguments.

## Supported versions

| Version | Supported |
|---|---|
| the latest release on the main branch | yes — fixes land there |
| earlier releases | no — upgrade; the container format is versioned and old containers keep decoding |

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
**Security**, and the release's [build manifest](build-manifest.json) and
[SBOM](sbom.spdx.json) are regenerated with it.
