# Submitting cadapt to the Large Text Compression Benchmark

<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-15, from the operator's decisions of that day: the binary alone is published, never the sources; both enwik8 and enwik9 are submitted. The listing was recorded the same day. Updated 2026-09-18 for the released 0.12.0 (9109c3f): the assets, figures and email of the update are from `dist/ltcb-0.12.0-9109c3f/manifest.json`; the listed row and the email of 2026-09-15 are kept as the record of what the benchmark shows. -->

Part of [adapt](../README.md). What the benchmark asks for, what this
repository produces for it, and the email that was sent.

## Listed

The entry is on the page since 2026-09-15 — "Sep 15 2026 - added cadapt
0.12.0, zmix 1.0, zmix 1.0.1" — as `cadapt 0.12.0 --max --allocation
context`, **the pre-release build 7c3d238 of that day, not the released
0.12.0** (its figures are in the next section; the update below is what
sends them): enwik8 18,207,203, enwik9 150,063,628, decompressor 209,893 `x`,
total 150,273,521, 1385 / 1748 ns per byte, 661 MB, CM, note 114 ("Tested
by Pedro Anisio Silva on a AMD Ryzen 5 5600X (6 cores, 3.7 GHz), 128 GB
RAM, Linux (kernel 7.0), glibc."), the description under `.1502 cadapt`
being the algorithm paragraph of the email below. Rank 30 of 226 by
total, between nanozip 0.09a and xwrt 3.2, exactly where the section
below predicted on the 224-row table; the page of that day is transcribed
in [research/ltcb-main-table.csv](../research/ltcb-main-table.csv). The
figures are the ones submitted: nothing was re-measured by the benchmark.
The goals the README states against this table are a top-20 place and
the smallest memory of every entry ranked above.

## The rules that apply

From mattmahoney.net/dc/textrules.html (read 2026-09-15): entries are sent
by email to `mattmahoneyfl(at)gmail.com` (or `matmahoney(at)yahoo.com`)
with the program's name and version, the compressed sizes of enwik8 and
enwik9, the options used, "the size of the decompressor as a zip file
(smaller of source or executable)" together with "any other files needed by
the decompressor at run time", approximate times and memory, and the test
machine. The ranking is enwik9 plus that zip. There is "no restriction on
compression/decompression time, memory, or disk space" and no licence
requirement; the program must be "available on the Internet for free
download and testing", the results "subject to public verification", and
the decompressor must run "without a network connection" and "without
selecting options that affect the contents of the uncompressed file".

The entry is the **binary alone**: the sources stay private, so the zip
that counts is the executable's (the table marks such entries `x`; 152 of
its 224 rows are executable-only). `cadapt decode` takes no option that
changes the output — the container's header names the model (`FMT-6`).

## The assets

`make ltcb-release` (`tools/ltcb_release.sh`) builds them from a committed
revision into the git-ignored `dist/ltcb-<version>-<rev>/`: the revision is
exported with `git archive` and built there, the binary stripped and
checked for embedded source paths, the exported sources deleted; the
containers are written and decoded byte for byte by that binary through
`tools/check_ratio_floor.py --verify` (or taken over from an earlier run
with `--containers`, never from under the directory the run empties);
the verifier's note takes the entry's command from the revision's
`tools/ratio_floor.json` and the memory from `cadapt info` of the enwik9
container; `SHA256SUMS` and `manifest.json` record every file. The build
reproduces: the binary of 9109c3f hashed the same (`7f3a948a…`) built from
the tag object and from the commit.

### The released 0.12.0 — `dist/ltcb-0.12.0-9109c3f/`, 2026-09-18

The tag `v0.12.0`, commit 9109c3f, the codec the release ships:

| File | Bytes | SHA-256 |
|---|---:|---|
| `cadapt` (stripped, Linux x86-64) | 583,904 | `7f3a948a877707f33c602550909836a29d80a6a825dd17356738b4ab3fe826f1` |
| `cadapt-0.12.0-linux-x86_64.zip` (the binary, `README-ltcb.txt`, `LICENSE`) — **the decompressor the ranking counts** | 249,216 | `9e25f701fa1d21feee559fca2cf41a9a2aa52cd51863810c4dc13f58537ea13f` |
| `containers/enwik8.adpt` — the compressed enwik8 | 17,800,141 | `b62a9572e1a5f0c77c0d66c97f7819349cacb0bcb65fc6c903288b42b33f8920` |
| `containers/enwik9.adpt` — the compressed enwik9 | 145,784,134 | `c2d151720a14d3bc3ab7230aa51234ed7403dc9aa12abfbaa2942ae471b2497a` |

Both containers were written by this binary under the locked command
`encode --max --allocation context --contexts lean` (tier `-9`, the
default), decoded by it and compared byte for byte with the inputs
(`floor-check.txt`): enwik8 in 108.5 CPU s, enwik9 in 1,064.6 CPU s, one
thread, on the reference host (AMD Ryzen 5 5600X, Linux 7.0, quiet). The
sizes equal the locked floors of `tools/ratio_floor.json`. The model the
enwik9 container's header names is ~654 MiB (`cadapt info`), so encode
and decode allocate the same; the release notes measured the decode of
this file at 753 s and the process peak at 667 MB under the default
command, and the entry's command was not timed on decode for this build.

**Total the table would show: 145,784,134 + 249,216 = 146,033,350**, between
`mcm 0.83 -x11` (144,934,149) and `nanozip 0.09a` (149,328,821) — rank 29
of the 226 rows of the 2026-09-15 snapshot
([research/ltcb-main-table.csv](../research/ltcb-main-table.csv)), one
above the listed row, 4,240,171 bytes (2.8 %) below it.

### The listed entry — `dist/ltcb-0.12.0-7c3d238/`, 2026-09-15 (the record)

The build of 7c3d238, sent on 2026-09-15 and listed as `cadapt 0.12.0`;
its codec is 22 library commits behind the release. Kept so the row on
the page can be matched to what produced it:

| File | Bytes | SHA-256 |
|---|---:|---|
| `cadapt` (stripped, Linux x86-64) | 465,120 | `415f9ec3782d07249c955aa70af3961ad20a308bbcb26eedd80c279ac594a6fa` |
| `cadapt-0.12.0-linux-x86_64.zip` | 209,893 | `f3c59cbe2743d2368715bbe2ebca45f00072510ab6c6c11fee751144b8a6e200` |
| `containers/enwik8.adpt` | 18,207,203 | `58a73a9b188ed8164405caca9caba5a64c89ee56b8bf4afdec9f1428946ca9ac` |
| `containers/enwik9.adpt` | 150,063,628 | `d7404c8160ffe095f105a9dd5beccdd5f0279d35e07d7a52f3ae0690165ab30c` |

Under `--max --allocation context` (the locked command of that day):
enwik8 130.5 s encode; enwik9 1,385.4 s encode, 1,748 s decode; peak RSS
661 MB; other jobs sharing the machine. Total 150,273,521, rank 30 of 226.

## Before sending

1. Put `dist/ltcb-0.12.0-9109c3f/cadapt-0.12.0-linux-x86_64.zip` at a
   public URL that will stay up (the rule is free download and public
   verification; a private repository does not satisfy it, a release asset
   or any plain file host does).
2. Put the two containers beside it, `containers/enwik8.adpt` (17.8 MB)
   and `containers/enwik9.adpt` (145.8 MB): with them a verifier decodes
   in 20 minutes and compares hashes instead of encoding for an hour, and
   the enwik9 figure is checked directly against the file that produced it.
3. Keep the three URLs and the SHA-256s above in the email, so program and
   result can both be confirmed.
4. Do not attach or link the sources, the repository, or anything under
   `_tmp/`.

## The email

The update of the listed entry, for the released 0.12.0. Subject:
`LTCB update: cadapt 0.12.0 (release build)`

```
Hello Matt,

