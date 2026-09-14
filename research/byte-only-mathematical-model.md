# Mathematical model for byte-only compression improvements

<!-- Authored with Codex / GPT-6, 2026-09-14. -->

This document formalizes the proposals in the [research report](byte-only-compression-strategy.md). It supplies a complete-container objective, causal predictor constructions, conditional loss bounds, counterexamples, resource accounting, and [reproducible mathematical checks](verify_byte_only_math.py).

**Status: a mathematical specification and re-derivation of established methods.** The propositions below are proved under their stated assumptions. The usefulness of the proposed expert families and state-retention rules remains a hypothesis. Neither a new compression theorem nor a measured 30% improvement is claimed.

The governing FMT-1 through FMT-8 requirement rows were checked against the current docs/CONSTRAINTS.md and match the constraint snapshot embedded in [byte-only-baseline.json](byte-only-baseline.json), whose snapshot SHA-256 is 03f94497b8d565a09281535b32ba64c87b98c80b77e727f1902f5b33230854cf. The live document's introductory wording has changed since that snapshot. Fixed algorithmic transforms are allowed by FMT-2; tokenizers, dictionaries, character decoding, and language models remain excluded by FMT-8. The referenced targets.md is absent, so numerical runtime requirements are parameters here.

## 1. Objects and the optimization problem

Let A be one finite input object: a file or a filesystem tree including its required metadata. Let P be the caller's profile and B its resource envelope. A candidate encoder choice theta specifies only algorithms supported by the fixed codec: expert configuration, update rules, routing rules, transforms, framing, and any input-derived parameters.

Write C_theta(A) for the **actual serialized container**, not its modeled payload. Its length in bits is

$$
L_\theta(A)=8\,|C_\theta(A)|_{\mathrm{bytes}}.
\tag{1}
$$

A candidate is admissible only if all of the following hold:

- D(C_theta(A)) = A, including the required filesystem identity and metadata.
- D is the fixed decoder and uses no outside data, model, dictionary, or service.
- Every choice not implied by the format or recoverable causally is encoded in C_theta(A).
- The predictor consumes raw bits/bytes and respects FMT-8.
- Encoder and decoder resources satisfy P and B; the default decoder allocation comes from the container declaration.

The objective is

$$
\theta^*(A;P,B)\in
\arg\min_{\theta\in\Theta(A;P,B)} L_\theta(A).
\tag{2}
$$

This defines an objective over an explicitly admissible family. A terminating search evaluates a finite subset S of that family. It can prove optimality over S, not automatically over all computable representations.

The resource envelope includes peak working memory and, when bounded by the profile, **total** encoder search work. Each trial individually fitting a budget does not make an unbounded number of trials admissible. Required initialization data remains container overhead even if encoding uses it only once.

For a fixed candidate representation, let x be the N-byte stream presented to the byte codec, and b_1:T its fixed-order bits, with T = 8N. For folders, this may be an unambiguous archive serialization; it is not automatically the sum of file-content sizes. Header and metadata accounting must avoid counting the same bytes twice.

For N = 0 the prediction loss is zero and the objective still counts the container. Per-input-bit rates are undefined. The switching formulas below assume T >= 1; the empty-sequence distribution is Q(empty) = 1.

## 2. The decoder synchronization invariant

Let h_t = b_<t denote the already decoded prefix. A predictor has state s_t and parameters theta available from the format or container:

$$
s_1=I(\theta),\qquad
p_t=F(s_t,h_t;\theta),\qquad
s_{t+1}=U(s_t,b_t;\theta),
\quad 0<p_t<1.
\tag{3}
$$

Here p_t is the probability of bit 1. The probability assigned to the observed bit is

$$
q_t(b_t)=p_t^{b_t}(1-p_t)^{1-b_t}.
\tag{4}
$$

No function in (3) receives an undecoded byte. Encoder-only future information can influence theta or an explicit route, but that information must arrive before the decoder needs it.

**P1 — PROVED, conditional on inverse entropy coding and deterministic state arithmetic.** Encoder and decoder use the same probability at every step.

*Proof.* They start with the same I(theta). Assume their states and recovered prefixes agree before step t. They compute the same p_t; the inverse entropy decoder recovers the encoded b_t. Applying the same U gives identical next states. Induction proves the assertion through the declared length. A bijective representation inverse then restores A. This proves synchronization of the construction; it does not prove that the current codec implements all of FMT-7.

