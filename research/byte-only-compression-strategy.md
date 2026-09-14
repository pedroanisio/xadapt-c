# Byte-only compression strategy and experiment report

<!-- Authored with Codex / GPT-6, 2026-09-14. Source and measurement provenance: byte-only-baseline.json. -->

The strongest direction under the current constraints is **a byte-level predictor that adapts its context, memory, and learning speed to the input's changing statistical structure, with encoder search choosing the smallest complete result within the caller's profile and budget**. This is a research recommendation. No measured 30% improvement or twofold reduction has been established for that design.

The existing codec has now been measured on all 100,000,000 bytes of enwik8 using a saved executable from commit `d6d66ad63325df7ed30c3282c81bed4d08b1e68d`. Other work is changing the checkout concurrently; these results belong to that frozen executable and the explicit 64 MiB configuration. The measurements establish a reproducible reference and the opportunity available from choosing among its three existing profiles. They do not establish enterprise savings or generalization beyond this fixture.

The current `docs/CONSTRAINTS.md` requirements ([captured verbatim in the measurement record](byte-only-baseline.json)), especially `FMT-8`, exclude several approaches in the earlier [literature review](arxiv-compression-review.md) and experiment specification (`research/self-contained-experiment-spec.md`). This report supersedes those earlier recommendations where they conflict with the current requirements. Their historical measurements remain historical; they are not the current baseline.

## Constraint interpretation

The encoder and decoder are a fixed algorithm plus one container. There is no persistent learned state between independently encoded inputs. Anything learned from the whole current input that the decoder cannot reconstruct causally must be represented inside the container and charged to its size.

| Constraint | Consequence for the proposed research |
|---|---|
| `FMT-1`: no external dependencies | Research papers provide algorithms, not permission to call their Python frameworks, external compressors, services, or model servers from the delivered codec. |
| `FMT-2`: no shared knowledge | No corpus-specific executable, external vocabulary, pretrained weights, or prior archive supplies missing information. Fixed algorithmic rules are allowed; learned probability state comes from this input or from explicit container bytes. |
| `FMT-3`: best representation for each input | Search can be expensive and input-specific within the caller's profile and budget. Selection uses complete serialized size. A finite search establishes its best evaluated candidate, not a proof of the global shortest representation. |
| `FMT-4`: any input bytes | Every path handles arbitrary bytes, including malformed documents. Declining a specialized candidate must still leave a usable general byte representation. |
| `FMT-5` and `FMT-6`: one container, standalone decode | Only one container is delivered. It carries all reconstruction choices and required metadata. The required default decoder allocation comes from its header; only an explicit caller cap may reject that allocation. |
| `FMT-7`: filesystem fidelity | The accounting and acceptance tests include filesystem metadata, not just file contents. Permissions and access/modification times are implemented on the tested Linux path; ACLs, extended attributes, hard-link identity, sparse allocation, and broader portability remain incomplete. |
| `FMT-8`: bytes, no tokenizer/dictionary/language model | The core proposal uses byte histories, equality, recurrence, positions, and adaptive probabilities. It does not use words, syntax trees, BPE, semantic embeddings, or language identification as a replacement alphabet. |

`FMT-2` now explicitly distinguishes fixed rules of the format, including the x86 branch transform, from tables or transforms learned from an outside corpus. **Fixed reversible transforms are therefore eligible under `FMT-2`.** A transform learned from the submitted input must carry its necessary parameters in the container. Neither route permits a tokenizer, dictionary, text-decoding model, or language model prohibited by `FMT-8`. This report prioritizes byte prediction and retains exact format recompression as a separate experiment; it does not rule out fixed transforms.

Adaptive probability counters are distinguished here from a dictionary: counters estimate upcoming raw bits or bytes; they do not replace variable-length strings with token IDs. A byte model cannot satisfy `FMT-8` merely by renaming its tokenizer or language model. Likewise, explicit substring dictionaries, grammars, and content-addressed deduplication are not counted as eligible gains in this report. Required hard-link reconstruction under `FMT-7` is a filesystem identity requirement, not permission for an unrestricted dictionary compressor.

