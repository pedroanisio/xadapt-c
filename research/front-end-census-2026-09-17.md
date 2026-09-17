<!-- Authored with Claude Opus 5 (Claude Code), 2026-09-17, executing phase 2 of
research/three-axis-strategy-2026-09-17.md. Every number is from a run of
bench/stream_census.c on the fixture named. -->
# What is left for a second transform

Part of [adapt](../README.md).

The front-end transforms the strategy note ranked first are worth **1.05× on
enwik8 and up to 2× on the mixed corpus**, so the plan's phase 3 is closed
against the benchmark and opened against `DLV-3`. Markup coding alone reaches
53.22 % of the Silesia `xml` row and 17.19 % of `webster` while reaching 1.88 %
of enwik8; run-length reaches 10.61 % of `mozilla` and 0.47 % of enwik8. The
work is worth doing, and it will not move the enwik8 figure.

## Method

`bench/stream_census.c` models the existing text transform's rule — a word
replaced once its third occurrence has been seen — and puts every byte of the
input in exactly one of seven buckets, so no two candidate transforms can
claim the same byte. It then reports, per candidate, the bytes it could remove
net of the codes it must write.

Every figure is an upper bound taken before the model has a say. A transform
that removes bytes the model already predicted almost for free returns time
and not ratio; on a stream the model finds hard it returns both. The census
ranks candidates and nothing more — only an encode decides the payback, which
is why the next step is a prototype and not a build.

## enwik8: the front end is spent

| Bucket | Bytes | Share |
|---|---|---|
| word, codeable — in 14,041,454 tokens | 66,635,570 | 66.64 % |
| word, still raw | 5,182,995 | 5.18 % |
| digit groups — 53,287 of them | 319,639 | 0.32 % |
| utf-8 high bytes | 537,762 | 0.54 % |
| markup tokens — 267,029 tags | 2,419,787 | 2.42 % |
| repeated-byte runs — 309,950 of them | 1,401,993 | 1.40 % |
| everything else — separators and punctuation | 23,502,254 | 23.50 % |

| Candidate | Bytes it removes | Share |
|---|---|---|
| **word book — already built** | 38,552,662 | **38.55 %** |
| markup book | 1,884,438 | 1.88 % |
| run-length | 472,143 | 0.47 % |
| utf-8 packing | 268,881 | 0.27 % |
| digit packing | 159,778 | 0.16 % |

The model is handed about 61.5 % of enwik8 after the word transform. The four
remaining candidates together remove 2.78 % of the input, which is 4.5 % of
what the model still codes, so **all four together are worth about 1.05×** —
against the 1.4× the strategy note assumed when it put them first. The largest
single bucket left is 23.50 % of separators and punctuation, and
`adapt_text.h` already records the one idea that would reach it: implying the
space before a word rather than keeping it measured *worse*, so that door is
shut too.

The plan's phase 3 gate said a candidate covering under 5 % of the stream
cannot repay an M. On enwik8 every candidate fails that gate. The gate worked.

## The mixed corpus: the same candidates are worth up to 2×

The first 16 MiB of each Silesia row, bytes each candidate could remove as a
share of the input:

| Row | word book | markup | run-length | utf-8 | digits |
|---|---|---|---|---|---|
| `xml` | 6.53 % | **53.22 %** | 0.72 % | 0.00 % | 0.08 % |
| `webster` | 18.30 % | **17.19 %** | 0.01 % | 0.00 % | 0.00 % |
| `mozilla` | 0.26 % | 0.02 % | **10.61 %** | **7.76 %** | 0.02 % |
| `samba` | 25.23 % | 5.51 % | **7.20 %** | 1.87 % | 0.23 % |
| `nci` | 0.00 % | 0.43 % | 2.53 % | 0.00 % | 0.59 % |
| `osdb` | 25.51 % | 0.00 % | 0.00 % | 1.22 % | 0.02 % |

Three things follow. **Markup coding is the largest single front-end lever
this codec does not have**: it reaches more than half of the `xml` row, where
the existing word book reaches a sixteenth, and it is the same mechanism — a
token book built as the file is read, nothing sent ahead, `FMT-2`-clean.
**Run-length and utf-8 packing are the levers for binaries**, together 18.4 %
of `mozilla` and 9.1 % of `samba`, where the word book is useless. And **no
single candidate covers the corpus**: each is large on two rows and nil on the
others, which is the input-detected shape `DLV-3` already asks for and the
same conclusion the context-set price tables reached.

## What this changes

The strategy note put transforms first because they were the only lever
measured above 1.11×, and assumed one more of the text transform's size
existed. On enwik8 it does not. The correct reading is narrower and still
worth acting on: **transforms remain the only lever that returns time and
ratio together, and the remaining ones pay on data that is not English prose.**

The enwik8 deficit against `kanzi -l9` — 1.60× — therefore does not close from
the front end. It closes from the model or not at all, and the model levers
cost ratio, which is the trade the strategy note deliberately held back to
last. That is now the only option left for the benchmark, and it should be
priced before it is spent.

## The markup book, prototyped and measured

`bench/markup_book_proto.c` implements the candidate outside the container so
its payback could be measured before a version number is spent: the same rule
as the text transform — a complete `<...>` tag coded once it has been seen
three times, the book built as the file is read, nothing sent ahead — with its
inverse, held to a byte-exact round trip on random bytes, malformed tags and
every byte value.

Applied offline, then encoded with `--mid`:

| Row | Transform alone | Container, raw | Container, transformed | Seconds |
|---|---|---|---|---|
| `xml` | 5,345,280 → 2,484,731 | 324,997 B | **321,082 B (−1.2 %)** | 4.38 → **2.25 (1.95×)** |
| `webster`, 16 MiB | 16,777,216 → 13,398,332 | 2,494,816 B | **2,457,701 B (−1.5 %)** | 11.78 → 11.42 (1.03×) |
| enwik8, 16 MiB | 16,777,216 → 16,509,542 | 3,393,807 B | 3,421,908 B (**+0.83 %**) | 10.40 → 10.47 (1.00×) |

**On `xml` the markup book wins both axes at once — a smaller container in
half the time.** That is the only result in this session's work that improved
ratio and speed together by a margin worth a format version. On `webster` it
buys 1.5 % of ratio and no time. On enwik8 it *costs* 0.83 % and buys nothing,
so it has to be input-detected, exactly as the census's coverage figures
(53.22 %, 17.19 %, 1.88 %) predicted.

One design fault the prototype exposed, and a production version must fix.
`webster` lost 20 % of its bytes to the transform and only 3 % of its time,
which breaks the proportionality the text transform showed. The reason is that
this prototype *stacks*: it spends 0x80..0xFF on tag codes and escapes every
literal high byte, and the text transform then meets a stream whose code space
is already taken and whose escapes break its words. The two books must share
one code space in a single transform rather than run in series — which is a
design constraint on the real implementation, not a reason to doubt the `xml`
result, where the markup win is large enough to dominate the interference.

## Next

Build the markup book into the container as a second entry in the text
transform's own book — one token space for words and tags, one detector, one
version pair — rather than as a transform stacked ahead of it. The `xml` row
is the acceptance case at 1.95× and −1.2 %; enwik8 is the regression case that
the detector must leave alone. Run-length and utf-8 packing follow the same
route for binaries: 10.61 % and 7.76 % of `mozilla`, 7.20 % and 1.87 % of
`samba`, neither reached by any book of tokens.