Finite-precision rounding, integer overflow semantics, tie rules, state eviction, hash functions, and byte order are part of F and U. Agreement on a real-valued formula alone is insufficient.

## 3. Exact accounting and the size target

Define ideal log loss in bits:

$$
\ell_\theta(b_{1:T})
=-\log_2 Q_\theta(b_{1:T})
=-\sum_{t=1}^T\log_2 q_{\theta,t}(b_t).
\tag{5}
$$

Separate complete-container length as

$$
L_\theta=H_\theta+\ell_\theta+\delta_\theta.
\tag{6}
$$

H_theta is the exactly counted framing, model-description, routing, and metadata cost outside this modeled stream. Delta_theta is the difference between actual coded payload bits and ideal log loss, including termination, finite-precision coding effects, and byte padding. It is a residual to measure, not an assumed small nonnegative constant. A stored block can be modeled by q_t = 1/2 for its literal bits, with its tag charged to H.

For two candidates using the same underlying bit representation, set

$$
G=\sum_t\log_2\frac{q_{1,t}(b_t)}{q_{0,t}(b_t)},\qquad
\Delta H=H_1-H_0,\qquad
\Delta\delta=\delta_1-\delta_0.
$$

**P2 — PROVED by subtraction of (6).**

$$
L_0-L_1=G-\Delta H-\Delta\delta.
\tag{7}
$$

A fractional reduction greater than r therefore requires exactly

$$
G>rL_0+\Delta H+\Delta\delta.
\tag{8}
$$

This is the central experimental decision rule. Better predictions must repay their additional representation and coder cost.

The [measured reference](byte-only-baseline.json) is the saved 64 MiB normal-profile executable at revision d6d66ad: N = 100,000,000 bytes and L_0 = 169,774,432 bits, or 21,221,804 container bytes. It is a pinned historical measurement, not a claim about later defaults.

| Target | Largest passing container | Minimum whole-byte saving | Equal-overhead log-loss gain per input bit |
|---|---:|---:|---:|
| More than 30% smaller | 14,855,262 bytes | 6,366,542 bytes | Strictly greater than 0.063665412 bits |
| At least twice the compression ratio | 10,610,902 bytes | 10,610,902 bytes | At least 0.10610902 bits |

The final column is the real-valued boundary before whole-byte rounding and assumes Delta H + Delta delta = 0. A 30% gain is 0.509323296 bits per input byte at that boundary; a twofold ratio gain is 0.84887216 bits per input byte.

Equivalently, under that same equal-overhead assumption,

$$
\left(\prod_{t=1}^T
 \frac{q_{1,t}(b_t)}{q_{0,t}(b_t)}\right)^{1/T}
>2^{0.3L_0/T}
=1.0451176897303868\ldots .
\tag{9}
$$

For the twofold target, replace the strict inequality with a non-strict one and the right side with 1.0763214559926713.... These are geometric mean improvements on **realized outcomes**, not classification-accuracy improvements. Normalization prevents improving both possible outcomes at every history. The calculation establishes a target, not an available algorithm.

If a transform changes the symbol stream, use the whole-sequence log-likelihood difference between the two representations in (7); do not pair nonexistent corresponding bit positions.

## 4. What additional context can buy

Let a random input be drawn from a fixed evaluation distribution. For one prediction, let Z be the current causal information and W additional causal information. Suppose q_0 uses Z and q_1 uses (Z,W). Define their expected estimation errors

$$
E_0=\mathbb E_Z D_{\mathrm{KL}}
 \big(P(B\mid Z)\,\|\,q_0(B\mid Z)\big),
$$

$$
E_1=\mathbb E_{Z,W} D_{\mathrm{KL}}
 \big(P(B\mid Z,W)\,\|\,q_1(B\mid Z,W)\big).
$$

**P3 — PROVED from the cross-entropy identity.**

$$
\mathbb E[\ell(q_0)-\ell(q_1)]
=I(B;W\mid Z)+E_0-E_1.
\tag{10}
$$

*Proof.* The two expected losses are H(B|Z)+E_0 and H(B|Z,W)+E_1. Subtract; the entropy difference is conditional mutual information.

A sparse lag, retained state, or longer match can expose information absent from Z. But a larger model can also increase E_1 by spreading observations over too many counters or causing more collisions. Under equal memory, adding W may require discarding some of Z; (10) then applies only after specifying the actual information available to each model. It is not a theorem that adding a context improves compression.