The measurement JSON embeds the exact constraint text used for the final assessment, including the clarified `FMT-2`, `FMT-3`, and `FMT-6`. The constraints reference `targets.md` and its L1/L2 provisions, but that file is absent from this checkout. No numerical throughput, latency, memory, or per-script acceptance thresholds have been inferred from that missing document.

## Current baseline and the size of the requested improvement

The [measurement record](byte-only-baseline.json) contains input and executable identities, commands, source hashes, complete-container accounting, and independent decode results. The fixture is the extracted `enwik8` member of the repository ZIP. **The experiment did not recompress the ZIP file itself.**

All runs use 64 MiB of configured codec memory and one unsegmented stream. Encoding runs without `--verify`; independent decoding and byte comparison establish correctness separately. The codec's memory option describes its allocation bound, while peak RSS below is an observed process measurement. Timing is one observation per configuration, in normal/fast/max order on a shared host with uncontrolled cache state, not a repeated throughput study. Concurrent development can affect timings; it cannot change the saved executable's identity.

| Profile | Container bytes | Bits/input byte | Encode (s) | Decode (s) | Peak RSS encode/decode (MiB) | Exact round trip |
|---|---:|---:|---:|---:|---:|---|
| normal | 21,221,804 | 1.69774432 | 86.11 | 97.01 | 43.54 / 43.02 | Passed |
| fast | 23,454,499 | 1.87635992 | 31.29 | 34.99 | 29.40 / 29.31 | Passed |
| max | 21,307,735 | 1.70461880 | 165.15 | 171.93 | 45.80 / 45.52 | Passed |

Normal is the smallest measured result. Max is **0.405% larger** and took **1.92 times** the observed encode time; fast is **10.521% larger** and used less time and memory. Choosing the smallest of these three gives **0% improvement over normal**. All three trials together consumed 282.57 seconds of observed encode time, excluding independent decode checks. This is the cost of the measured comparison, not a delivered search feature.

The equal 64 MiB caps do not mean identical allocations: the fast profile uses a 2 MiB context table, while normal and max use 16 MiB. This experiment compares existing profile behavior; it does not isolate a single predictor change at equal table size. All three outputs restore the 100,000,000 original bytes and the recorded Linux mode and access/modification times. They emit container formats 16, 17, and 18 respectively.

Against the smallest complete result of **21,221,804 bytes**, a reduction of **more than 30%** requires **14,855,262 bytes or fewer**. Doubling the compression ratio at the same input length requires **10,610,902 bytes or fewer**. These thresholds apply to the whole container, including every required model, parameter, and metadata byte.

To repeat the encode measurements, build the stated revision and run the existing collector:

```sh
python3 -B bench/telemetry.py run \
  --binary adpat/c/build/cadapt \
  --corpus static/corpus/enwik8.zip --member enwik8 \
  --output bench/results/byte-only-research \
  --profiles normal fast max --repeats 1 \
  --memory 64MiB --threads 1 --no-plots
```

The JSON retains each exact decode command. Decoding was timed separately; byte comparison and hashing occurred afterward and are excluded from decode time. File metadata was inspected before that comparison could update access time.

The encoder's first-stage configuration search operates on at most the first 32 KiB sample, with some checks on the first 256 KiB block. Probabilities continue adapting afterward, but that does not mean the selected context family or period was optimized over the full file. These are different choices, visible in `choose_model()` in [adapt_encode.c](../adpat/c/adapt_encode.c).

The framing result is particularly informative. The normal output contains **21,218,313 payload bytes and 3,491 framing bytes**, with the 32 metadata bytes included in that framing total. Even removing every framing byte would save only **0.01645%** of the container. More than 30% improvement requires saving at least **6,366,542 bytes** against this baseline. This does not measure arithmetic-coder inefficiency: quantization, normalization, termination, and model prediction loss must be separated before assigning them a potential gain. It does rule out explaining a large improvement through fixed-header cleanup.

The source also reveals a generalization question. `byte_class()` explicitly distinguishes ASCII letters and digits while grouping all values at least 128 together; `is_word_byte()` treats those high bytes as continuing one word-like hash. This still operates on bytes without decoding characters, but it is not evidence of equal effectiveness across scripts or encodings. See [adapt_model.h](../adpat/c/adapt_model.h). The issue deserves controlled measurement before claiming script neutrality.

