# Compression research for xadapt-c

<!-- Research synthesis authored with Codex / GPT-6. Local measurements and external reported results are identified separately. -->

Halving the verified full-enwik8 archive requires **10,667,270 bytes or less**, down from **21,334,541 bytes**. None of the reviewed work demonstrates that result for this codec, or establishes a way to obtain it within its present 64 MiB model budget. The strongest immediate direction is to improve the existing predictor: specialize its gates, adapt its statistics at multiple time scales, and measure whether longer structural contexts add information. A compact recurrent expert is the most promising larger experiment. These are research priorities, not measured gains.

The evidence supports maintaining two distinct objectives. The main objective is a self-contained, portable lossless compressor with a bounded memory footprint. An optional second objective uses pretrained language models, with their weights explicitly charged or declared as pre-shared. Papers in the second category can report striking payload reductions without meeting the first objective.

## Scope and evidence

This review covers **23 arXiv papers**, with targeted reading of relevant methods, results, and qualifications in **21**, and abstract screening of two less applicable papers. It also includes the original SkipCTS conference paper and the authors' cmix and NNCP benchmarks. Coverage spans context mixing, changing source statistics, context trees, reversible preprocessing, recurrent predictors, pretrained and diffusion language models, and entropy coding. It is a broad targeted review through **2026-09-14**, not an exhaustive census of arXiv or a reproduction of every paper.

The [paper inventory](arxiv-compression-papers.json) records identifiers, authors, URLs, reading depth, applicability, and limitations. The [comparison CSV](compression-comparison.csv) exposes the numerical inputs and distinguishes measured bytes, reported bytes, and sizes inferred from rounded percentages. External measurements remain author reports; only the local baseline archive was freshly decoded and compared here.

## Verified baseline and version boundary

The reference is the complete saved session `20260914T123309Z-1e275502` (`bench/results/20260914T123309Z-1e275502/session.json`, a path git ignores). It encoded the full 100,000,000-byte fixture with a 67,108,864-byte memory budget, one thread, and encode-time verification disabled. The executable identifies itself as `cadapt 0.10.0`; the three profiles emit formats 10, 11, and 12.

| Profile | Archive bytes | Bits per input byte | Recorded encode seconds |
|---|---:|---:|---:|
| Fast | 23,674,129 | 1.89393032 | 29.95 |
| Normal | 21,508,175 | 1.72065400 | 78.53 |
| Max | 21,334,541 | 1.70676328 | 94.11 |
| At least 2× smaller than max | **10,667,270** | **0.85338160** | No result yet |

The timings are individual recorded observations, not isolated comparative performance estimates. This review checked the saved executable, corpus, and all three archive hashes against their records. It then used that saved executable to decode the max archive and compared every returned byte against the saved corpus. The decoder exited successfully; all 100,000,000 bytes matched. Full commands, hashes, original run records, and validation results are retained in [enwik8-baseline.json](enwik8-baseline.json).

The corpus SHA-256 is `2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8`. The archived executable SHA-256 is `fa28816f162434851e8ed2c73ba5433b32b13e2b1688a88d02ed44135a698333`.

**The live working tree is a different version.** During this review it declared formats **13/14/15**, with concurrent predictor, sizing, and transform changes. Those edits are not the source provenance of the archived executable. Recommendations below refer to the inspected live mechanisms; numerical gains are anchored to the immutable older archive. A new-format baseline must be established before scoring implementation experiments. Source hashes at review time are included in the baseline evidence file, without claiming they reproduce the saved binary.

The max archive contains 382 modeled blocks and no stored blocks. Its framing is only **3,459 bytes**, approximately **0.0162%** of the archive. Removing every framing byte would leave 21,331,082 bytes. Consequently, a container cleanup cannot account for a 10.67 MB reduction. Likewise, improving incompressible-block selection has no direct size opportunity in this particular saved run.

## Implementation mechanisms relevant to the research

The public API and format contract are in [`adapt.h`](../adpat/c/adapt.h). Encoder and decoder share the integer predictor and arithmetic rules in [`adapt_model.h`](../adpat/c/adapt_model.h). That common implementation is the main place where a research idea can change compression while retaining symmetric prediction.