P is unknown on real files. Its mutual information and KL terms are explanatory quantities; cross-validation estimates and actual recodes, rather than an oracle P, decide usefulness.

## 5. E2: adaptation speed and mixer learning

### 5.1 Fast and slow estimates

A basic time-scale model is an exponentially weighted Bernoulli estimate:

$$
p^{(\eta)}_{t+1}=(1-\eta)p^{(\eta)}_t+\eta b_t,
\qquad 0<\eta<1.
\tag{11}
$$

Keep at least two candidates with eta_fast > eta_slow. For a context table, t in this equation counts visits to that context, unless wall-clock decay is explicitly specified.

**P4 — PROVED for the unclipped ideal estimator on an independent Bernoulli source.** If the source changes to mean theta_B and the estimator's initial mean is theta_A, then after r updates

$$
\mathbb E[p_r]
=\theta_B+(1-\eta)^r(\theta_A-\theta_B).
\tag{12}
$$

In stationarity,

$$
\operatorname{Var}(p)
=\frac{\eta}{2-\eta}\theta_B(1-\theta_B).
\tag{13}
$$

*Proof.* Taking expectations in (11) gives a first-order linear recurrence. For the variance, the next independent observation is independent of the current estimator, giving v = (1-eta)^2 v + eta^2 theta_B(1-theta_B). Solve for v.

A fast estimator forgets old bias sooner and has greater stationary variance. This justifies comparing time scales; it does not by itself establish a log-loss improvement. Clipping, quantization, context-dependent sampling, and non-independent sources require their own analysis.

### 5.2 Geometric mixing, relevant to the current codec

Let experts output probabilities p_k,t in [epsilon,1-epsilon]. Set z_k,t = ln(p_k,t/(1-p_k,t)). A geometric/logistic mixer is

$$
p_t=\sigma(w_t^\top z_t),\qquad
\sigma(a)=\frac{1}{1+e^{-a}}.
\tag{14}
$$

With bit loss f_t(w) = -b_t log_2 p_t -(1-b_t) log_2(1-p_t),

$$
\nabla f_t(w)=\frac{p_t-b_t}{\ln 2}\,z_t,\qquad
\nabla^2 f_t(w)=
\frac{p_t(1-p_t)}{\ln 2}\,z_tz_t^\top\succeq0.
\tag{15}
$$

**P5 — PROVED under exact projected-gradient assumptions.** For a convex feasible weight set of diameter D, gradients bounded by G, and update w_(t+1) = projection(w_t - eta grad f_t),

$$
\sum_{t=1}^T\big(f_t(w_t)-f_t(w)\big)
\le\frac{D^2}{2\eta}+\frac{\eta TG^2}{2}
\quad\text{for every fixed feasible }w.
\tag{16}
$$

*Proof.* Non-expansiveness of projection gives the squared-distance recurrence. Convexity bounds f_t(w_t)-f_t(w) by the gradient inner product. Rearrange and telescope; bound the initial squared distance by D^2 and the gradient squares by TG^2. The choice eta = D/(G sqrt(T)) gives DG sqrt(T) when D,G are positive. A known finite horizon or an explicitly analyzed schedule is required.

This comparator uses the **actual causal expert features** presented to the mixer. It does not retrain the experts separately under each hypothetical w. Learned gates, integer approximation, skip thresholds, and multiple interacting mixer layers do not automatically inherit (16). The code's [predict_impl and train](../adpat/c/adapt_model.h) make these distinctions material.

The arithmetic switching mixture below is a different construction from (14). Its guarantee should not be silently attributed to the present geometric mixer.

## 6. E3: sparse raw-bit contexts

Choose finite candidate lag sets S_j contained in {1,...,D_max}. A context can be

$$
c_{j,t}=
\big(t\bmod8,\ (b_{t-d})_{d\in S_j}\big),
\tag{17}
$$

with fixed start padding or explicit missing-history flags. All lags are strictly positive. No encoding, character, word, producer label, or syntax tree is needed.

For an unbounded-count reference, each context maintains counts n_0,n_1 and uses the Krichevsky–Trofimov prediction

$$
p_{j,t}=\frac{n_1(c_{j,t})+1/2}
 {n_0(c_{j,t})+n_1(c_{j,t})+1}.
\tag{18}
$$

A finite bank can combine these causal experts using the static special case of Section 8, paying at most log_2 J ideal bits relative to its best member under a uniform prior. A sparse mask captures a hypothesis about relevant positions; it is not a substring dictionary.

