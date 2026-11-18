# Beating the time of LTCB place 39 within 1 GiB

Part of [adapt](../README.md). Measured 2026-11-18 against the snapshot in
[ltcb-main-table.csv](ltcb-main-table.csv).

A context mixer of cadapt's own family beats 394 ns/byte compression and
281 ns/byte decompression within 1 GiB when three changes are combined: a
model with fewer inputs, an engine written for that model, and two or three
independent models coding equal parts of the file on separate cores. Measured
sizes put that design at place 41 with two models and at place 42 with three.
The single full model holds place 38 at 1,373/1,526 ns/byte, and no measured
configuration holds place 38 and the time of place 39 together.

## The target

Place 39 is mcm 0.83 -x11: 144,934,149 B in total, 394 ns/byte compression,
281 ns/byte decompression, 5,961 MB, one thread. cadapt 0.13.0 with
`--max --allocation context --contexts lean -10` codes enwik9 to
143,751,760 B; with its 258,243 B decompressor archive the total is
144,010,003 B, place 38, at 1,373 ns/byte compression and 1,526 ns/byte
decompression with a peak of 1,011 MiB. Decompression is the binding limit:
the single model is 5.43 times too slow to decode and 3.48 times too slow to
encode. The benchmark reports "the smaller of process time (summed over
processors if multi-threaded) or real time", so a program that codes on
several cores is timed by its wall clock.

## Context mixing is the only family at this size

Every faster family stops short of the size by 4 MB or more. Of the 238
entries, 72 are faster than place 39 in both directions. The smallest of them
is the Burrows–Wheeler coder bsc-m03 0.4.0 at 160,364,392 B (place 49) with
13,000 MB; the smallest by partial matching is ppmx 0.10 at 164,344,009 B with
39,727 MB; the smallest Lempel–Ziv coder is above 250 MB; and the smallest
that uses 1,024 MB or less is GRZipII 0.2.4 at 209,035,641 B (place 114). Of
the 44 smallest totals in the table, 43 belong to context mixers and neural
mixers and one to durilca'kingsize, which uses 13,000 MB at 1,341/1,466
ns/byte. A design that reaches place 44 or better inside the time limit is
therefore a context mixer made 5.4 times faster, and the open question is
which parts of the mixer can be given up at the least cost in size.

## How mcm reaches its time

mcm buys its time with memory. Its strongest text mode mixes thirteen inputs
of one probability each under a single selected weight set, reads them from a
direct-indexed table without tags or eviction, replaces a long repeat by one
flag bit, and codes the text through a two-pass word dictionary stored in the
stream. The untagged table is cheap per bit and expensive in memory: two
contexts that share a slot share statistics, and only a table of several
gigabytes keeps that rare. The published ladder shows the cost: 144,854,575 B
of enwik9 with 5,961 MB and 146,525,446 B with 3,056 MB, 1.15% for one
halving. 1 GiB is 1.6 halvings below 3,056 MB, so at a constant loss per
halving mcm would produce 149.2 MB, and the loss per halving grows as
collisions multiply. An mcm-class engine at 1 GiB is therefore estimated at
149 to 152 MB, place 41 to 43, at the price of replacing cadapt's model and
format. The estimate is an extrapolation from the published points; mcm was
not run for this study.

## What a lighter cadapt model costs

cadapt's tagged tables already hold the size at 1 GiB, so the time has to come
out of the mixing, and most of the mixing can go for about 1% of size. The
full model gives each of fifteen contexts three inputs and mixes them in five
first-layer arbiters. One input per context, two arbiters and ten contexts cut
the context inputs from 45 to 10 and the first-layer arbiters from five to
two, for 1.09% of enwik9:

| Model, one instance in the 1 GiB layout | enwik8 (B) | enwik9 (B) | Against the full model |
| --- | ---: | ---: | ---: |
| Full: 15 contexts, 3 inputs each, 5 arbiters | 17,772,852 | 143,751,760 | — |
| One input per context | 17,835,693 | | +0.35% |
| One input per context, 2 arbiters | 17,916,995 | 145,046,965 | +0.81%, +0.90% |
| The same with 10 contexts | 17,982,225 | 145,313,010 | +1.18%, +1.09% |
| The same with 8 contexts | 18,046,599 | 145,784,904 | +1.54%, +1.41% |
| 10 contexts without the run memories | 18,323,172 | | +3.10% |

The ten contexts are orders 2 to 6, class, word2, sparse2, nest and wordlc;
order 1, sparse, record, indirect and nibbles each cost under 0.1% when left
out. The run memories stay, because the ten-context model loses a further
1.9% without them. A single ten-context model totals 145,571,253 B, place 40,
so the lighter model by itself costs two places and the remaining cost depends
on how its speed is obtained.

## The engine is the larger part of the cost