| Surface | Observed implementation | Consequence for experiments |
|---|---|---|
| `start_byte` | Byte-order, word, field, record, class, and sparse hashes | Many proposed “new context” ideas already have equivalents. |
| `H_WORD2`, `H_SPARSE2`, `H_INDIRECT`, `H_NIBBLES` | Additional max-profile contexts in the live tree | Their incremental value needs an ablation; do not count adding them as future work. |
| `context_lanes` | Max profile supplies stretched probability, linear probability, and evidence of a never-contradicted context | Confidence features are already being expanded. |
| `predict_impl` | Context-selected mixers followed by an adaptive probability map | Gating and calibration experiments extend an existing architecture. |
| `train`, `map_update` | Deterministic, count-dependent adaptation; one map estimate per state at inspection | Different adaptation horizons remain a specific candidate. |
| `recall_key`, `MATCH_MIN`, `MATCH_VERIFY` | Hashed byte recall; minimum five bytes, backward verification capped at 32 | Longer-context research should examine candidate quality and match losses first. |
| `choose_model` in [`adapt_encode.c`](../adpat/c/adapt_encode.c) | Context-set/class choices based on the first block, initially a 32 KiB sample | Configuration selection is less adaptive than the continually trained probabilities. |
| `model_layout`, sizing functions | Model tables compete with history and auxiliary state for memory | Added experts must replace or shrink existing allocations under equal-budget comparisons. |
| `PROB_BITS` | Binary coding with 12-bit probabilities | Large-token-vocabulary CDF results are not directly transferable. |
| `ADAPT_X86_FLAG` | Reversible executable-branch transform selected by a binary signature | It is not an XML or lexical transform for the enwik8 fixture. |
| Segment handling | Model reset at segment boundaries; ordered coding | Parallel-model papers address a future throughput tradeoff, not a demonstrated present size gain. |

Changing prediction, update order, initialization, or probability quantization changes the bitstream contract. Experiments need an explicit version or isolated experimental format and a matching decoder. The archived format-12 decoder must remain attached to its archived data even when the live decoder rejects that version.

## Ranked implementation experiments

### 1. Specialize the existing mixers with a small number of useful gates

PAQ's architecture and gated linear networks are the closest conceptual match to the live predictor. They combine predictions through locally trained, context-selected weights. The useful extension is additional specialization using causal side information, rather than simply adding another large table.[^1][^5]

Test a bounded gate describing whether the current byte is inside XML markup, an entity, ordinary text, or a numeric field. Maintain that state incrementally from already decoded bytes, including transitions spanning blocks. Another candidate gate separates strong recall from disagreement among context experts. Keep the number of gate buckets small enough that their weights receive repeated observations.

At `predict_impl` and `train`, compare the current gates against each candidate with the same total weight budget. Record bucket occupancy, prediction loss, and how much each expert improves the final mixture. Too many gates can slow learning; larger parameter count alone is not evidence of better compression. The PAQ and GLN papers justify this experiment but do not establish a 2× enwik8 result.

### 2. Give probability estimates more than one adaptation horizon

The non-stationary prediction literature distinguishes rapid adaptation from lower-variance estimates built from longer histories. Mattern provides directly relevant binary estimators and optimization analysis; context and partition switching supply more general treatments of changing sources.[^4][^6][^7]

At `map_update`, try one fast and one slow estimate for selected high-value context families, then let the mixer learn their use. Start with a small subset rather than doubling the entire context arena. Separately test whether the current mixer learning-rate schedule reacts too slowly after an article or content-type transition. All updates must be reproduced from the decoded prefix.

Measure steady-region losses and transition losses separately. Compare against the actual current maps, which already adapt; the control is not a stationary frequency counter. A full partition-tree wrapper around multiple large codec instances is a poor first experiment under 64 MiB. A bounded approximation can be useful, but it does not inherit the paper's theorem automatically.

### 3. Add one bounded variable-order or structural-context expert