Context Tree Switching and SkipCTS offer richer context-family constructions.[^3][^4] Equation (17) is a simpler finite-family specification, not an implementation of those papers. Hash collisions, bounded tables, pruning, and count rescaling change the reference models. Their particular redundancy bounds cannot be transferred without re-establishing their assumptions.

## 7. E4: retain recurring probability state

Maintain K bounded expert states and a common causal gate state:

$$
s_{k,t+1}=
\begin{cases}
U_k(s_{k,t},b_t),&g_{k,t}=1,\\
s_{k,t},&g_{k,t}=0,
\end{cases}
\qquad
g_{k,t}=G_k(h_t,\psi_t;\theta).
\tag{19}
$$

The gate and its update are deterministic functions of recovered history. All hypothetical predictor routes must use the **same realized gate sequence** for the switching theorem in Section 8.

One concrete experimental family computes an integer descriptor phi from the previous completed byte block: byte-value counts and equality counts at selected lags. It chooses the nearest stored descriptor under a fixed integer distance, breaking ties by slot index. The selected slot learns the next block; inactive slots retain their probability estimates. Slot descriptors and replacement rules update deterministically afterward. Window size, lag set, distance weights, count caps, and eviction policy belong to the profile or container.

Using the previous block is deliberate: classifying the entire current block before decoding it would leak its contents unless that classification were transmitted. The resulting adaptation delay and possible descriptor collisions are real costs.

For a return to source Bernoulli(theta_A), compare a retained prediction q_A with an overwritten prediction q_B. The expected gain per returned bit is

$$
d=
D_{\mathrm{KL}}\big(\operatorname{Ber}(\theta_A)\|
                       \operatorname{Ber}(q_B)\big)
-
D_{\mathrm{KL}}\big(\operatorname{Ber}(\theta_A)\|
                       \operatorname{Ber}(q_A)\big).
\tag{20}
$$

This follows by cancelling the source entropy in the two cross-entropies. If q_A = theta_A and q_B = theta_B, it reduces to a nonnegative KL divergence. A poorly retained estimate can make d negative.

For the **illustrative, known-parameter** case theta_A = q_A = 1/4 and q_B = 3/4,

$$
d=\tfrac12\log_2 3
=0.7924812503605781\ldots\ \text{bits per returned bit}.
\tag{21}
$$

This is not a simulation of learned states or an estimate for enwik8.

If an added mechanism saves d_bar bits per affected byte over n affected bytes, transmits S additional bits, and loses D_displaced bits by removing other useful state, its simplified break-even condition is

$$
n\bar d>S+D_{\mathrm{displaced}}+\Delta\delta.
\tag{22}
$$

For S = 2 KiB and d_bar = 0.1 bits/byte with the other terms zero, the first passing integer length is 163,841 bytes. Causally reconstructed state has no transmitted-state charge, but it still consumes memory and learning work.

Selecting eviction by estimated future gain per memory byte is a **HEURISTIC**. Future recurrence is unknown. Evaluation must count losses outside the returning region as well as savings inside it.

## 8. A switching distribution with a precise guarantee

This section re-derives a standard expert-sequence/hidden-Markov construction.[^1] Assume K >= 2 deterministic causal expert sequences q_k,t and a fixed 0 < alpha < 1. Let the latent route z_t select an expert. Its prior is uniform initially and switches **to a different expert**:

$$
\pi(z_1)=1/K,\qquad
A_{ij}=
\begin{cases}
1-\alpha,&i=j,\\
\alpha/(K-1),&i\ne j.
\end{cases}
\tag{23}
$$

This convention is explicit because “reset to a uniformly sampled expert, possibly itself” has different diagonal probabilities.

Define

$$
Q(b_{1:T})
=\sum_{z_{1:T}}\pi(z_1)
 \prod_{t=2}^T A_{z_{t-1},z_t}
 \prod_{t=1}^T q_{z_t,t}(b_t).
\tag{24}
$$

The normalized forward recurrence is

$$
w_{k,1}=1/K,\qquad
q_t(b)=\sum_k w_{k,t}q_{k,t}(b),
\tag{25}
$$

$$
v_{k,t}=
\frac{w_{k,t}q_{k,t}(b_t)}
 {\sum_jw_{j,t}q_{j,t}(b_t)},\qquad
