#!/usr/bin/env python3
"""Check the research mathematics; this is not a codec or a codec dependency.

Authored with Codex / GPT-6, 2026-09-14.
Run with Python 3 and SymPy: python3 -B research/verify_byte_only_math.py
Exact finite enumeration supplements the proofs; it does not prove all inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import platform
from decimal import Decimal, localcontext
from fractions import Fraction as F
from functools import cache
from pathlib import Path

import sympy as sp


class VerificationError(ValueError):
    """An identity, invariant, or input contract failed."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def symbolic_checks() -> list[str]:
    x, w, bit = sp.symbols("x w bit", real=True)
    require(sp.expand((x + 1) ** 2) == x * x + 2 * x + 1, "CAS canary")
    p = 1 / (1 + sp.exp(-w * x))
    loss = (sp.log(1 + sp.exp(w * x)) - bit * w * x) / sp.log(2)
    require(
        sp.simplify(sp.diff(loss, w) - (p - bit) * x / sp.log(2)) == 0,
        "logistic gradient",
    )
    require(
        sp.simplify(sp.diff(loss, w, 2) - p * (1 - p) * x**2 / sp.log(2)) == 0,
        "logistic Hessian",
    )
    eta, theta, old, variance = sp.symbols("eta theta old variance", positive=True)
    r = sp.symbols("r", integer=True, nonnegative=True)
    mean = theta + (1 - eta) ** r * (old - theta)
    require(
        sp.simplify((1 - eta) * mean + eta * theta - mean.subs(r, r + 1)) == 0,
        "adaptation recurrence",
    )
    equilibrium = eta * theta * (1 - theta) / (2 - eta)
    require(
        sp.simplify(
            ((1 - eta) ** 2 * variance + eta**2 * theta * (1 - theta) - variance).subs(
                variance, equilibrium
            )
        )
        == 0,
        "stationary estimator variance",
    )
    alpha, switches, transitions = sp.symbols(
        "alpha switches transitions", positive=True
    )
    penalty = -switches * sp.log(alpha) - (transitions - switches) * sp.log(1 - alpha)
    require(
        sp.simplify(sp.diff(penalty, alpha).subs(alpha, switches / transitions)) == 0,
        "interior switch-rate optimum",
    )
    l0, h0, h1, d0, d1, gain = sp.symbols("l0 h0 h1 d0 d1 gain")
    ell0 = l0 - h0 - d0
    l1 = h1 + ell0 - gain + d1
    require(
        sp.expand(l0 - l1 - (gain - (h1 - h0) - (d1 - d0))) == 0,
        "complete-container gain accounting",
    )

    def log2(value: sp.Expr) -> sp.Expr:
        return sp.log(value) / sp.log(2)

    ce0 = ce1 = mutual = kl0 = kl1 = sp.Integer(0)
    for context, conditional in enumerate((sp.Rational(1, 4), sp.Rational(3, 4))):
        estimate = (sp.Rational(1, 3), sp.Rational(4, 5))[context]
        for outcome in (0, 1):
            truth = conditional if outcome else 1 - conditional
            joint = truth / 2
            old_q = sp.Rational(2, 5) if outcome else sp.Rational(3, 5)
            new_q = estimate if outcome else 1 - estimate
            ce0 -= joint * log2(old_q)
            ce1 -= joint * log2(new_q)
            mutual += joint * log2(truth / sp.Rational(1, 2))
            kl0 += joint * log2(sp.Rational(1, 2) / old_q)
            kl1 += joint * log2(truth / new_q)
    require(
        sp.simplify(sp.expand_log(ce0 - ce1 - mutual - kl0 + kl1, force=True)) == 0,
        "conditional information decomposition example",
    )
    return [
        "CAS canary",
        "logistic gradient",
        "logistic Hessian",
        "adaptation recurrence",
        "stationary estimator variance",
        "interior switch-rate optimum",
        "complete-container gain accounting",
        "conditional information decomposition example",
    ]


def probabilities(prefix: tuple[int, ...], count: int) -> tuple[F, ...]:
    """Three deterministic causal experts, all using the same decoded history."""
    slow = F(1 + sum(prefix), 2 + len(prefix))
    fast = F(1, 2)
    for bit in prefix:
        fast = (fast + bit) / 2
    lag = F(1, 2) if len(prefix) < 2 else F(1 + 2 * prefix[-2], 4)
    return (slow, fast, lag)[:count]


def transition(a: int, b: int, count: int, alpha: F) -> F:
    return 1 - alpha if a == b else alpha / (count - 1)