Context Tree Switching provides an explicit alternative to a fixed collection of context orders. SkipCTS extends the model family to contexts containing irrelevant intervening variables. Its original conference paper is included as supplementary evidence because an arXiv copy was not located.[^6][^24]

The live sparse masks and word-bigram context already cover part of this territory. A useful remaining experiment is one bounded expert over prior words or XML structure, with backoff when observations are sparse. If it predicts a byte distribution, derive probabilities for the current partial byte and feed them into the existing binary mixer. Charge its dictionary or node arena against the same memory limit.

For the recall path, first record the distribution of match lengths, failed candidate verifications, and surprise when a predicted match breaks. Those observations decide whether a longer search, a second candidate, or improved confidence would help. Increasing `MATCH_VERIFY` indiscriminately can add work without improving the probability distribution. The papers establish model families, not an expected gain for this particular recall table.

### 4. Evaluate an exact reversible lexical representation

Word transformation literature supports testing a different representation before entropy coding. Its numerical results are much weaker evidence for this codec than its mechanism: the reviewed dictionary paper relies on shared dictionary assumptions and a very different backend.[^9]

An enwik8 experiment could replace recurring words, XML fragments, or entities with an explicitly specified reversible representation. Preserve case, punctuation, whitespace, entity spelling, and arbitrary bytes through literal escapes. A per-file dictionary must be serialized; a causal dictionary must be constructed identically during decode. Count the transformed payload, dictionary, escape stream, and all new headers together.

Begin as a separate transform experiment feeding the existing codec. It changes the statistics seen by every downstream context; the combined improvement can be smaller than either component's isolated improvement. Reject a transform that makes its intermediate file shorter but makes the complete archive larger. XML parsing and reserialization without exact original-byte preservation is not a valid lossless solution.

### 5. Add a compact recurrent expert in a separate profile

L3TC is the most directly useful compact-neural design in this review. On full enwik8, its Table 4 reports an adjusted ratio of **17.28% for the 800K model**, including FP16 weights. The 3.2M model's 13.81% payload becomes 20.23% after model cost. These are stronger guides than a model-excluded headline, but neither is the complete measured distribution size of a C99 implementation.[^15]

A concrete hybrid experiment would allocate a small fixed memory budget to a byte-level GRU or RWKV-style predictor, produce a distribution once per completed byte, and expose its conditional bit probabilities to the current mixer. This preserves the existing coder and lets local exact-context models complement the recurrent state. DeepZip explains the causal encoder/decoder requirement; DZip supplies a bootstrap-plus-online-supporter design.[^10][^11]

There are two different accounting choices: serialize a trained compact model, or start from a deterministic initialization and replay online learning at decode time. Include parameters, tokenizer if any, optimizer state in resident memory, scratch activations, and initialization rules. Fixed-point inference or a precisely specified deterministic implementation would be necessary to preserve the portability of the current codec. Integration benefit and speed remain unmeasured.

### 6. Keep large pretrained and diffusion models as a separate research track

A stronger language prior may provide a route toward the target when external model and compute constraints are relaxed. FineZip's arithmetic variant reports a ratio of 0.0797 on the **first 10 MB**, below the numerical target rate but on a different experiment. Its pretrained-model and adaptation costs require explicit treatment. This is evidence of a different operating regime, not proof of a 10.67 MB self-contained enwik8 archive.[^14]

For this track, fix the weights, tokenizer, inference implementation, and probability quantization at both ends. Report payload bytes and standalone bytes as separate metrics. If the weights are already installed at both endpoints, also report the amortized cost over a stated number of documents. Large-model compression is not a transparent replacement for a 64 MiB C99 codec.

## Published numerical context

The following table is **not a controlled leaderboard**. The corpus extent, model accounting, hardware, and version matter. MB here is decimal; approximate sizes are explicitly marked.