w_{k,t+1}=(1-\alpha)v_{k,t}
+\frac{\alpha}{K-1}(1-v_{k,t}).
\tag{26}
$$

It costs O(K) arithmetic operations per bit beyond expert evaluation; the dense transition sum reduces to the final expression. The decoder marginalizes routes, so **no route IDs are transmitted**. The penalty below is paid implicitly through probability mixing; charging it again as routing bytes would double-count.

**P6 — PROVED for the stated ideal distribution.** For any expert route with s actual changes among T-1 transitions,

$$
-\log_2 Q(b_{1:T})
\le
\sum_t-\log_2 q_{z_t,t}(b_t)
+
\underbrace{
\log_2K
+s\log_2\frac{K-1}{\alpha}
+(T-1-s)\log_2\frac1{1-\alpha}
}_{R(K,s,T,\alpha)}.
\tag{27}
$$

*Proof.* The summand for this route is nonnegative and no larger than the full sum (24). Its prior is K^(-1)(alpha/(K-1))^s(1-alpha)^(T-1-s). Take negative logarithms. Forward induction proves that (25)–(26) compute the same sum. Summing over possible observed sequences gives one because each expert prediction is causal and normalized.

At alpha = 0, only constant routes remain and the static mixture obeys loss <= best expert loss + log_2 K. A positive alpha is not guaranteed the same constant-comparator penalty; it also pays for not switching.

For an interior 0 < s < T-1, the hindsight choice alpha* = s/(T-1) minimizes R:

$$
R^*=\log_2K+s\log_2(K-1)
+(T-1)H_2\left(\frac{s}{T-1}\right).
\tag{28}
$$

The endpoint cases use limiting values; T = 1 has no transitions. An encoder cannot use the future-dependent alpha* for free. A selected finite-grid alpha needs a decodable configuration code, or a further mixture over rates with its own prior penalty.

Combining (27) with (6) yields a **sufficient**, not necessary, condition for a fractional target:

$$
\ell_{\mathrm{route}}+R+H_1+\delta_1
<(1-r)L_0.
\tag{29}
$$

The experiment must find useful experts: a small R does not ensure that the route loss is small. An outer switching bound also does not give each selected segment the inner geometric mixer's best independently retrained weights.

### 8.1 The private-learning-history counterexample

If “expert k” learns only when a hypothetical latent route selects k, its prediction depends on z_<t as well as b_<t. Equation (24) then needs that route-specific state. A forward table indexed only by k cannot merge arbitrary histories.

A two-expert example uses a Laplace-smoothed count predictor and observed prefix (0,1). At the third prediction, select expert 0 in both cases:

| Previous route | Data learned by expert 0 | Its probability of bit 1 |
|---|---|---:|
| (0,0) | (0,1) | 1/2 |
| (0,1) | (0) | 1/3 |

The current expert ID is the same; the next distributions differ. The verifier checks this exactly. This refutes the proposed merge rule, not the possibility of state retention.

Valid alternatives are: the shared causal gates in (19); an explicitly encoded route replayed by one decoder; or a richer hidden state carrying sufficient learning history. Specialized expert-HMM constructions distinguish these learning semantics.[^2] They do not make arbitrary bounded-cache histories interchangeable.

### 8.2 Resource and precision qualifications

O(K) arithmetic operations does not imply O(K) fixed machine words: exact posterior numerators can grow with T. A production implementation needs a stated precision strategy.

One exact, bounded-horizon alternative resets only the mixture prior every H bits while retaining the shared causal experts. In each epoch use expert frequencies f_k,t/F and alpha = a/D_alpha. With integer weights W_k initially 1, form m_k = W_k f_k,t(b_t) and update

$$
W'_k=(D_\alpha-a)(K-1)m_k
+a\left(\sum_jm_j-m_k\right).
\tag{30}
$$

The represented transition denominator is d = D_alpha(K-1). After h updates,

$$
\sum_k W_k\le K(Fd)^h,
\qquad
\operatorname{bits}(W_k)
\le 1+\left\lfloor\log_2K+h\log_2(Fd)\right\rfloor.
\tag{31}
$$

*Proof.* Summing (30) gives d times the sum of m_k, which is at most dF times the previous total weight. Iterate from K. Resetting before h exceeds H bounds weight storage independently of total file length.

This is a construction for bounding mixture-weight storage, not a fast big-integer coder. Expert state, arithmetic-coder buffers, and temporary multiplication space must also fit the budget. The loss comparison pays the sum of (27) over epochs, including a new log_2 K term per epoch. It is not the original uninterrupted bound.