an update to the cadapt 0.12.0 entry you listed on Sep 15: that build was
a pre-release; the released 0.12.0 codes both files smaller.

Program and version: cadapt 0.12.0 (build 9109c3f, the release), a
context-mixing compressor in C; Linux x86-64 executable, no runtime
files, MIT licence.

Download: <URL>/cadapt-0.12.0-linux-x86_64.zip
  zip 249,216 bytes, SHA-256
  9e25f701fa1d21feee559fca2cf41a9a2aa52cd51863810c4dc13f58537ea13f
  (the binary alone is published; it contains the decompressor)

Compressed files, for verification by decoding:
  <URL>/containers/enwik8.adpt   17,800,141 bytes, SHA-256
    b62a9572e1a5f0c77c0d66c97f7819349cacb0bcb65fc6c903288b42b33f8920
  <URL>/containers/enwik9.adpt  145,784,134 bytes, SHA-256
    c2d151720a14d3bc3ab7230aa51234ed7403dc9aa12abfbaa2942ae471b2497a
  ./cadapt decode enwik9.adpt -o enwik9   (md5 of the output:
    e206c3450ac99950df65bf70ef61a12d, the published enwik9)

Compressed sizes:
  enwik8: 17,800,141 bytes
  enwik9: 145,784,134 bytes

Options (the same for both, -9 is the default memory tier):
  ./cadapt encode --max --allocation context --contexts lean enwik9 -o enwik9.adpt
  ./cadapt decode enwik9.adpt -o enwik9
  (decode takes no option that changes the output; the container's
   header names the model it needs)

Approximate times, one thread, quiet machine:
  enwik9: compression 1,065 s; decompression about the same
          (the decoder runs the same model)
  enwik8: compression 109 s

Approximate memory: 654 MiB for both compression and decompression of
enwik9 (the header declares the model; decode allocates the same).

Test machine: AMD Ryzen 5 5600X (6 cores, 3.7 GHz), 128 GB RAM,
Linux (kernel 7.0), glibc.

Algorithm: CM — a context-mixing model (hashed contexts of orders 1–8,
word and record contexts, run and recall memories, five first-layer
mixers — three of them keyed per file by the byte classes, the markup
nesting or the recall state — and a final mixer, two SSE stages) with a
binary arithmetic coder, behind a
word transform whose book both sides build as they code (nothing is
transmitted or built in).

Thank you,
<name>
```

Fill in `<URL>` (the directory holding the zip and `containers/`) and the
name; nothing else in the text depends on anything outside
`dist/ltcb-0.12.0-9109c3f/manifest.json` and `SHA256SUMS`. The email of
2026-09-15, which the listed row answers, named build 7c3d238 with the
figures of the record above and the command `--max --allocation context`.