| System | Input extent | Reported archive or adjusted size | Meaning for the target |
|---|---|---:|---|
| Verified cadapt max, format 12 | Full enwik8 | **21,334,541 B** | Reference archive. |
| Required result | Full enwik8 | **≤10,667,270 B** | At least 2× smaller. |
| cmix v21 | Full enwik8 | 14,623,723 B | About 31.5% smaller than reference; author reports roughly 20 GiB memory for this run.[^25] |
| NNCP, 2023-10-21 release | Full enwik8 | 14,915,298 B | About 30.1% smaller; different neural-compute regime.[^26] |
| L3TC-800K, adjusted | Full enwik8 | ≈17,280,000 B | Inferred from rounded 17.28%; includes FP16 weights, about 19.0% below reference.[^15] |
| Nacrith | Full enwik8 | 11,737,280 B | Reported payload excludes about 500 MB weights; still above target.[^16] |
| Midicoth | Full enwik8 | 21,914,998 B | Its reported archive is 2.7% larger than reference.[^19] |
| StateSMix | Full enwik8 | 26,622,640 B | Its reported archive is 24.8% larger than reference.[^17] |
| FineZip, arithmetic coding | First 10 MB | Ratio 0.0797; 0.6376 bpb | Subset experiment with a large pretrained model; not a full-file size result.[^14] |
| Diffuse to Compress, rANS | First 10 MB | Ratio 0.127; 1.016 bpb | Different input extent and a pretrained diffusion model.[^18] |

The cmix and NNCP author pages establish that stronger complete-file compression than this frozen cadapt baseline exists. They do not establish that their advantage can be transferred into the current memory and speed envelope. Their remaining gap to 10.67 MB also shows why “add more PAQ techniques” is not a substantiated 2× forecast.

## Paper-by-paper assessment

| ID | Work | Evidence and applicability |
|---|---|---|
| P01 | PAQ from a machine-learning perspective (2011) | Closest architecture reference. Read its mixer specialization and training discussion before duplicating existing context mixing. Its small final-layer optimizer experiment is a possible later ablation.[^1] |
| P02 | Mixing Strategies (2013) | Compares geometric and linear weighting with the same experts. Calgary mean rates are 2.187 versus 2.231 bpb, respectively: an incremental comparison, not a 50% result.[^2] |
| P03 | Linear and Geometric Mixtures — Analysis (2013) | Useful code-length analysis for online weight estimation. Validate the actual learning-rate and weight constraints before claiming its bounds.[^3] |
| P04 | Non-stationary prediction and mixing (2013) | Directly relevant to estimator adaptation and tuning. Its post-BWT evaluation does not establish performance on the present raw-byte context model.[^4] |
| P05 | Gated Linear Networks (2019) | Local learning and side-information gates fit the existing design. Classification and learning results provide architectural support, not measured full-enwik8 savings.[^5] |
| P06 | Context Tree Switching (2011) | Calgary evaluation reports small improvements over CTW, with gains varying by file. Consider one expert; do not replace the whole ensemble solely on a universality guarantee.[^6] |
| P07 | Partition Tree Weighting (2012) | Weights possible locally stationary partitions. Useful for source changes, but its additional base-model instances have a real memory cost.[^7] |
| P08 | Parallel two-pass MDL context trees (2014) | Shares a learned source across parallel blocks. Relevant if throughput becomes the objective; source descriptions and the preliminary pass must be included.[^8] |
| P09 | Dictionary preprocessing (2014) | Supports reversible word transforms. Shared dictionary assumptions, 100-byte BWT blocks, and unresolved special-character handling make it weak quantitative evidence for enwik8.[^9] |
| P10 | DeepZip (2018) | A clear recurrent-predictor/arithmetic-coder construction, with stored trained weights and causal decoding. Useful prototype reference, not proof of a gain over cadapt.[^10] |
| P11 | DZip (2019) | Small stored bootstrap plus a synchronized online supporter. Its ablation separates prediction gains from substantial training and decoding costs.[^11] |
| P12 | Language Modeling Is Compression (2023) | Essential evaluation reference: distinguish source prediction from coder overhead and include model size. Its main cross-modality setup uses 1 GB datasets and 2,048-byte chunks.[^12] |
| P13 | LLMZip (2023) | Original results include cleaned text8 and sampled token batches. Lowercasing and filtering cannot be substituted for lossless compression of the original Wikipedia XML.[^13] |
| P14 | FineZip (2024) | Combines document adaptation with dynamic contexts. The principal comparison is on the first 10 MB; its faster rank-coding variant differs materially from the arithmetic variant.[^14] |
| P15 | L3TC (2024) | Best compact-neural entry point here. Use its full-enwik8 adjusted results, not its enwik9 table or model-excluded payload, for preliminary feasibility.[^15] |
| P16 | Nacrith (2026) | Interesting adaptive residual bias and predictor gating. Its 24-bit CDF addresses a large vocabulary, unlike cadapt's binary alphabet. Reproduce its own result before using it as a target baseline.[^16] |
| P17 | StateSMix (2026) | CPU online neural learning is relevant in principle. The full-input result and large table footprint make this implementation a poor replacement candidate.[^17] |
| P18 | Diffuse to Compress (2026) | Parallel symbol commitment is an alternative architecture. Its 1 MB ablations and 10 MB comparison address a neural throughput tradeoff, with substantial redesign required.[^18] |
| P19 | Micro-Diffusion / Midicoth (2026) | Count-aware calibration is an ablation candidate. Its own complete compressor is larger than the reference; cadapt already has an adaptive probability map.[^19] |
| P20 | Weighted Product of Experts (2025) | Useful when combining a neural prior and local counts. The best-expert statement concerns an optimally chosen mixture, not automatic non-regression of every learned weight.[^20] |
| P21 | Asymmetric numeral systems (2013) | Entropy-coder alternative with arithmetic-like rate. Reverse coding order complicates a causally adaptive stream; not a stand-alone explanation for halving size.[^21] |
| P22 | Llamazip (2025) | Abstract screening only. Pretrained next-token exception coding has lower marginal relevance than the already reviewed probability-coding systems.[^22] |
| P23 | Compression of LLM-generated text (2025) | Abstract screening only. Very high ratios concern model-generated data, a different distribution from original enwik8 XML.[^23] |