Alternatively, fixed-point approximations need their own perturbation bound. For a final probability p in [epsilon,1-epsilon], suppose nearest-grid quantization satisfies |p_hat-p| <= delta < epsilon. For either observed outcome,

$$
\hat q(b)\ge q(b)(1-\delta/\epsilon),
$$

$$
\ell(\hat q)-\ell(q)
\le T\log_2\frac{\epsilon}{\epsilon-\delta}.
\tag{32}
$$

With grid denominator F, delta <= 1/(2F) applies when endpoint handling preserves the rounding guarantee. A sufficient simple condition is epsilon >= 1/F.

The proof multiplies the pointwise domination inequality and takes logarithms. It covers final-probability rounding **for the same pre-rounding state trajectory**. Rounding posterior weights changes later predictions; its total trajectory error is not bounded by 1/(2F) merely because the last rounding step is. The current codec's integer mixer does not automatically inherit (27) or (32) against an ideal infinite-precision reference.

## 9. E3/E5: independent temporal partitions

For a complete dyadic interval I, let P_0(b_I) be an independently initialized base-model likelihood. A simple binary partition mixture is

$$
Q_I=
\begin{cases}
P_0(b_I),&|I|=1,\\
\tfrac12P_0(b_I)+\tfrac12Q_{I_L}Q_{I_R},&|I|>1.
\end{cases}
\tag{33}
$$

This is the elementary partition-tree construction underlying this proposed control.[^5] At each non-leaf node, either stop and use the base model or split. The resulting prior w(partition) is the product of those one-half decisions.

**P7 — PROVED by recursive expansion and nonnegative summands.**

$$
-\log_2 Q_I
\le
-\log_2 w(\mathcal P)
+\sum_{J\in\mathcal P}-\log_2P_0(b_J)
\quad\text{for every admitted dyadic partition }\mathcal P.
\tag{34}
$$

The guarantee concerns those dyadic partitions and reset base models. Prefix prediction requires the properly marginalized prefix distribution and its online recurrence; substituting a partial block into the complete-block formula without defining its missing subtree is not justified.

There is a separate optimization for **explicitly encoded independent segments**. Let c(i,j,k) be the actual bit cost of segment b_i:j encoded from a specified reset state with mode k, including its decodable length, mode, termination, and local metadata. With additive edge costs,

$$
D(0)=0,\qquad
D(j)=\min_{0\le i<j,\ k}
 \left[D(i)+c(i+1,j,k)\right].
\tag{35}
$$

**P8 — PROVED by the last-segment decomposition.** This recurrence finds the minimum among the permitted segmentations and modes. Restricting endpoints restricts the comparison family.

With m possible endpoints and K modes, the dynamic program takes O(m^2 K) comparisons after edge costs are available. Producing those costs can dominate: naively recoding every substring of an N-symbol input takes cubic total symbol work. This is not a claim of a cheap whole-file search.

If the manifest uses a global entropy code, or earlier choices change later model state, c is not an independent scalar edge cost. Continuing a KT predictor on (0,0) gives probability 3/8; multiplying two independently reset one-bit likelihoods gives 1/4. The verifier checks this mismatch. Arbitrary retained state therefore requires an augmented-state search, explicit replay, or a restricted independent model—not an unqualified use of (35).

## 10. E5: encoder search and a precise fallback guarantee

Let S be the finite set of **completed valid containers** produced within the selected profile and resource envelope. Retain

$$
C^*=\arg\min_{C\in S} \big(|C|_{\mathrm{bytes}},\operatorname{tie}(C)\big).
\tag{36}
$$

**P9 — PROVED directly from the minimum.** If the baseline container C_0 belongs to S, then |C*| <= |C_0|.

The comparison must include all selectors and wrappers. If adding a selector changes every candidate container, the guarantee is against that wrapped baseline. There is no free promise of the old bare size. A codec that already identifies the candidate format inside each container can select complete containers without adding an outside wrapper.

Search order affects which candidates finish under a budget. Trial costs add; decoder cost is that of the selected result. An unfinished or invalid trial cannot be selected. To retain P9, the baseline must actually be available within the budget.

Only configuration selection may inspect the whole submitted input freely. The selected configuration must be reproducible from the container; it cannot depend on an unrecorded seed, model, or previous input.

