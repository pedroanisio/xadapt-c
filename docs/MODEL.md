# Predictor synchronization and container accounting

Part of [adapt](../README.md). These invariants explain the adaptive predictor
and candidate selection implemented by the codec.

## Deterministic prediction

Before bit t, encoder and decoder have the same decoded prefix and predictor
state. Both compute the probability before observing that bit, then apply the
same update after entropy coding. By induction, deterministic initialization,
inverse entropy coding and identical updates preserve synchronization.

Integer rounding, overflow semantics, tie breaking, eviction, hashing and byte
order are part of the format. Agreement on a real-valued formula is insufficient.
An encoder choice based on future input must be represented in the container
before the decoder needs it. All adaptive experts share the decoded history;
a retained-bank routing decision affects subsequent predictions only.

## Complete-container objective

Candidate selection minimizes actual serialized bytes, including framing,
metadata, model-selection fields, transforms and checksums. For an eligible
finite candidate set S containing the base candidate, selecting the shortest
successful container gives:

    size(selected) = min(size(candidate) for candidate in S)
    size(selected) <= size(base)

This guarantee requires a successfully encoded base candidate. It establishes
optimality within S only. Allocation or I/O failures must remain visible.

Ideal log loss is the sum of negative base-two logarithms of probabilities
assigned to observed bits. Actual container length additionally includes
headers and the residual from finite-precision coding, termination, padding
and stored blocks. A predictor improvement is useful only when it repays
those costs. Empty input has zero prediction loss but nonzero framing cost.

## Finite precision and resource limits

Real-arithmetic expert-mixture bounds do not automatically apply to rounded
weights, hashed contexts, count halving or shared mutable learning state.
The adaptive implementation normalizes weights with explicit integer rules;
its acceptance evidence is the actual reversible container and measured
resource use. See [ADAPTIVE.md](ADAPTIVE.md) for the implemented equations.

Every trial must fit the declared memory policy. Sequential trials bound live
model memory but do not bound total search time. Temporary input and output
storage is accounted separately from predictor memory.

## Algorithm sources

The expert-sequence and fixed-share formulation is described by Wouter Koolen
and Steven de Rooij in [Combining Expert Advice Efficiently](https://arxiv.org/abs/0802.2015).
The distinction between shared-history and separately learning experts is
described by Wouter Koolen and Tim van Erven in
[Switching between Hidden Markov Models using Fixed Share](https://arxiv.org/abs/1008.4532).
These sources explain the mathematical constructions; they do not establish
a compression bound for this finite-precision implementation.