The current folder encoder already codes a shared archive stream. Merely putting several files into a folder is therefore not a new compression mechanism. Any new folder strategy must beat that complete-container baseline, including its manifest and metadata.

## Where an improvement can come from

For a fixed decoded history, arithmetic coding approximately charges the negative logarithm of the probability assigned to the next outcome. Better probabilities can reduce payload size; changing the name of the coder cannot supply information missing from the predictor. The PAQ and mixture literature supports this separation.[^1][^2]

For this research, partition the excess coding cost into five questions:

1. **Unavailable dependency:** does the next byte depend on history the current contexts never expose?
2. **Forgotten dependency:** was useful information learned but displaced by collisions, eviction, or limited recall?
3. **Conflicting state:** are unrelated regions updating the same estimates and undoing one another's learning?
4. **Slow adaptation:** after a statistical change, how many bits are spent recovering appropriate predictions?
5. **Representation cost:** do extra model choices, restarts, and stored state cost more than the predictions save?

These categories guide experiments; they are not yet measured percentages and do not form an automatically additive accounting. One intervention can affect several categories. An expert with weak standalone predictions can still improve a mixture when its errors complement the others.

The key distinction is between an input's inherent uncertainty and the present model's avoidable surprise. Instrumentation should record the probability assigned **before** consuming each actual bit, plus the change caused by quantization. Recoding with one component removed supplies stronger evidence of marginal value than merely ranking expert accuracies.

There is also an important limit to the objective. Lossless encoding must distinguish every possible input. There are more length-`n` bitstrings than shorter bitstrings, so no injective code can shorten all inputs. A raw fallback can limit expansion to its necessary container overhead; it cannot provide a universal positive savings guarantee. Exhaustively finding the shortest arbitrary reconstruction program is also not an implementable promise. Minimum-description-length reasoning instead motivates optimizing data plus model description within a specified admissible family.[^8]

Input-specific adaptation is not benchmark overfitting by itself. An unchanged algorithm can legitimately inspect the submitted input, choose parameters, and pay for them in the output. Overfitting arises when developer choices depend on evaluation cases and the resulting benchmark gains are then presented as evidence for unseen workloads.

## Recommended architecture hypothesis

The main proposal is to **separate learning at different time scales and preserve a small number of recurring statistical states**. This differs from adding many more almost-identical contexts. It asks whether the predictor's available capacity is being applied to the wrong history at the wrong time.

Consider a byte stream with regions `A → B → A`. A single adaptive state may learn A, overwrite part of that knowledge while learning B, then pay to relearn A. A reset-only model also loses A. A small bank of causally learned probability states could retain useful estimates for the returning region. This is a proposed extension inspired by switching and nonstationary coding; the cited papers do not prove a gain for this particular bank design.[^4][^5]

The state-selection features would come from already decoded bytes: distributions of byte values, equalities at candidate lags, recent match lengths, counter ages, and previous prediction errors. There would be no producer label, filename shortcut, tokenizer, or dictionary lookup. Repeated byte patterns are evidence of recurrence; a high entropy estimate alone is not proof that an interval is incompressible.

A first design should keep the ordinary short-context model and compare fast- and slow-adapting estimates. The next step would add a tightly bounded number of states, each with a deterministic update and eviction rule. All states start independently for each container. At a fixed 64 MiB budget, adding states must reduce some other allocation or share structures; it cannot quietly multiply memory use.

There are two legitimate ways to communicate a state choice. A causal rule lets the decoder recompute it from already recovered data. Alternatively, the encoder can inspect future input to select a route and serialize that decision before the affected data. The second option spends bits but permits asymmetric work: the encoder searches; the decoder executes the selected route.

A route selected using future bytes is not free side information. Nor can one assume every possible route has the same resulting model state. Optimizing independently measured segment costs is exact only when those segments have specified independent initialization. With state reuse, changing earlier decisions changes later probabilities; a naive shortest-path calculation over fixed segment costs would optimize the wrong objective.