## 11. E6: folder sharing and allocation

For candidate archive traversal pi, serialize A as x_pi together with sufficient path and metadata information to invert that traversal. Then optimize the **complete** L(theta,pi), not payload size alone. Repeated data already benefits from a solid stream; an added mechanism must improve that baseline.

A memory allocation mu must satisfy

$$
M_{\mathrm{history}}
+\sum_{k=1}^K M_k(\mu_k)
+M_{\mathrm{gate}}+M_{\mathrm{mix}}
+M_{\mathrm{coder}}+M_{\mathrm{manifest}}
+M_{\mathrm{temporary}}
\le M_{\mathrm{allowed}}.
\tag{37}
$$

Candidate grids over mu are legitimate encoder choices if the winning allocations are declared. A continuous condition such as equal marginal saved bits per additional memory byte applies only to a differentiable concave relaxation. Real hash tables, context collisions, and discrete cache slots need not have that shape. No concavity or greedy-allocation optimality is assumed.

All cross-file learned state is discarded at the boundary between independently encoded inputs. Within a folder container it may persist causally. Dictionaries and content-addressed deduplication are not part of the claimed FMT-8-compliant gain.

## 12. E7: reversible transforms and residual information

An eligible fixed transform must be injective as a reconstruction mapping:

$$
T_\theta(x)=(y,r),\qquad T_\theta^{-1}(y,r)=x.
\tag{38}
$$

Here y exposes useful structure and r preserves every otherwise-lost decision: original compressed bitstream choices, ordering, padding, or representation details. Theta is a fixed format rule or is encoded in the container.

The actual comparison is

$$
L_{\mathrm{transform}}
=L(\theta,\text{framing})
+L_{\mathrm{coded}}(y,r)
\quad\text{versus}\quad
L_{\mathrm{opaque}}(x).
\tag{39}
$$

If f(x)=y collapses m different originals into one y, a fixed-width residual needs at least ceil(log_2 m) bits to distinguish them. A conditional prefix code for the residual has at least that worst-case length; otherwise Kraft's inequality is violated. Under a source distribution its mean lossless residual length is at least H(X|Y). Discarding r can preserve rendered content while failing byte-exact restoration.

This is why a JPEG or document experiment must reconstruct original bytes, not just pixels or extracted text. FMT-2 permits fixed reversible rules; it does not remove the residual charge or the FMT-8 restrictions on the model.

## 13. Enterprise objective and generalization

For a fixed evaluation manifest with existing retained sizes B_i and new complete sizes C_i, the aggregate byte reduction is

$$
r_{\mathrm{bytes}}=
1-\frac{\sum_i C_i}{\sum_i B_i}.
\tag{40}
$$

It is not the unweighted mean of per-file percentages. Report every format/script family as well as this aggregate. Producer-related versions stay in the same development or held-out partition. Algorithm selection on the test corpus changes what “held out” means.

Under an explicitly affine cost model, let v_i be cost per retained byte over the comparison horizon, F_i unchanged fixed costs, and E additional encoding/decoding/operational cost over that horizon. Then

$$
r_{\mathrm{cost}}=
\frac{\sum_i v_i(B_i-C_i)-E}
 {\sum_i(F_i+v_iB_i)}.
\tag{41}
$$

More than 30% bill reduction requires the numerator to exceed 0.3 times the denominator. Existing compression, replication, deduplication, minimum object charges, and retrieval patterns determine whether this affine approximation is appropriate. No customer inputs to (41) have been measured here.

This objective is separate from FMT-3's byte minimization within the caller's chosen profile. A small container can still have unattractive restore costs.

## 14. Claim ledger and reproducible verification

The following is the initial claim ledger. Future revisions should append a superseding entry when an assumption or claim changes, rather than silently changing a verified statement's meaning.

