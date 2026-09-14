<!-- Measured by bench/telemetry.py; the numbers are quoted from research/scripts-baseline.json. Assembled with Claude Opus 5 (Claude Code). -->
# Every script, measured — FMT-8

`FMT-8` in [docs/CONSTRAINTS.md](../docs/CONSTRAINTS.md) has two halves. The
model sees bytes, never characters — there is no text decoding, tokenizer,
dictionary or language model anywhere in the codec — and every script is
not only accepted but *measured*. These are the measurements: six rows of
Wikipedia article text of about 2 MiB each, one per script, fetched
reproducibly by [bench/fetch_scripts.py](../bench/fetch_scripts.py) into
[static/corpus/scripts/](../static/corpus/scripts/) (the manifest names every
article and revision), and run through the same binary by
[bench/telemetry.py](../bench/telemetry.py) at the bench's 64 MiB budget,
one thread, no proof. Raw sessions: [scripts-baseline.json](scripts-baseline.json).

Binary: `cadapt 0.12.0 (format version 13)`, Linux-7.0.11-76070011-generic-x86_64-with-glibc2.39.

| Row | Script | Bytes | Non-ASCII | normal % | normal bits/B | fast % | fast bits/B | max % | max bits/B | normal MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `cjk-ja` | CJK (kana, kanji) | 2,100,118 | 93.1 % | 21.97 | 1.758 | 23.41 | 1.873 | 22.03 | 1.762 | 0.80 |
| `cjk-zh` | CJK (Han) | 2,097,273 | 92.9 % | 27.67 | 2.214 | 28.82 | 2.305 | 27.64 | 2.212 | 0.72 |
| `cyrillic-ru` | Cyrillic | 2,119,081 | 87.6 % | 15.12 | 1.210 | 17.27 | 1.382 | 15.09 | 1.207 | 0.97 |
| `diacritics-cs` | Latin with diacritics | 2,100,535 | 17.7 % | 22.79 | 1.824 | 24.33 | 1.946 | 22.94 | 1.835 | 0.76 |
| `diacritics-vi` | Latin with diacritics | 2,102,489 | 35.4 % | 17.93 | 1.434 | 18.95 | 1.516 | 17.48 | 1.399 | 0.88 |
| `latin-en` | Latin | 2,112,258 | 0.5 % | 22.82 | 1.825 | 24.23 | 1.939 | 22.79 | 1.823 | 0.71 |

Percentages are container bytes over input bytes (lower is better);
bits per byte likewise. Every row was modelled block by block — no block
was stored — and the encoder chose a class-history width and a context
set for each row from its first block, as it does for Latin text:

| Row | Class history | Context set (normal) | Modelled blocks |
|---|---:|---|---:|
| `cjk-ja` | 0 bytes | 4 | 9 of 9 |
| `cjk-zh` | 8 bytes | 4 | 9 of 9 |
| `cyrillic-ru` | 8 bytes | 6 | 9 of 9 |
| `diacritics-cs` | 8 bytes | 2 | 9 of 9 |
| `diacritics-vi` | 8 bytes | 6 | 9 of 9 |
| `latin-en` | 8 bytes | 0 | 9 of 9 |

## Reading the numbers

- Bits per byte is the like-for-like measure across scripts, since a CJK
  character is three UTF-8 bytes and a Cyrillic one two. Against English at
  1.825 bits/B: cjk-ja 1.758, cjk-zh 2.214, cyrillic-ru 1.210, diacritics-cs 1.824, diacritics-vi 1.434.
- No script is refused, decoded or special-cased: the byte-level contexts
  (orders 1–6, word, class history) see multi-byte sequences as the byte
  runs they are, and the same file-level choices are made for each row as for
  Latin text.
- To re-measure: `python3 bench/fetch_scripts.py --from-manifest static/corpus/scripts/manifest.json`
  refetches the same articles (content may have drifted; the manifest's
  hashes say whether it did), then one `bench/telemetry.py run` per row.