## Findings that need qualification

**Cross-paper benchmark reuse can change the experiment.** Nacrith labels FineZip's 1.024 bpb as a full-100-MB comparison. FineZip's original Table 1 gives 0.12799 on the first 10 MB; multiplying by eight yields approximately 1.024. The corpus extent does not transfer with the number. Separately, L3TC's often-cited 16.23% is in its enwik9 table, not its enwik8 table.[^14][^15][^16]

**StateSMix's throughput table needs independent verification.** It lists 700 KB/s, approximately 4.2 hours for 100 MB, and 2,000 tokens/s together. The file size and elapsed time imply only about 6.6 KB/s. Its table also reports 6.1 GB peak RAM. These inconsistencies prevent a reliable speed comparison here; the full-input compressed-byte count remains an author-reported value.[^17]

**An optimal mixture is different from an implemented adaptive guarantee.** The wPoE proposition takes an infimum over mixture weights. Section 4.3 explicitly qualifies that weights still need to be found. A weight chosen from a sample, quantized to integers, or adapted online requires its own assessment. The existing `choose_model` has a related practical issue: a choice that wins on the first sample need not win over the rest of the file.[^20]

**Coder precision is not the same as model quality.** The current alphabet has two outcomes at each coding step. A minimum-count problem involving tens of thousands of tokens does not imply comparable waste in a 12-bit binary probability. Measure actual prediction loss before and after quantization, and compare it with payload bits, before redesigning the coder. ANS can change computational costs; it does not supply missing predictive information.[^16][^21]

**Published ratios do not establish a lower bound for this fixture.** A universal coding theorem is asymptotic and conditional on a source class. An observed good rate under a pretrained model is conditional on that model. Neither proves that 0.853 bpb is impossible for enwik8, nor that the current implementation can reach it.

## Experiment and telemetry requirements