Context Tree Switching, Partition Tree Weighting, and SkipCTS suggest complementary investigations: adapting effective context depth, responding to changes, and ignoring irrelevant recent positions.[^4][^5][^6] The proposed system should test each effect separately before combining them. Contextual gating and estimator learning rates provide another independent axis, but adding the term “gated network” is not itself a feature: the present codec already mixes adaptive predictors.[^1][^3][^7]

The encoder can explore this space while the decoder remains a bounded deterministic byte machine. Store only the winning configuration and whatever state is actually needed to start decoding. Do not serialize the search history, failed candidates, or encoder-only diagnostics.

A useful break-even test is `N × Δh > S`: an extra state or route that saves `Δh` bits per input byte over `N` bytes must save more than its `S` transmitted bits. For example, a 2 KiB state description needs more than 163,840 affected bytes to repay itself at 0.1 bit saved per byte. These are illustrative numbers, not estimated gains. Causally reconstructed state can avoid that description cost, but still consumes decoder memory and computation. This explains why one strategy can benefit a long archive and lose on a small document.

## Literature assessment under the current constraints

The existing inventory screens 23 arXiv papers. This reassessment follows the relevant primary papers and checks newer candidates for eligibility; it is not an exhaustive inspection of every arXiv paper. Mechanism references support experiments, not transferable percentage forecasts.

| Source | Useful result or mechanism | Assessment for this codec |
|---|---|---|
| PAQ statistical perspective, 2011, §§3.1–3.2[^1] | Online prediction, gated local updates, and calibration. | Closest architectural explanation; much already exists. Use it to isolate changes in estimator behavior and gating. |
| Mixing Strategies, DCC 2012 / arXiv 2013[^2] | Geometric versus linear mixing with controlled experts. | Relevant comparison, but its gains are incremental and against its own system. |
| Linear and Geometric Mixtures, 2013[^3] | Code-length analysis of particular online weight-estimation schemes. | Useful discipline; bounds require the paper's conditions and do not automatically cover this integer implementation. |
| Context Tree Switching, 2011, §4[^4] | Richer context-tree mixtures with Calgary evaluations. | Candidate expert and switching reference. Its regret guarantees concern specified source/model classes. |
| Partition Tree Weighting, 2012, §4[^5] | Efficient averaging over temporal partitions for piecewise stationary sources. | Directly motivates adaptation experiments; extra base-model state is a real resource cost. |
| SkipCTS, ICML 2014, §4.1[^6] | Ignoring selected irrelevant context positions improves some structured binary cases. | Useful for byte-agnostic sparse dependencies. A gain over CTS is not a gain over this codec's existing sparse contexts. |
| Gated Linear Networks, 2019, §§3–4[^7] | Local learning and feature-dependent choice of weights. | Consider deterministic byte-history gates. The paper is not a complete-container enwik8 result for this implementation. |
| MDL tutorial, 2004, §2.9.4[^8] | Separating model-description and data-description cost; limits of idealized optimization. | Accounting foundation, not an oracle for the best possible container. |
| Nonstationary prediction and mixing, 2013, §§II–IV[^9] | Estimator adaptation and encoder-side parameter optimization. | Transfer the estimator question. Its post-BWT results cannot be treated as measurements of raw-byte coding here. |
| ANS, 2013/2014[^10] | A different rate/compute tradeoff for entropy coding. | Lower priority for size until actual coder overhead is measured; not evidence that the current payload can be halved. |
| Lepton, 2017, §§3–4[^11] | Demonstrates byte-exact JPEG recompression, beyond reproducing identical pixels. | Important boundary evidence for image storage. A fixed reversible transform is eligible under `FMT-2`. A compatible experiment must still use byte modeling under `FMT-8`, restore the original coding decisions, and introduce no external codec dependency. |
| StateSMix, 2026, §4.2[^12] | Online learning without pretrained predictor weights. | Ineligible as published: uses GPT-NeoX BPE vocabulary and a token-level language-model head. Online initialization does not make it byte-only. |
| L3TC, 2024, §3.3[^13] | Compact learned text prediction and an outlier-aware tokenizer. | Ineligible as published under `FMT-8`, even when model storage is counted. |
| cmix author documentation[^14] | Strong published complete-file compression with substantial resource use. | Its English dictionary and word replacement mean the reported result is not evidence of constraint-compliant performance. |