The present engine turns the lighter model into only 1.3 times the speed,
because most of its instructions do not depend on the number of inputs.
Decoding takes 2,124 instructions per coded bit under the full model and 1,759
under the ten-context model (callgrind, 2 MiB of enwik8), and the measured
decode time fell by the factor 1.30. Of the 1,759, learning takes 759,
prediction 581, and byte boundaries, hashing, prefetching and the coder 411.
The loops still run over 32 padded lanes, prefetch the rows of three arbiters,
walk every run memory and hash both candidate bytes in the decoder. Counting
only the operations the ten-context model needs gives about 720 instructions
per bit:

| Work per coded bit | Instructions (estimate) |
| --- | ---: |
| Inputs: 11 state lookups, 6 run memories, recall | 130 |
| Two arbiters and the final arbiter | 45 |
| Two estimation stages | 35 |
| Arithmetic decoder | 20 |
| Learning: weights, estimation cells, states and maps | 265 |
| Nibble boundary: ten claims, hashing, prefetch | 125 |
| Byte boundary: ten context hashes, recall, transform, prefetch | 100 |
| Total | 720 |

An engine written for the ten-context model is therefore projected at 720 to
800 instructions per bit, 2.7 to 3.0 times fewer than the full model takes
today. The projection is a count of operations; no such engine exists yet, and
its measured count is the first gate below. At that count a single model still
decodes in 517 to 575 ns/byte, above the limit of 281.

## Independent models supply the remaining factor

Independent models on separate cores close the gap that the engine leaves, at
a measured cost in size. One model has 281 ns/byte times the number of models
to decode its part, which is 391 instructions per bit times that number
(2,124 × 281 / 1,526). Each model sees only its part of the file and holds
only its share of the memory:

| Ten-context models | Memory each | enwik9 (B) | Total (B) | Place | Decode budget (instructions per bit) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1,006 MiB | 145,313,010 | 145,571,253 | 40 | 391 |
| 2 | 430 MiB | 150,134,964 | 150,393,207 | 41 | 782 |
| 3 | 334 MiB | 152,467,695 | 152,725,938 | 42 | 1,173 |

Two models cost 3.3% over one and three cost 4.9%. The full model does not
escape this trade: three full models of 350 MiB give 150,589,516 B, also place
41, against a budget of 1,173 instructions per bit, and its prediction and
learning alone take 1,601 today after vectorization; six full models of
176 MiB give 156,452,849 B, place 44. At the projected 720 to 800 instructions
per bit the ten-context engine decodes two models in 259 to 287 ns/byte and
three in 172 to 192, and encodes them in 233 to 259 and 155 to 172 ns/byte.
Two models therefore land on the limit of 281 without margin, and three pass
with a third to spare.

## Position

The design that beats the time of place 39 within 1 GiB is a segment-parallel
ten-context mixer: place 42 with three models and a third of the time in hand,
place 41 with two models if the engine comes in at 700 instructions per bit or
fewer. Place 41 is 830,726 B ahead of place 42 and 1,064,386 B behind place
40. Three steps build it, ordered so that the largest uncertainty is retired
first:

1. The ten-context engine as a new profile with its own container versions.
   Its gate is the decode instruction count on the 2 MiB sample: at most 1,173
   for three models, at most 700 for two.
2. Segment input and output that stream, so that the models and their buffers
   fit in 1 GiB. The present threaded path buffers whole segments and peaked
   at 1.18 GB with 64 MiB segments.
3. Both times on the reference host through `tools/ltcb_release.sh`.

The benchmark entry stays the single full model at place 38 unless the
two-model configuration passes both limits on the reference host; the
three-model configuration is the fast profile in either case.

Two results would change this position. mcm measured at 1 GiB below 149.0 MB
of enwik9 would make its untagged table the better base, because it would
reach place 40 on one thread. A reference-host decode time for the single
model other than the 1,526 ns/byte measured here moves every budget in
proportion.

## Measurement conditions

Sizes and times were taken on a shared 4-core cloud VM (Intel Xeon 2.3 GHz),
which is not the reference host; enwik9 has MD5
e206c3450ac99950df65bf70ef61a12d. The reduced models are development builds of
the 0.13.0 codec with the named inputs, arbiters and contexts switched off;
they are not part of the tree, and their sizes are encoder output, with
byte-exact restoration checked on enwik8 samples of 2 and 8 MiB. The split
figures are the sums of independent containers over equal byte ranges of
enwik9, with 860 MiB of model memory for two ten-context models and 1,002 MiB
for three. Four workers on the VM's four virtual cores reached 2.6 times the
encoding speed and 3.2 times the decoding speed of one; the projections assume
that two or three models on the reference host's six cores scale with their
count. Places are computed on the snapshot with cadapt's published row
replaced and a 258,243 B decompressor archive.