def forward(bits: tuple[int, ...], count: int, alpha: F) -> F:
    require(count in (2, 3) and 0 <= alpha < 1, "invalid switching configuration")
    weights = [F(1, count)] * count
    probability = F(1)
    for t, bit in enumerate(bits):
        predictions = probabilities(bits[:t], count)
        likelihoods = [p if bit else 1 - p for p in predictions]
        q = sum(w * p for w, p in zip(weights, likelihoods))
        posterior = [w * p / q for w, p in zip(weights, likelihoods)]
        weights = [(1 - alpha) * v + alpha * (1 - v) / (count - 1) for v in posterior]
        require(sum(weights) == 1 and q > 0, "forward normalization")
        probability *= q
    return probability


def path_joint(bits: tuple[int, ...], path: tuple[int, ...], count: int, alpha: F) -> F:
    value = F(1, count)
    for t, (bit, expert) in enumerate(zip(bits, path)):
        if t:
            value *= transition(path[t - 1], expert, count, alpha)
        p = probabilities(bits[:t], count)[expert]
        value *= p if bit else 1 - p
    return value


def switching_checks() -> dict[str, int]:
    sequences = paths = normalizations = 0
    for count, maximum in ((2, 6), (3, 4)):
        for alpha in (F(1, 5), F(1, 2)):
            for length in range(1, maximum + 1):
                total = F(0)
                for bits in itertools.product((0, 1), repeat=length):
                    q = forward(bits, count, alpha)
                    explicit = F(0)
                    for route in itertools.product(range(count), repeat=length):
                        joint = path_joint(bits, route, count, alpha)
                        require(q >= joint, "switching comparator dominance")
                        explicit += joint
                        paths += 1
                    require(explicit == q, "forward vs enumerated routes")
                    total += q
                    sequences += 1
                require(total == 1, "distribution over all binary sequences")
                normalizations += 1
    for bits in itertools.product((0, 1), repeat=5):
        q = forward(bits, 3, F(0))
        static = sum(path_joint(bits, (k,) * len(bits), 3, F(0)) for k in range(3))
        require(q == static, "zero-switch Bayesian mixture")
    return {
        "sequences": sequences,
        "route_comparisons": paths,
        "normalizations": normalizations,
        "static_mixture_sequences": 32,
    }


@cache
def kt(bits: tuple[int, ...]) -> F:
    counts = [0, 0]
    probability = F(1)
    for bit in bits:
        probability *= F(2 * counts[bit] + 1, 2 * sum(counts) + 2)
        counts[bit] += 1
    return probability


@cache
def partition_mixture(bits: tuple[int, ...]) -> F:
    if len(bits) == 1:
        return kt(bits)
    middle = len(bits) // 2
    return (
        kt(bits) + partition_mixture(bits[:middle]) * partition_mixture(bits[middle:])
    ) / 2