In particular, cmix's listed enwik8 size of 14,623,723 bytes cannot be used as a compliant “30% already demonstrated” result. Its author also reports 20,987,016 KiB memory for that benchmark, a very different resource regime. Both eligibility and resource accounting must be checked before using a competitor as a reachable reference.[^14]

Lepton is useful for a different reason: it illustrates that exact reconstruction of an already compressed image requires preserving the original file's coding decisions, not just visible image content.[^11] The analogous issue applies to compressed streams inside PDF, EPUB, and office documents. A fixed transform could expose hidden redundancy by replacing an internal entropy representation, but must also encode every residual choice needed to rebuild its exact original bytes. This is a separate size opportunity from better prediction on opaque compressed bytes. It is not permission to compare extracted text with the original document's size.

## Concrete experiment sequence

Only E0 below has been executed in this report. E1–E7 are specifications for later experimental implementation. The codec source was not changed for this research.

| ID | Hypothesis and controlled comparison | Required result and rejection condition |
|---|---|---|
| E0 — Existing profile choice | Encode the entire fixture with normal, fast, and max at the same budget; independently decode each output. | Completed in the measurement record. The smallest existing output is a cross-profile reference, not evidence for an unimplemented selector or permission to ignore the caller's profile. |
| E1 — Locate avoidable prediction loss | Instrument per-bit loss before/after probability quantization, per-expert loss, context evictions, match reuse, and recovery after source changes. Compare component-removal recodes. | Retain a diagnostic only if its aggregates reconcile with actual coded payload and its overhead is excluded from timing comparisons. Do not claim a gain from an oracle that sees the true next bit. |
| E2 — Adaptation rate | Compare one estimator, paired fast/slow estimators, and a bounded switching rule, holding contexts and total allocation fixed. | Must improve complete size on changing-source controls and held-out real data. Reject if it only tracks the synthetic change schedule or spends more switching bits than it saves. |
| E3 — Context relevance | Add one bounded CTS/SkipCTS-style byte or bit expert; compare it with the existing sparse and period contexts. | Require marginal gain after equal-memory displacement of other state. Reject a result explained solely by a larger memory allocation or already-covered dependencies. |
| E4 — Recurring state | Compare one state, reset-only adaptation, and 2/4/8 bounded states on recurring regions. Test both causal routing and explicitly encoded routing. | Count selection, initialization, and any stored-state bytes. Reject if gains vanish on unseen producers, mixed order, or realistic gaps between recurring regions. |
| E5 — Whole-input choice | Compare first-block selection with complete recodes of configurations permitted by the selected profile and budget. Add independently initialized partitions only after the whole-file control. | The winner must be the smallest actual complete candidate evaluated, with a fixed tie rule. Track all trial cost. Do not claim global optimality. |
| E6 — Folder recurrence and budget allocation | Compare current directory-order solid coding with eligible state-management choices over the same archive bytes. | Preserve every file and required metadata. Report additional manifest/state memory. Explicit dictionary deduplication is excluded from the claimed core gain. |
| E7 — Expose compressed-format redundancy | Compare opaque-byte coding with one fixed reversible format transform and byte-coded residual reconstruction choices. | Under `FMT-1`, `FMT-2`, and `FMT-8`, count transform selection, parameters, and residual bytes. Restore the original compressed bitstream exactly; reject the candidate when the complete result is larger. |

Development controls should include stationary data, abrupt changes, recurring regions separated by irrelevant data, noisy fixed-stride records, long exact repetitions, and adversarial hash-collision patterns. Change region lengths and ordering between development and evaluation. Success on a control identifies a mechanism; it does not estimate enterprise workload savings.

The most informative first implementation, if separately authorized, would be **E1 plus a small E2 ablation**, followed by E3 only where the diagnostics show missing dependencies. E4 is the highest-uncertainty architectural bet. E5 provides an encoder-search control and a way to avoid shipping an evaluated candidate that is larger than its available alternative. E7 is a separate investigation for workloads dominated by already compressed documents or images, where statistical improvements on plain text may contribute little. These are priorities, not promised outcomes.

## Generalization and acceptance