The existing [`bench/telemetry.py`](../bench/telemetry.py) provides process and archive telemetry. Before implementing research changes, add predictor measurements in an experimental build, because RSS and elapsed-time curves cannot identify which context is wasting bits. Keep the measurement build distinct from the binary used for performance timing.

Useful counters are aggregate log loss per input block and content class, per-expert and final-mixture log loss, context lookup and eviction counts, match-length buckets, match failures, gate occupancy, calibration error by count bucket, and probability-clamp frequency. Log loss is computed from the probability assigned **before** updating on the actual bit. Dropping one expert and recoding remains the stronger measure of its marginal value; an expert's standalone accuracy can be poor while its errors complement the others.

Use a frozen baseline and one change at a time. Evaluate explicit 1 MB and 10 MB prefixes for development, then the entire 100 MB fixture. Prefix results must remain labeled as prefixes. Include unseen text, XML with unusual spacing and escapes, source code, mixed binary data, and random bytes so an enwik8-specific adjustment is not silently presented as a general improvement.

For a byte predictor added to the bit mixer, calculate each next-bit probability by summing candidate-byte probabilities consistent with the already decoded partial byte. Apply a positive minimum probability and deterministic rounding. This makes it possible to compare a new byte-level expert without simultaneously replacing the arithmetic coder. For a token predictor, tokenization and exact detokenization become additional format requirements.

The acceptance record for each candidate should include:

1. Exact input, executable, source, build options, model assets, tokenizer, and collector identity. Equal profile names are not sufficient to reuse a result.
2. Complete archive bytes; separate counts for framing, dictionary, tokenizer, stored weights, and other decoder dependencies. Report both standalone and explicitly amortized shared-model costs where applicable.
3. Encode and decode time, peak resident memory, and the precise model-budget setting. Compare equal-memory experiments separately from experiments allowed more resources.
4. Exact decoded-byte comparison and successful decoder completion. Optional verification may remain optional in the ordinary CLI; a claimed compression result must still be validated.
5. Correct handling of empty input, arbitrary bytes, block boundaries, truncation, corruption, and format selection. Check deterministic prediction across supported build targets before calling a new neural path portable.

A candidate reaches the requested **2× milestone** only when the full enwik8 result plus all newly required decoding assets is at most 10,667,270 bytes relative to this frozen reference, with an exact round trip and declared resource costs. If the new live format becomes the accepted baseline, recompute the threshold from its measured complete archive first.

The recommended order is **gates and estimator adaptation → one bounded structural expert → reversible representation → compact recurrent hybrid**. Each stage earns continuation through complete-file measurements. Large pretrained or diffusion systems belong in the separately accounted track. There is no evidence-based percentage forecast for any unimplemented stage, and component gains should not be added together as though they were independent.

## Sources

The numbered references link to original papers or author-maintained implementation pages. The paper inventory records which sections were assessed. Historical publication dates are retained; they do not imply that an older implementation result is a current record.