@cache
def dyadic_partitions(length: int) -> tuple[tuple[F, tuple[int, ...]], ...]:
    if length == 1:
        return ((F(1), (1,)),)
    result = [(F(1, 2), (length,))]
    for left_weight, left in dyadic_partitions(length // 2):
        for right_weight, right in dyadic_partitions(length // 2):
            result.append((left_weight * right_weight / 2, left + right))
    return tuple(result)


def partition_checks() -> dict[str, int]:
    sequences = comparisons = 0
    for length in (1, 2, 4, 8):
        partitions = dyadic_partitions(length)
        require(sum(w for w, _ in partitions) == 1, "partition prior")
        total = F(0)
        for bits in itertools.product((0, 1), repeat=length):
            q = partition_mixture(bits)
            explicit = F(0)
            for weight, sizes in partitions:
                joint = weight
                start = 0
                for size in sizes:
                    joint *= kt(bits[start : start + size])
                    start += size
                require(joint <= q, "partition comparator dominance")
                explicit += joint
                comparisons += 1
            require(explicit == q, "recursive vs enumerated partitions")
            total += q
            sequences += 1
        require(total == 1, "partition source normalization")
    return {
        "sequences": sequences,
        "partition_comparisons": comparisons,
        "dyadic_partitions_at_length_8": len(dyadic_partitions(8)),
    }


def segment_bits(bits: tuple[int, ...]) -> int:
    """Toy prefix code: gamma length, mode bit, then a run bit or literal bits."""
    require(bool(bits), "empty segment")
    length = len(bits)
    payload = 1 if len(set(bits)) == 1 else length
    return 2 * length.bit_length() - 1 + 1 + payload


def segmentation_checks() -> dict[str, int]:
    sequences = partitions = 0
    for length in range(1, 9):
        for bits in itertools.product((0, 1), repeat=length):
            costs = [0] + [10**9] * length
            for end in range(1, length + 1):
                costs[end] = min(
                    costs[start] + segment_bits(bits[start:end]) for start in range(end)
                )
            brute = 10**9
            for cuts in itertools.product((0, 1), repeat=length - 1):
                bounds = [0] + [i + 1 for i, cut in enumerate(cuts) if cut] + [length]
                cost = sum(segment_bits(bits[a:b]) for a, b in zip(bounds, bounds[1:]))
                brute = min(brute, cost)
                partitions += 1
            require(costs[-1] == brute, "independent-segment dynamic program")
            sequences += 1
    return {"sequences": sequences, "partitions": partitions}


def quantization_checks() -> dict[str, int]:
    checked = 0
    epsilon = F(1, 16)
    for frequency_total in (16, 256):
        delta = F(1, 2 * frequency_total)
        for numerator in range(16, 241):
            p = F(numerator, 256)
            scaled = frequency_total * p + F(1, 2)
            frequency = scaled.numerator // scaled.denominator
            quantized = F(frequency, frequency_total)
            require(0 < frequency < frequency_total, "frequency range")
            require(abs(quantized - p) <= delta, "rounding distance")
            for bit in (0, 1):
                ideal = p if bit else 1 - p
                actual = quantized if bit else 1 - quantized
                require(
                    actual >= ideal * (1 - delta / epsilon),
                    "log-loss perturbation bound",
                )
                checked += 1
    return {"outcome_comparisons": checked}


def integer_weight_checks() -> dict[str, int]:
    """Compare integer epoch updates to a rational transition-matrix calculation."""
    updates = sequences = 0
    frequency_total, alpha = 8, F(1, 5)
    for count in (2, 3):
        denominator = alpha.denominator * (count - 1)
        for horizon in (2, 4):
            total_probability = F(0)
            for bits in itertools.product((0, 1), repeat=8):
                integers = [1] * count
                normalized = [F(1, count)] * count
                likelihood = F(1)
                for t, bit in enumerate(bits):
                    if t % horizon == 0:
                        integers, normalized = [1] * count, [F(1, count)] * count
                    frequencies = []
                    for p in probabilities(bits[:t], count):
                        rounded = frequency_total * p + F(1, 2)
                        f1 = max(
                            1,
                            min(
                                frequency_total - 1,
                                rounded.numerator // rounded.denominator,
                            ),
                        )
                        frequencies.append(f1 if bit else frequency_total - f1)
                    weighted = [w * f for w, f in zip(integers, frequencies)]
                    q = F(sum(weighted), frequency_total * sum(integers))
                    q_matrix = sum(
                        w * F(f, frequency_total)
                        for w, f in zip(normalized, frequencies)
                    )
                    require(q == q_matrix, "integer vs rational mixture prediction")
                    posterior = [
                        w * F(f, frequency_total) / q_matrix
                        for w, f in zip(normalized, frequencies)
                    ]
                    normalized = [
                        sum(
                            posterior[j] * transition(j, k, count, alpha)
                            for j in range(count)
                        )
                        for k in range(count)
                    ]
                    integers = [
                        (alpha.denominator - alpha.numerator) * (count - 1) * m
                        + alpha.numerator * (sum(weighted) - m)
                        for m in weighted
                    ]
                    require(
                        [F(w, sum(integers)) for w in integers] == normalized,
                        "integer vs rational posterior",
                    )
                    epoch_steps = t % horizon + 1
                    require(
                        sum(integers)
                        <= count * (frequency_total * denominator) ** epoch_steps,
                        "integer epoch storage bound",
                    )
                    likelihood *= q
                    updates += 1
                total_probability += likelihood
                sequences += 1
            require(total_probability == 1, "integer epoch source normalization")
    return {"sequences": sequences, "weight_updates": updates, "epoch_lengths": [2, 4]}


def counterexamples_and_failures() -> dict[str, object]:
    def selected_only(
        prefix: tuple[int, ...], route: tuple[int, ...], expert: int
    ) -> F:
        seen = [bit for bit, chosen in zip(prefix, route) if chosen == expert]
        return F(sum(seen) + 1, len(seen) + 2)

    first = selected_only((0, 1), (0, 0), 0)
    second = selected_only((0, 1), (0, 1), 0)
    require(first != second, "private learning histories must not be merged")
    continued, reset = kt((0, 0)), kt((0,)) * kt((0,))
    require(continued != reset, "reset costs cannot represent continuing state")
    rejected = 0
    for invalid in (-1, F(1), F(2)):
        try:
            forward((0,), 2, F(invalid))
        except VerificationError:
            rejected += 1
        else:
            raise VerificationError("invalid switching parameter was accepted")
    try:
        require(F(1, 2) == F(1, 3), "intentional incorrect identity")
    except VerificationError:
        rejected += 1
    else:
        raise VerificationError("false identity did not fail")
    return {
        "selected_only_predictions": [str(first), str(second)],
        "continuing_vs_reset_likelihood": [str(continued), str(reset)],
        "deliberately_invalid_cases_rejected": rejected,
    }


def target_numbers(baseline: dict) -> dict[str, object]:
    size = baseline["derived"]["reference_complete_bytes"]
    input_bytes = baseline["session"]["fixture"]["bytes"]
    result: dict[str, object] = {"baseline_bytes": size, "input_bytes": input_bytes}
    for label, fraction in (
        ("more_than_30_percent", F(3, 10)),
        ("double_ratio", F(1, 2)),
    ):
        threshold = ((7 * size - 1) // 10) if fraction == F(3, 10) else size // 2
        require(
            threshold
            == (
                (
                    baseline["derived"]["strictly_more_than_30_percent_reduction"]
                    if fraction == F(3, 10)
                    else baseline["derived"]["at_least_double_compression_ratio"]
                )["largest_passing_integer_bytes"]
            ),
            "baseline threshold agreement",
        )
        per_bit = fraction * size / input_bytes
        exact_multiplier = sp.Pow(
            2, sp.Rational(per_bit.numerator, per_bit.denominator)
        )
        with localcontext() as context:
            context.prec = 85
            exponent = Decimal(per_bit.numerator) / Decimal(per_bit.denominator)
            decimal_value = (Decimal(2).ln() * exponent).exp()
            symbolic_value = Decimal(str(sp.N(exact_multiplier, 75)))
            require(
                abs(decimal_value - symbolic_value) < Decimal("1e-70"),
                "independent power evaluation",
            )
        result[label] = {
            "maximum_container_bytes": threshold,
            "minimum_saved_bytes": size - threshold,
            "ideal_gain_bits_per_input_bit": str(per_bit),
            "ideal_gain_bits_per_input_byte": str(8 * per_bit),
            "equal_overhead_geometric_probability_multiplier": str(
                sp.N(exact_multiplier, 30)
            ),
        }
    toy = sp.log(3) / (2 * sp.log(2))
    with localcontext() as context:
        context.prec = 85
        independent = Decimal(3).ln() / (2 * Decimal(2).ln())
        require(
            abs(independent - Decimal(str(sp.N(toy, 75)))) < Decimal("1e-70"),
            "Bernoulli KL evaluation",
        )
    result["retention_toy_KL_bits_per_bit"] = str(sp.N(toy, 30))
    result["state_description_break_even_bytes_strict"] = (2 * 1024 * 8) * 10 + 1
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, help="write the verification JSON to this path"
    )
    args = parser.parse_args()
    here = Path(__file__).resolve()
    baseline_path = here.with_name("byte-only-baseline.json")
    baseline = json.loads(baseline_path.read_text())
    record = {
        "schema": "cadapt.research.math_verification.v1",
        "schema_version": "1.0.0",
        "generated_by": {"tool": "Codex", "model": "GPT-6"},
        "python": platform.python_version(),
        "sympy": sp.__version__,
        "verifier_sha256": hashlib.sha256(here.read_bytes()).hexdigest(),
        "baseline_sha256": hashlib.sha256(baseline_path.read_bytes()).hexdigest(),
        "constraints_sha256": baseline["constraints"]["sha256"],
        "checks": {
            "symbolic": symbolic_checks(),
            "switching": switching_checks(),
            "dyadic_partition_mixture": partition_checks(),
            "independent_segmentation": segmentation_checks(),
            "quantization": quantization_checks(),
            "integer_epoch_weights": integer_weight_checks(),
            "counterexamples_and_failures": counterexamples_and_failures(),
        },
        "numbers": target_numbers(baseline),
        "status": "passed",
        "limitations": [
            "Finite checks supplement written proofs; they are not universal proofs.",
            "No compression improvement or production encoder/decoder was implemented or benchmarked.",
            "SymPy is a research verifier dependency, not a codec dependency.",
        ],
    }
    text = json.dumps(record, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