Enwik8 remains one fixture. A deployment claim needs a corpus manifest that separates genuinely different producers and collections. Related versions and near duplicates must stay together when forming development and held-out sets. No such enterprise manifest was available for this report.

The evaluation set should cover the following families, with small and large cases in each where meaningful:

| Family | Required distinctions |
|---|---|
| Plain text and source | Latin, Cyrillic, CJK, diacritics; UTF-8, UTF-16, legacy encodings, invalid byte sequences, varied line endings. Corpus labels are evaluator metadata and are not passed to the model. |
| CSV, HTML, XML, CSS, structured logs | Different producers, punctuation conventions, field widths, repetition, rare values, and malformed examples. Exact original spelling and whitespace remain bytes to restore. |
| Binaries, executables, raw arrays/images | Local and long-range recurrence, endianness, strides, dense numeric data, and incompressible controls. |
| JPEG/PNG, PDF, EPUB and archives | Already compressed, partly compressed, encrypted, and uncompressed members; compare the complete original file, not extracted logical content. |
| Folders | Tiny-file overhead, empty paths/entries where permitted, repeated content, hard-link identity, sparse extents, symlinks, metadata and portability limits. |

Byte relabeling and order perturbations are useful diagnostic controls, but should not be turned into a requirement that every byte permutation has identical compressed size. They expose reliance on handcrafted byte values or favorable ordering. Real-language and real-binary tests remain necessary.

For each accepted measurement, retain input hashes and byte counts, source/build and executable identities, all configuration choices, complete container bytes, component totals, observed resources, all search/training work, independent decode status, and exact comparison. Keep failure, timeout, and unsupported-feature records in the denominator. The measurement record here includes successful byte comparisons but is not a hostile-container audit or an OS-isolated no-network certification.

The strongest standalone acceptance exercise copies only the fixed decoder and one container into an isolated environment, then compares the restored result externally. For `FMT-7`, contents and paths alone are insufficient: preserve and compare modes, timestamps, ACLs, extended attributes, link identity and sparse allocation under explicit portability rules. The existing Linux metadata fixes cover only part of that requirement.

Encoding verification remains optional for normal operation and runs only with `--verify`. This does not make verification optional for research claims: every reported compression result still needs an independently established exact round trip.

Choose objective thresholds before opening the held-out set. Report both a byte-weighted aggregate and every format/script family; avoid presenting a good aggregate as improvement for every input. Compare equal-resource configurations separately from best-size configurations with larger budgets. There is no evidence yet that the proposed predictor meets a fixed throughput or decode-latency target.

## Enterprise storage economics

A 30% reduction in a container's bytes is not automatically a 30% reduction in an enterprise's storage bill. The baseline must be what is currently retained and billed, including existing compression, deduplication, replicas, minimum object charges, and retention. Compare identical logical data, filesystem fidelity, durability, and retrieval service.

A useful sensitivity calculation is:

`fractional cost reduction ≈ s × f × r − δ`

Here `s` is the fraction of the original bill that scales with retained bytes, `f` is the fraction of those baseline bytes eligible for the measured improvement, `r` is the reduction on that eligible portion, and `δ` is additional normalized encoding, decoding, retrieval, and operational cost. This is an illustrative cost model, not a quoted vendor price or a measured customer result.

For example, `s = 0.80`, `f = 0.60`, and `r = 0.40` yield 19.2% before additional costs. Even a 40% improvement on the eligible data would miss the 30% bill target in that scenario. With all bill components assumed proportional to size and only 60% of baseline bytes compressible further, a 30% overall byte reduction would require 50% reduction within that portion.

One-time encoding makes more encoder search economically plausible, especially for long retention and infrequent restores. It does not make decoder cost irrelevant. A small representation requiring excessive memory or restore time may cost more over its lifetime. Present the smallest representation and the size/resource frontier separately so the storage objective does not silently become a speed objective.

No reduction target can be guaranteed for every arbitrary, well-encrypted, or already effectively compressed input. Under the present constraints, the claim worth testing is a reduction on a specified, representative workload, with honest coverage of opaque formats.

## Decision

Proceed as research toward **better byte-level adaptation, sparse dependencies, recurring probability state, and complete-container selection**. The concrete contribution is a falsifiable hypothesis about where prediction capacity is wasted, paired with tests that can distinguish that mechanism from more memory, favorable input order, or benchmark tuning.