[^1]: Byron Knoll and Nando de Freitas. [A Machine Learning Perspective on Predictive Coding with PAQ](https://arxiv.org/abs/1108.3298). 2011. P01.
[^2]: Christopher Mattern. [Mixing Strategies in Data Compression](https://arxiv.org/abs/1302.2839). 2013 arXiv posting. P02.
[^3]: Christopher Mattern. [Linear and Geometric Mixtures — Analysis](https://arxiv.org/abs/1302.2820). 2013. P03.
[^4]: Christopher Mattern. [Combining non-stationary prediction, optimization and mixing for data compression](https://arxiv.org/abs/1302.2856). 2013. P04.
[^5]: Joel Veness et al. [Gated Linear Networks](https://arxiv.org/abs/1910.01526). 2019; inspected revised PDF dated 2020. P05.
[^6]: Joel Veness, Kee Siong Ng, Marcus Hutter, and Michael Bowling. [Context Tree Switching](https://arxiv.org/abs/1111.3182). 2011. P06.
[^7]: Joel Veness, Martha White, Michael Bowling, and András György. [Partition Tree Weighting](https://arxiv.org/abs/1211.0587). 2012. P07.
[^8]: Nikhil Krishnan and Dror Baron. [A Universal Parallel Two-Pass MDL Context Tree Compression Algorithm](https://arxiv.org/abs/1407.1514). 2014. P08.
[^9]: R. R. Baruah, V. Deka, and M. P. Bhuyan. [Enhancing Dictionary Based Preprocessing For Better Text Compression](https://arxiv.org/abs/1403.4714). 2014. P09.
[^10]: Mohit Goyal, Kedar Tatwawadi, Shubham Chandak, and Idoia Ochoa. [DeepZip: Lossless Data Compression using Recurrent Neural Networks](https://arxiv.org/abs/1811.08162). 2018. P10.
[^11]: Mohit Goyal, Kedar Tatwawadi, Shubham Chandak, and Idoia Ochoa. [DZip: improved general-purpose lossless compression based on novel neural network modeling](https://arxiv.org/abs/1911.03572). 2019. P11.
[^12]: Grégoire Delétang et al. [Language Modeling Is Compression](https://arxiv.org/html/2309.10668v2). 2023. P12.
[^13]: Chandra Shekhara Kaushik Valmeekam et al. [LLMZip: Lossless Text Compression using Large Language Models](https://arxiv.org/abs/2306.04050). 2023. P13.
[^14]: Fazal Mittu et al. [FineZip: Pushing the Limits of Large Language Models for Practical Lossless Text Compression](https://arxiv.org/html/2409.17141v1). 2024. P14.
[^15]: Junxuan Zhang et al. [L3TC: Leveraging RWKV for Learned Lossless Low-Complexity Text Compression](https://arxiv.org/html/2412.16642v2). 2024. P15.
[^16]: Roberto Tacconelli. [Nacrith: Neural Lossless Compression via Ensemble Context Modeling and High-Precision CDF Coding](https://arxiv.org/html/2602.19626v1). 2026. P16.
[^17]: Roberto Tacconelli. [StateSMix: Online Lossless Compression via Mamba State Space Models and Sparse N-gram Context Mixing](https://arxiv.org/html/2605.02904v1). 2026. P17.
[^18]: Angelo Nardone and Paolo Ferragina. [Diffuse to Compress: Leveraging Diffusion LMs for Lossless Compression](https://arxiv.org/html/2608.11249v1). 2026. P18.
[^19]: Roberto Tacconelli. [Micro-Diffusion Compression: Binary Tree Tweedie Denoising for Online Probability Estimation](https://arxiv.org/html/2603.08771v3). 2026. P19.
[^20]: Qihang Zhang, Muchen Li, Ziao Wang, Renjie Liao, and Lele Wang. [Test-Time Steering for Lossless Text Compression via Weighted Product of Experts](https://arxiv.org/html/2511.10660v1). 2025. P20.
[^21]: Jarek Duda. [Asymmetric numeral systems: entropy coding combining speed of Huffman coding with compression rate of arithmetic coding](https://arxiv.org/abs/1311.2540). 2013. P21.
[^22]: Sören Dréano, Derek Molloy, and Noel Murphy. [Llamazip: Leveraging LLaMA for Lossless Text Compression and Training Dataset Detection](https://arxiv.org/abs/2511.17589). 2025. Abstract screening only. P22.
[^23]: Yu Mao, Holger Pirk, and Chun Jason Xue. [Lossless Compression of Large Language Model-Generated Text via Next-Token Prediction](https://arxiv.org/abs/2505.06297). 2025. Abstract screening only. P23.
[^24]: Marc G. Bellemare, Joel Veness, and Erik Talvitie. [Skip Context Tree Switching](https://proceedings.mlr.press/v32/bellemare14.html). ICML/PMLR, 2014. Supplementary conference source S01.
[^25]: Byron Knoll. [cmix v21 and author benchmarks](https://www.byronknoll.com/cmix.html). Release dated September 10, 2024; page accessed September 14, 2026. S02.
[^26]: Fabrice Bellard. [NNCP and author benchmarks](https://bellard.org/nncp/). Referenced enwik8 result dated October 21, 2023; page accessed September 14, 2026. S03.
