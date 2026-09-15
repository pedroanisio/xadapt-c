# Submitting cadapt to the Large Text Compression Benchmark

<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-15, from the operator's decisions of that day: the binary alone is published, never the sources; both enwik8 and enwik9 are submitted. -->

Part of [adapt](../README.md). What the benchmark asks for, what this
repository produces for it, and the email, ready to send once the zip is
at a public address.

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

`make ltcb-release` (`tools/ltcb_release.sh`) builds them from the
committed HEAD into the git-ignored `dist/ltcb-<version>-<rev>/`: the
revision is exported with `git archive` and built there, the binary
stripped and checked for embedded source paths, the exported sources
deleted; the containers are written and decoded byte for byte by that
binary through `tools/check_ratio_floor.py --verify` (or taken over from
an earlier run with `--containers`); `SHA256SUMS` and `manifest.json`
record every file. The build reproduces: the binary of 7c3d238 hashed the
same in two builds (`415f9ec3…`).

Release of 2026-09-15, `dist/ltcb-0.12.0-7c3d238/`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `cadapt` (stripped, Linux x86-64) | 465,120 | `415f9ec3782d07249c955aa70af3961ad20a308bbcb26eedd80c279ac594a6fa` |
| `cadapt-0.12.0-linux-x86_64.zip` (the binary, `README-ltcb.txt`, `LICENSE`) — **the decompressor the ranking counts** | 209,893 | `f3c59cbe2743d2368715bbe2ebca45f00072510ab6c6c11fee751144b8a6e200` |
| `containers/enwik8.adpt` | 18,207,203 | `58a73a9b188ed8164405caca9caba5a64c89ee56b8bf4afdec9f1428946ca9ac` |
| `containers/enwik9.adpt` | 150,063,628 | `d7404c8160ffe095f105a9dd5beccdd5f0279d35e07d7a52f3ae0690165ab30c` |

Both containers were decoded by this binary and compared byte for byte with
the inputs (`floor-check.txt`). The sizes equal the locked floors of
`tools/ratio_floor.json`, whose times were measured on the same codec bytes
(a1ed995, the two commits since touch only the archive and command-line
safety paths): enwik8 130.5 s encode; enwik9 1,385.4 s encode, 1,748 s
decode; peak RSS 661 MB on enwik9 (the model the header names, so decoding
takes the same). Host: AMD Ryzen 5 5600X, Linux, one thread, other jobs
sharing the machine.

**Total the table would show: 150,063,628 + 209,893 = 150,273,521**, between
nanozip 0.09a (149,328,821) and xwrt 3.2 (151,223,933) — rank 29 of the 224
rows transcribed in `research/ltcb-main-table.csv`.

## Before sending

1. Put `cadapt-0.12.0-linux-x86_64.zip` at a public URL that will stay up
   (the rule is free download and public verification; a private repository
   does not satisfy it, a release asset or any plain file host does).
2. Keep the URL, the zip's SHA-256 above and the two container hashes in
   the email, so a verifier can confirm both the program and the result.
3. Do not attach or link the sources, the repository, or anything under
   `_tmp/`.

## The email

Subject: `LTCB submission: cadapt 0.12.0`

```
Hello Matt,

a new entry for the Large Text Compression Benchmark.

Program and version: cadapt 0.12.0 (build 7c3d238), a context-mixing
compressor in C; Linux x86-64 executable, no runtime files, MIT licence.

Download: <URL of cadapt-0.12.0-linux-x86_64.zip>
  zip 209,893 bytes, SHA-256
  f3c59cbe2743d2368715bbe2ebca45f00072510ab6c6c11fee751144b8a6e200
  (the binary alone is published; it contains the decompressor)

Compressed sizes:
  enwik8: 18,207,203 bytes
  enwik9: 150,063,628 bytes
  (SHA-256 of the enwik9 container:
   d7404c8160ffe095f105a9dd5beccdd5f0279d35e07d7a52f3ae0690165ab30c)

Options (the same for both, -9 is the default memory tier):
  ./cadapt encode --max --allocation context enwik9 -o enwik9.adpt
  ./cadapt decode enwik9.adpt -o enwik9
  (decode takes no option that changes the output; the container's
   header names the model it needs)

Approximate times, one thread, other jobs sharing the machine:
  enwik9: compression 1,385 s, decompression 1,748 s
  enwik8: compression 131 s

Approximate memory: 661 MB for both compression and decompression of
enwik9 (the header declares the model; decode allocates the same).

Test machine: AMD Ryzen 5 5600X (6 cores, 3.7 GHz), 128 GB RAM,
Linux (kernel 7.0), glibc.

Algorithm: CM — a context-mixing model (hashed contexts of orders 1–8,
word and record contexts, run and recall memories, four first-layer
mixers and a final mixer, two SSE stages) with a binary arithmetic coder,
behind a word transform whose book both sides build as they code
(nothing is transmitted or built in).

Thank you,
<name>
```

Fill in the URL and the name; nothing else in the text depends on anything
outside `dist/ltcb-0.12.0-7c3d238/manifest.json`.