The fresh enwik8 reference is **21,221,804 bytes**, with all three profile outputs independently restored exactly. None of the existing alternative profiles beats normal. The next proposed design would need to reach **14,855,262 bytes** for more than 30% reduction, or **10,610,902 bytes** to double the current compression ratio. These are input-specific targets, not evidence that either will generalize.

Do not use token-based language-model results, dictionary-backed cmix results, or pixel-preserving image transcoding as proof of this constraint-compliant objective. Do not label a corpus-specific reduction as general enterprise savings. A twofold reduction remains a research target; the evidence assembled here does not establish either its impossibility on a particular input or an implementable route that achieves it.

## Sources

The references below are original papers or author-maintained documentation. Full-text sections relevant to the stated mechanisms and qualifications were inspected where cited; the mixture-analysis and ANS entries were screened at abstract level. Numerical performance from other systems was not reproduced locally. The local baseline is documented separately in [byte-only-baseline.json](byte-only-baseline.json).

[^1]: Byron Knoll and Nando de Freitas. *A Machine Learning Perspective on Predictive Coding with PAQ*. arXiv posting 2011, §§3.1–3.2. [Paper](https://arxiv.org/pdf/1108.3298).
[^2]: Christopher Mattern. *Mixing Strategies in Data Compression*. DCC 2012; arXiv 2013. [Paper](https://arxiv.org/pdf/1302.2839).
[^3]: Christopher Mattern. *Linear and Geometric Mixtures — Analysis*. DCC/arXiv 2013. Abstract screening for the scope of the stated bounds. [Record](https://arxiv.org/abs/1302.2820).
[^4]: Joel Veness, Kee Siong Ng, Marcus Hutter, and Michael Bowling. *Context Tree Switching*. 2011, §§2–4. [Paper](https://arxiv.org/pdf/1111.3182).
[^5]: Joel Veness, Martha White, Michael Bowling, and András György. *Partition Tree Weighting*. 2012, §§3–4. [Paper](https://arxiv.org/pdf/1211.0587).
[^6]: Marc G. Bellemare, Joel Veness, and Erik Talvitie. *Skip Context Tree Switching*. ICML/PMLR 2014, §4.1. [Paper](https://proceedings.mlr.press/v32/bellemare14.pdf).
[^7]: Joel Veness et al. *Gated Linear Networks*. arXiv 2019, §§3–4. [Paper](https://arxiv.org/pdf/1910.01526).
[^8]: Peter D. Grünwald. *A Tutorial Introduction to the Minimum Description Length Principle*. 2004, especially §2.9.4. [Paper](https://arxiv.org/pdf/math/0406077).
[^9]: Christopher Mattern. *Combining Non-stationary Prediction, Optimization and Mixing for Data Compression*. arXiv 2013, §§II–IV. [Paper](https://arxiv.org/pdf/1302.2856).
[^10]: Jarek Duda. *Asymmetric Numeral Systems: Entropy Coding Combining Speed of Huffman Coding with Compression Rate of Arithmetic Coding*. 2013, revised 2014. Abstract screening. [Record](https://arxiv.org/abs/1311.2540).
[^11]: Daniel Reiter Horn, Ken Elkabany, Chris Lesniewski-Laas, and Keith Winstein. *The Design, Implementation, and Deployment of a System to Transparently Compress Hundreds of Petabytes of Image Files for a File-Storage Service*. 2017. [Paper](https://arxiv.org/pdf/1704.06192).
[^12]: Roberto Tacconelli. *StateSMix: Online Lossless Compression via Mamba State Space Models and Sparse N-gram Context Mixing*. 2026, §4.2. [Paper](https://arxiv.org/html/2605.02904v1).
[^13]: Junxuan Zhang et al. *L3TC: Leveraging RWKV for Learned Lossless Low-Complexity Text Compression*. 2024, §3.3. [Paper](https://arxiv.org/html/2412.16642v2).
[^14]: Byron Knoll. *cmix*: author benchmarks, preprocessing and model description. Accessed 2026-09-14. [Documentation](https://www.byronknoll.com/cmix.html).