| Claim | Status | Scope and evidence |
|---|---|---|
| P1 | PROVED, CONDITIONAL | Induction establishes synchronization under deterministic state updates and inverse coding; full filesystem support remains an implementation obligation. |
| P2–P3 | PROVED | Exact length accounting and conditional cross-entropy decomposition. |
| P4 | PROVED, CONDITIONAL | Independent Bernoulli observations and the unclipped ideal estimator. Recurrences checked symbolically. |
| P5 | PROVED, CONDITIONAL | Fixed-feature convex loss and exact projected gradient. Gradient and Hessian checked symbolically. |
| P6 | PROVED, CONDITIONAL | The normalized path-independent expert distribution (23)–(26); exact finite enumeration supplements the proof. |
| P7 | PROVED, CONDITIONAL | Complete dyadic intervals and independently reset base models. |
| P8 | PROVED, CONDITIONAL | Additive, independent, fully charged segment costs. |
| P9 | PROVED, CONDITIONAL | A completed baseline belongs to the evaluated set, including required wrapper costs. |
| (30)–(32) | PROVED, CONDITIONAL | Integer mixture-weight bound and the stated pointwise probability perturbation; not an unqualified fixed-point codec guarantee. |
| Recurrence descriptors and eviction | HEURISTIC | The proposed gate and storage policy are causal and testable, but their compression benefit is unmeasured. |
| 30% / twofold codec gain | OPEN | Necessary target accounting is established; the expert gains needed to meet it are not. |
| 30% enterprise bill reduction | OPEN | Requires a representative retained-data baseline and cost inputs. |

Run the research verifier:

    python3 -B research/verify_byte_only_math.py \
      --output research/byte-only-math-verification.json

The [recorded run](byte-only-math-verification.json), using Python 3.12.3 and SymPy 1.14.0, passed:

| Verification | Observed coverage |
|---|---:|
| Symbolic checks, including the CAS canary | 8 |
| Switching forward versus exhaustive routes | 312 sequences, 14,028 route comparisons |
| Switching distribution normalization | 20 complete finite distributions |
| Static-mixture boundary | 32 sequences |
| Recursive versus explicit dyadic partitions | 278 sequences, 6,746 comparisons |
| Independent-segment DP versus all partitions | 510 sequences, 43,690 partitions |
| Probability quantization domination | 900 outcome comparisons |
| Integer epoch weights versus rational transition matrices | 1,024 sequences, 8,192 updates |
| Deliberately incorrect/invalid cases rejected | 4 |

Counterexamples use exact fractions. The target probability multipliers and the Bernoulli KL example were independently evaluated using SymPy and Decimal at more than 70 matching decimal digits. The verifier records its own hash and the baseline hash.

These finite checks are cross-checks of the written proofs, not a proof-assistant certification, compression benchmark, or production codec implementation. SymPy is an analysis dependency only; it is not required by the codec.

## 15. What the formalization establishes

The proposed improvement is now a collection of specific obligations:

1. Build causal experts whose measured log-loss gain G is large enough to satisfy (8).
2. Charge routing, models, transforms, residuals, and framing exactly.
3. State which learning history each expert uses before applying a switching or partition bound.
4. Keep the entire decoder state inside (37), including arithmetic precision.
5. Preserve a completed baseline when claiming the non-increasing-size property.
6. Test the resulting complete containers on held-out families and apply (40)–(41) to the real storage baseline.

The immediate mathematical control is a small bank of causal fast/slow or sparse-context experts under (25)–(26), compared with both its components and the existing geometric mixer. The recurring-state gate is a separate experiment under (19). This decomposition permits a failure to be attributed to expert quality, switching penalty, capacity displacement, precision, or representation overhead, instead of treating every failed size target as the same problem.

## References

These are primary sources for the established mechanisms. The proofs and explicit assumptions above are this document's derivations; no novelty claim is made.

[^1]: Wouter Koolen and Steven de Rooij. *Combining Expert Advice Efficiently* (2008), expert-sequence priors, forward evaluation, and fixed share. [Paper](https://arxiv.org/pdf/0802.2015), §§2–4.2.
[^2]: Wouter M. Koolen and Tim van Erven. *Switching between Hidden Markov Models using Fixed Share* (2010), distinctions between learning-expert reference schemes. [Paper](https://arxiv.org/pdf/1008.4532), §§1–3.
[^3]: Joel Veness, Kee Siong Ng, Marcus Hutter, and Michael Bowling. *Context Tree Switching* (2011), context-family prediction and assumption-specific redundancy bounds. [Paper](https://arxiv.org/pdf/1111.3182).
[^4]: Marc G. Bellemare, Joel Veness, and Erik Talvitie. *Skip Context Tree Switching* (2014), contexts with irrelevant intervening positions. [Paper](https://proceedings.mlr.press/v32/bellemare14.pdf).
[^5]: Joel Veness, Martha White, Michael Bowling, and András György. *Partition Tree Weighting* (2012), averaging over independently initialized temporal partitions. [Paper](https://arxiv.org/pdf/1211.0587).
