#!/usr/bin/env python3
"""Statistical analysis of the LTCB main table, with a local codec placed in it.

Source: Claude Opus 5 via Claude Code. Input is ltcb-main-table.csv (published
figures, see ltcb-tables.md) and optionally ltcb-local-entries.csv. Every
number is computed here from those files with the standard library; the
methods are small enough to read: average-rank Spearman, ordinary least
squares by normal equations, quartiles by the inclusive method, and the
speed/ratio frontier from plot_ltcb_phases. A prediction is always labelled as
one and carries its interval; a measurement is never replaced by a prediction.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

import plot_ltcb_phases as ph
import plot_ltcb_tables as ltcb

LADDER_TARGETS = (3.0, 2.0, 1.5, 1.3, 1.2, 1.1, 1.0, 0.9, 0.8, 0.75)
EFFICIENT_GAP = 0.05  # bits per byte above the frontier that still counts as "on it"


class AnalysisError(ValueError):
    """The data cannot support the requested statistic."""


# --- statistics -----------------------------------------------------------------------------

def _ranks(values: list[float]) -> list[float]:
    order = sorted(range(len(values)), key=lambda i: values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        average = (i + j) / 2 + 1
        for k in range(i, j + 1):
            ranks[order[k]] = average
        i = j + 1
    return ranks


def pearson(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    if n < 2:
        return math.nan
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return math.nan
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / math.sqrt(sxx * syy)


def spearman(xs: list[float], ys: list[float]) -> float:
    return pearson(_ranks(list(xs)), _ranks(list(ys)))


def quartiles(values: list[float]) -> tuple[float, float, float]:
    data = sorted(values)
    if len(data) < 2:
        return (data[0], data[0], data[0])
    q1, q2, q3 = statistics.quantiles(data, n=4, method="inclusive")
    return (q1, q2, q3)


@dataclass(frozen=True)
class Fit:
    coef: list[float]
    se: list[float]
    r2: float
    resid_sd: float
    n: int


def _invert(matrix: list[list[float]]) -> list[list[float]]:
    k = len(matrix)
    a = [row[:] + [1.0 if i == j else 0.0 for j in range(k)] for i, row in enumerate(matrix)]
    for col in range(k):
        pivot = max(range(col, k), key=lambda r: abs(a[r][col]))
        if abs(a[pivot][col]) < 1e-12:
            raise AnalysisError("singular design matrix")
        a[col], a[pivot] = a[pivot], a[col]
        scale = a[col][col]
        a[col] = [v / scale for v in a[col]]
        for r in range(k):
            if r != col and a[r][col] != 0.0:
                factor = a[r][col]
                a[r] = [rv - factor * cv for rv, cv in zip(a[r], a[col])]
    return [row[k:] for row in a]


def ols(y: list[float], X: list[list[float]]) -> Fit:
    """Least squares of y on the rows of X (include a 1.0 column for an intercept)."""
    n, k = len(y), len(X[0])
    if n <= k:
        raise AnalysisError("not enough observations")
    xtx = [[sum(row[i] * row[j] for row in X) for j in range(k)] for i in range(k)]
    xty = [sum(row[i] * yi for row, yi in zip(X, y)) for i in range(k)]
    inv = _invert(xtx)
    coef = [sum(inv[i][j] * xty[j] for j in range(k)) for i in range(k)]
    fitted = [sum(c * v for c, v in zip(coef, row)) for row in X]
    ssr = sum((yi - fi) ** 2 for yi, fi in zip(y, fitted))
    mean = sum(y) / n
    sst = sum((yi - mean) ** 2 for yi in y)
    resid_sd = math.sqrt(ssr / (n - k))
    se = [resid_sd * math.sqrt(max(inv[i][i], 0.0)) for i in range(k)]
    r2 = 1.0 if sst == 0 else 1.0 - ssr / sst
    return Fit(coef, se, r2, resid_sd, n)


# --- derived quantities ---------------------------------------------------------------------

@dataclass(frozen=True)
class ScalingFit:
    """ln(enwik9 bpb) = intercept + slope * ln(enwik8 bpb), fitted on the published pairs."""
    slope: float
    intercept: float
    se_slope: float
    r2: float
    resid_sd: float
    n: int

    def factor_at(self, bpb8: float) -> float:
        return self.predict(bpb8)[0] / bpb8

    def predict(self, bpb8: float) -> tuple[float, float, float]:
        """Point prediction and an approximate 95 % prediction interval (residual spread)."""
        centre = self.intercept + self.slope * math.log(bpb8)
        half = 1.96 * self.resid_sd * math.sqrt(1 + 1 / self.n)
        return (math.exp(centre), math.exp(centre - half), math.exp(centre + half))


def scaling_fit(rows: list[ltcb.Entry]) -> ScalingFit:
    pairs = [(r.enwik8_bpb, r.enwik9_bpb) for r in rows if r.enwik8_bpb is not None]
    fit = ols([math.log(b9) for _, b9 in pairs], [[1.0, math.log(b8)] for b8, _ in pairs])
    return ScalingFit(fit.coef[1], fit.coef[0], fit.se[1], fit.r2, fit.resid_sd, fit.n)


@dataclass(frozen=True)
class LadderStep:
    target: float
    program: str | None
    speed: float | None
    bpb: float | None
    time_multiplier: float | None


def cost_ladder(rows: list[ltcb.Entry], key: str, targets=LADDER_TARGETS) -> list[LadderStep]:
    """For each target ratio, the fastest frontier entry reaching it, and its time cost
    relative to the fastest frontier entry of all."""
    points = ph.frontier_points(rows, key)
    steps = []
    for target in targets:
        reach = next((p for p in points if p.bpb <= target), None)
        if reach is None:
            steps.append(LadderStep(target, None, None, None, None))
        else:
            steps.append(LadderStep(target, reach.entry.program, reach.speed, reach.bpb,
                                    reach.speed / points[0].speed))
    return steps


@dataclass(frozen=True)
class Gap:
    program: str
    family: str
    speed: float
    bpb: float
    gap: float


def frontier_gaps(rows: list[ltcb.Entry], key: str) -> list[Gap]:
    points = ph.frontier_points(rows, key)
    gaps = []
    for r in rows:
        speed = getattr(r, key)
        if speed is None:
            continue
        reach = ph.frontier_bpb_at(points, speed)
        gaps.append(Gap(r.program, r.family, speed, r.enwik9_bpb, r.enwik9_bpb - reach))
    return gaps


@dataclass(frozen=True)
class Neighbour:
    program: str
    family: str
    distance: float
    comp_ns_per_byte: float
    mem_mb: float
    bpb: float


def _features(comp: float, mem: float, bpb: float) -> list[float]:
    return [math.log10(comp), math.log10(mem), bpb]


def neighbours(rows: list[ltcb.Entry], local: ltcb.LocalEntry, k: int = 5) -> list[Neighbour]:
    """Published entries nearest to a local one in (log speed, log memory, ratio), each
    axis standardised by its published spread. The ratio axis is enwik9 when the local
    entry has it, else enwik8 on both sides."""
    use_enwik9 = local.enwik9_bpb is not None
    ratio_of = (lambda r: r.enwik9_bpb) if use_enwik9 else (lambda r: r.enwik8_bpb)
    pool = [r for r in rows if r.comp_ns_per_byte and r.mem_mb > 0 and ratio_of(r) is not None]
    if local.comp_ns_per_byte is None or len(pool) < 2:
        raise AnalysisError("neighbours need a timed local entry and a timed table")
    table = [_features(r.comp_ns_per_byte, r.mem_mb, ratio_of(r)) for r in pool]
    spread = [statistics.pstdev(col) or 1.0 for col in zip(*table)]
    target = _features(local.comp_ns_per_byte, local.mem_mb,
                       local.enwik9_bpb if use_enwik9 else local.enwik8_bpb)
    scored = []
    for r, feats in zip(pool, table):
        distance = math.sqrt(sum(((f - t) / s) ** 2 for f, t, s in zip(feats, target, spread)))
        scored.append(Neighbour(r.program, r.family, distance, r.comp_ns_per_byte, r.mem_mb,
                                ratio_of(r)))
    return sorted(scored, key=lambda n: n.distance)[:k]


@dataclass(frozen=True)
class Rate:
    """Size removed per unit of compressor time. For enwik9 the page's ns per byte is
    numerically the run time in seconds, so MB/s = (10^9 - bytes) / 10^6 / (ns per byte)."""
    program: str
    family: str
    ratio: float
    throughput_mb_per_s: float
    saved_mb_per_s: float


def savings_rates(rows: list[ltcb.Entry], key: str) -> list[Rate]:
    rates = []
    for r in rows:
        speed = getattr(r, key)
        if speed is None:
            continue
        saved = (ltcb.ENWIK9_BYTES - r.enwik9_bytes) / 1e6
        rates.append(Rate(r.program, r.family, r.enwik9_bytes / ltcb.ENWIK9_BYTES,
                          1000.0 / speed, saved / speed))
    return sorted(rates, key=lambda x: -x.saved_mb_per_s)


def local_rate(entry: ltcb.LocalEntry, key: str) -> Rate | None:
    speed = getattr(entry, key)
    if speed is None or entry.enwik9_bytes is None:
        return None
    saved = (ltcb.ENWIK9_BYTES - entry.enwik9_bytes) / 1e6
    return Rate(entry.program, entry.family, entry.enwik9_bytes / ltcb.ENWIK9_BYTES,
                1000.0 / speed, saved / speed)


@dataclass(frozen=True)
class TotalRate:
    """Size removed per second of total (compress + decompress) time, and per GB of memory."""
    program: str
    family: str
    ratio: float
    total_ns_per_byte: float
    mem_mb: float
    saved_mb_per_s: float
    saved_mb_per_s_per_gb: float


def total_rates(rows: list[ltcb.Entry]) -> list[TotalRate]:
    rates = []
    for r in rows:
        if r.comp_ns_per_byte is None or r.decomp_ns_per_byte is None or r.mem_mb <= 0:
            continue
        total = r.comp_ns_per_byte + r.decomp_ns_per_byte
        saved = (ltcb.ENWIK9_BYTES - r.enwik9_bytes) / 1e6 / total
        rates.append(TotalRate(r.program, r.family, r.enwik9_bytes / ltcb.ENWIK9_BYTES, total, r.mem_mb,
                               saved, saved / (r.mem_mb / 1000.0)))
    return sorted(rates, key=lambda x: -x.saved_mb_per_s)


def weighted_cost(entry, alpha: float, beta: float) -> float:
    """bpb x (comp + decomp ns/B)^alpha x (mem MB)^beta: the plain ratio at alpha = beta = 0,
    a time-and-memory-weighted cost otherwise. Lower is better."""
    total = entry.comp_ns_per_byte + entry.decomp_ns_per_byte
    return entry.enwik9_bpb * total ** alpha * entry.mem_mb ** beta


def _costable(rows):
    return [r for r in rows if r.comp_ns_per_byte is not None and r.decomp_ns_per_byte is not None
            and r.mem_mb > 0 and r.enwik9_bpb is not None]


def weighted_ranking(rows: list[ltcb.Entry], alpha: float, beta: float, k: int | None = None):
    ranked = sorted(_costable(rows), key=lambda r: weighted_cost(r, alpha, beta))
    return ranked if k is None else ranked[:k]


@dataclass(frozen=True)
class Preference:
    alpha: float
    beta: float
    program: str
    family: str
    cost: float
    bpb: float


ALPHAS = (0.0, 0.05, 0.1, 0.2, 0.35, 0.5)
BETAS = (0.0, 0.05, 0.1, 0.2, 0.35)


def preference_map(rows: list[ltcb.Entry], alphas=ALPHAS, betas=BETAS) -> list[Preference]:
    """The winning entry for every (alpha, beta) weight pair."""
    out = []
    for alpha in alphas:
        for beta in betas:
            best = weighted_ranking(rows, alpha, beta, 1)
            if best:
                out.append(Preference(alpha, beta, best[0].program, best[0].family,
                                      weighted_cost(best[0], alpha, beta), best[0].enwik9_bpb))
    return out


# --- the whole analysis ---------------------------------------------------------------------

def _corr(pairs: list[tuple[float, float]]) -> dict:
    xs, ys = [p[0] for p in pairs], [p[1] for p in pairs]
    return {"spearman": spearman(xs, ys), "pearson": pearson(xs, ys), "n": len(pairs)}


def _fit_dict(fit: Fit, names: list[str]) -> dict:
    return {"n": fit.n, "r2": fit.r2, "resid_sd": fit.resid_sd,
            "coef": dict(zip(names, fit.coef)), "se": dict(zip(names, fit.se))}


def _elasticity(sample: list[ltcb.Entry]) -> dict:
    """bpb on log10 time and log10 memory; states why when the sample cannot support it."""
    try:
        fit = ols([r.enwik9_bpb for r in sample],
                  [[1.0, math.log10(r.comp_ns_per_byte), math.log10(r.mem_mb)] for r in sample])
    except AnalysisError as error:
        return {"n": len(sample), "error": str(error)}
    return _fit_dict(fit, ["intercept", "log10_comp", "log10_mem"])


def analyze(rows: list[ltcb.Entry], local: list[ltcb.LocalEntry] = ()) -> dict:
    timed = [r for r in rows if r.comp_ns_per_byte is not None]
    both = [r for r in rows if r.comp_ns_per_byte is not None and r.decomp_ns_per_byte is not None]
    with_e8 = [r for r in rows if r.enwik8_bytes is not None]
    families = {}
    for family in ltcb.FAMILIES:
        members = [r for r in rows if r.family == family]
        if not members:
            continue
        q1, q2, q3 = quartiles([r.enwik9_bpb for r in members])
        timed_members = [r for r in members if r.comp_ns_per_byte is not None]
        sym = [r.decomp_ns_per_byte / r.comp_ns_per_byte for r in members
               if r.comp_ns_per_byte and r.decomp_ns_per_byte]
        families[family] = {
            "n": len(members), "bpb_q1": q1, "bpb_median": q2, "bpb_q3": q3,
            "comp_median": statistics.median([r.comp_ns_per_byte for r in timed_members]) if timed_members else None,
            "mem_median": statistics.median([r.mem_mb for r in members]),
            "decomp_over_comp_median": statistics.median(sym) if sym else None,
            "best": min(members, key=lambda r: r.enwik9_bpb).program,
        }
    correlations = {
        "enwik9_bpb_vs_log_comp": _corr([(math.log10(r.comp_ns_per_byte), r.enwik9_bpb) for r in timed]),
        "enwik9_bpb_vs_log_mem": _corr([(math.log10(r.mem_mb), r.enwik9_bpb) for r in rows if r.mem_mb > 0]),
        "enwik9_bpb_vs_log_decomp": _corr([(math.log10(r.decomp_ns_per_byte), r.enwik9_bpb)
                                           for r in rows if r.decomp_ns_per_byte]),
        "log_mem_vs_log_comp": _corr([(math.log10(r.comp_ns_per_byte), math.log10(r.mem_mb))
                                      for r in timed if r.mem_mb > 0]),
        "enwik8_bpb_vs_enwik9_bpb": _corr([(r.enwik8_bpb, r.enwik9_bpb) for r in with_e8]),
    }
    scaling = scaling_fit(rows)
    modelling = [r for r in timed if r.family in ("CM", "Neural") and r.mem_mb > 0]
    elasticity = {
        "cm_and_neural": _elasticity(modelling),
        "all_timed": _elasticity([r for r in timed if r.mem_mb > 0]),
    }
    efficiency = {}
    for name, key in ph.AXES:
        gaps = frontier_gaps(rows, key)
        efficiency[name] = {
            "frontier_points": sum(1 for g in gaps if g.gap == 0),
            "within_0.05": sum(1 for g in gaps if g.gap <= EFFICIENT_GAP),
            "median_gap": statistics.median([g.gap for g in gaps]),
            "worst": [g.__dict__ for g in sorted(gaps, key=lambda g: -g.gap)[:5]],
        }
    ladder = {name: [s.__dict__ for s in cost_ladder(rows, key)] for name, key in ph.AXES}
    shares = [(r.program, r.decompressor_zip_bytes / r.enwik9_bytes) for r in rows]
    decompressor = {
        "median_share": statistics.median([s for _, s in shares]),
        "over_1_percent": [{"program": p, "share": s} for p, s in shares if s > 0.01],
    }
    symmetry = {}
    for family in ltcb.FAMILIES:
        ratios = [r.decomp_ns_per_byte / r.comp_ns_per_byte for r in both if r.family == family]
        if ratios:
            q1, q2, q3 = quartiles(ratios)
            symmetry[family] = {"n": len(ratios), "q1": q1, "median": q2, "q3": q3}
    rates = {}
    for name, key in ph.AXES:
        ranked = savings_rates(rows, key)
        rates[name] = {
            "top": [x.__dict__ for x in ranked[:10]],
            "family_median": {f: statistics.median([x.saved_mb_per_s for x in ranked if x.family == f])
                              for f in ltcb.FAMILIES if any(x.family == f for x in ranked)},
            "best_ratio_rate": next((x.__dict__ for x in sorted(ranked, key=lambda x: x.ratio)), None),
        }
    totals = total_rates(rows)
    grid = preference_map(rows)
    efficiency_index = {
        "definition": "cost = bpb x (comp + decomp ns/B)^alpha x (mem MB)^beta; lower is better",
        "total_time_top": [x.__dict__ for x in totals[:10]],
        "per_gb_top": [x.__dict__ for x in sorted(totals, key=lambda x: -x.saved_mb_per_s_per_gb)[:10]],
        "alphas": list(ALPHAS), "betas": list(BETAS),
        "preference_map": [g.__dict__ for g in grid],
        "distinct_winners": sorted({g.program for g in grid}),
        "rankings": {f"{a}/{b}": [{"program": r.program, "family": r.family,
                                    "cost": weighted_cost(r, a, b), "bpb": r.enwik9_bpb}
                                   for r in weighted_ranking(rows, a, b, 5)]
                     for a, b in ((0.1, 0.05), (0.2, 0.1), (0.35, 0.2))},
    }
    placed = []
    for entry in local:
        item = {"label": entry.label, "program": entry.program, "family": entry.family,
                "enwik8_bpb": entry.enwik8_bpb, "enwik9_bpb": entry.enwik9_bpb,
                "measured_enwik9": entry.enwik9_bpb is not None}
        if entry.enwik8_bytes is not None:
            point, low, high = scaling.predict(entry.enwik8_bpb)
            item.update({"enwik8_rank": ltcb.enwik8_rank(rows, entry.enwik8_bytes),
                         "enwik8_rank_of": len(with_e8),
                         "predicted_enwik9_bpb": point,
                         "predicted_enwik9_interval": [low, high],
                         "predicted_total_rank": ltcb.total_rank(
                             rows, int(point * ltcb.ENWIK9_BYTES / 8) + entry.decompressor_zip_bytes)})
        if entry.enwik9_plus_prog_bytes is not None:
            item["total_rank"] = ltcb.total_rank(rows, entry.enwik9_plus_prog_bytes)
            item["total_rank_of"] = len(rows)
        for name, key in ph.AXES:
            rate = local_rate(entry, key)
            if rate is not None:
                ranked = savings_rates(rows, key)
                item[f"{name}_saved_mb_per_s"] = rate.saved_mb_per_s
                item[f"{name}_saved_rank"] = 1 + sum(1 for x in ranked if x.saved_mb_per_s > rate.saved_mb_per_s)
        if entry.enwik9_bytes is not None and entry.comp_ns_per_byte and entry.decomp_ns_per_byte:
            total = entry.comp_ns_per_byte + entry.decomp_ns_per_byte
            saved = (ltcb.ENWIK9_BYTES - entry.enwik9_bytes) / 1e6 / total
            item["total_time_saved_mb_per_s"] = saved
            item["total_time_saved_rank"] = 1 + sum(1 for x in totals if x.saved_mb_per_s > saved)
            item["per_gb_saved_mb_per_s"] = saved / (entry.mem_mb / 1000.0)
            item["per_gb_saved_rank"] = 1 + sum(1 for x in totals if x.saved_mb_per_s_per_gb > saved / (entry.mem_mb / 1000.0))
            item["weighted_ranks"] = {}
            for a, b in ((0.0, 0.0), (0.1, 0.05), (0.2, 0.1), (0.35, 0.2)):
                cost = weighted_cost(entry, a, b)
                item["weighted_ranks"][f"{a}/{b}"] = {
                    "cost": cost, "rank": 1 + sum(1 for r in _costable(rows) if weighted_cost(r, a, b) < cost)}
        if entry.comp_ns_per_byte is not None:
            points = ph.frontier_points(rows, "comp_ns_per_byte")
            ratio = entry.enwik9_bpb if entry.enwik9_bpb is not None else entry.enwik8_bpb
            reach = ph.frontier_bpb_at(points, entry.comp_ns_per_byte)
            item["frontier_gap"] = None if reach is None else ratio - reach
            item["frontier_gap_basis"] = "enwik9" if entry.enwik9_bpb is not None else "enwik8"
            item["neighbours"] = [n.__dict__ for n in neighbours(rows, entry)]
        placed.append(item)
    return {
        "entries": len(rows), "timed": len(timed), "with_enwik8": len(with_e8),
        "families": families, "correlations": correlations,
        "scaling": scaling.__dict__, "elasticity": elasticity, "efficiency": efficiency,
        "cost_ladder": ladder, "decompressor": decompressor, "symmetry": symmetry,
        "savings_rates": rates, "efficiency_index": efficiency_index, "local": placed,
    }


# --- report ---------------------------------------------------------------------------------

def _f(value, digits=3):
    if value is None:
        return "—"
    if isinstance(value, float) and math.isnan(value):
        return "n/a"
    return f"{value:,.{digits}f}" if isinstance(value, float) else f"{value:,}"


def report(analysis: dict, source: str = ltcb.DEFAULT_SOURCE) -> str:
    a = analysis
    out = ["---", "title: LTCB main table, statistical analysis", "date: 2026-09-14",
           "source_model: Claude Opus 5", "tool: Claude Code", "---", "",
           "# LTCB main table, statistical analysis", "",
           f"Computed by `analyze_ltcb.py` from `ltcb-main-table.csv` ({source}); "
           f"{a['entries']} entries, {a['timed']} timed, {a['with_enwik8']} with an enwik8 "
           "result. Ratios are bits per byte (bpb); times are the page's ns per byte.", ""]
    out += ["## Families", "", "| Family | n | bpb Q1 | median | Q3 | median comp ns/B | "
            "median mem MB | median decode/encode | best |", "|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    for family, f in a["families"].items():
        out.append(f"| {ltcb.FAMILY_LABELS[family]} | {f['n']} | {_f(f['bpb_q1'])} | "
                   f"{_f(f['bpb_median'])} | {_f(f['bpb_q3'])} | {_f(f['comp_median'], 0)} | "
                   f"{_f(f['mem_median'], 0)} | {_f(f['decomp_over_comp_median'])} | {f['best']} |")
    out += ["", "## Correlations", "", "| Pair | Spearman ρ | Pearson r | n |", "|---|---:|---:|---:|"]
    for name, c in a["correlations"].items():
        out.append(f"| {name.replace('_', ' ')} | {_f(c['spearman'])} | {_f(c['pearson'])} | {c['n']} |")
    s = a["scaling"]
    out += ["", "## enwik8 → enwik9 scaling", "",
            f"ln(enwik9 bpb) = {_f(s['intercept'])} + {_f(s['slope'])} · ln(enwik8 bpb) "
            f"(slope SE {_f(s['se_slope'])}, R² {_f(s['r2'])}, residual SD {_f(s['resid_sd'])} "
            f"in ln units, n = {s['n']}). A slope above 1 means the stronger a model is on "
            "enwik8, the more it gains from the ten times larger file.", ""]
    out += ["## Memory and time elasticity", "",
            "Ordinary least squares of enwik9 bpb on log10 compression time and log10 memory:", "",
            "| Sample | n | R² | per decade of time | per decade of memory |", "|---|---:|---:|---:|---:|"]
    for name, e in a["elasticity"].items():
        if "error" in e:
            out.append(f"| {name.replace('_', ' ')} | {e['n']} | not estimable: {e['error']} | | |")
            continue
        out.append(f"| {name.replace('_', ' ')} | {e['n']} | {_f(e['r2'])} | "
                   f"{_f(e['coef']['log10_comp'])} ± {_f(e['se']['log10_comp'])} | "
                   f"{_f(e['coef']['log10_mem'])} ± {_f(e['se']['log10_mem'])} |")
    out += ["", "## Cost of a bit", "",
            "The fastest frontier entry reaching each ratio, and its time relative to the fastest "
            "frontier entry of all:", ""]
    for axis, steps in a["cost_ladder"].items():
        out += [f"**{axis}**", "", "| target bpb | entry | ns/B | time × |", "|---:|---|---:|---:|"]
        for st in steps:
            out.append(f"| {st['target']} | {st['program'] or 'not reached'} | "
                       f"{_f(st['speed'], 0)} | {_f(st['time_multiplier'], 0)} |")
        out.append("")
    out += ["## Size removed per second", "",
            "For enwik9 the page's ns per byte equals the run time in seconds, so the size a "
            "compressor removes per second of its own time is (10⁹ − output) / time. The fast "
            "string matchers dominate; the best-ratio entries remove bytes thousands of times "
            "more slowly.", ""]
    for axis, rate in a["savings_rates"].items():
        out += [f"**{axis}**: top ten by MB removed per second", "",
                "| entry | family | ratio | MB/s processed | MB/s removed |", "|---|---|---:|---:|---:|"]
        for x in rate["top"]:
            out.append(f"| {x['program']} | {x['family']} | {_f(x['ratio'])} | "
                       f"{_f(x['throughput_mb_per_s'], 1)} | {_f(x['saved_mb_per_s'], 1)} |")
        best = rate["best_ratio_rate"]
        out += ["", "Family medians (MB removed per second): "
                + ", ".join(f"{ltcb.SHORT_LABELS[f]} {_f(v, 3)}" for f, v in rate["family_median"].items())
                + (f". The best-ratio entry, {best['program']}, removes {_f(best['saved_mb_per_s'], 4)} MB/s."
                   if best else "."), ""]
    ei = a["efficiency_index"]
    out += ["## Weighted efficiency", "",
            "Two indices that charge for resources. **Total-time rate**: MB removed per second of "
            "compress plus decompress time. **Per-GB rate**: the same per gigabyte of memory, the "
            "time × memory view. And a tunable **weighted cost** `bpb × (comp + decomp)^α × mem^β`: "
            "α and β say how much a tenfold increase in time or memory is worth in ratio terms; "
            "α = β = 0 is the plain ranking. Lower cost is better.", "",
            "| entry | family | ratio | total ns/B | mem MB | MB/s removed (total time) | MB/s per GB |",
            "|---|---|---:|---:|---:|---:|---:|"]
    for x in ei["total_time_top"]:
        out.append(f"| {x['program']} | {x['family']} | {_f(x['ratio'])} | {_f(x['total_ns_per_byte'], 0)} | "
                   f"{_f(x['mem_mb'], 0)} | {_f(x['saved_mb_per_s'], 1)} | {_f(x['saved_mb_per_s_per_gb'], 0)} |")
    out += ["", "Top by MB removed per second per GB of memory: "
            + ", ".join(f"{x['program']} ({_f(x['saved_mb_per_s_per_gb'], 0)})" for x in ei["per_gb_top"][:5]) + ".", "",
            "Winner for each weighting (rows α = time weight, columns β = memory weight):", "",
            "| α \\ β | " + " | ".join(str(b) for b in ei["betas"]) + " |",
            "|---:|" + "---|" * len(ei["betas"])]
    for alpha in ei["alphas"]:
        cells = [next(g["program"] for g in ei["preference_map"] if g["alpha"] == alpha and g["beta"] == beta)
                 for beta in ei["betas"]]
        out.append(f"| {alpha} | " + " | ".join(cells) + " |")
    out += ["", f"{len(ei['distinct_winners'])} distinct winners across the grid: "
            + ", ".join(ei["distinct_winners"]) + ".", ""]
    for key, ranking in ei["rankings"].items():
        out.append(f"- α/β = {key}: " + ", ".join(f"{r['program']} ({r['bpb']:.2f} bpb, cost {r['cost']:.2f})"
                                                  for r in ranking))
    out.append("")
    out += ["## Efficiency (distance above the frontier)", ""]
    for axis, e in a["efficiency"].items():
        out.append(f"- **{axis}**: {e['frontier_points']} frontier points, {e['within_0.05']} entries "
                   f"within {EFFICIENT_GAP} bpb of it, median gap {_f(e['median_gap'])} bpb; "
                   "furthest: " + ", ".join(f"{w['program']} (+{w['gap']:.2f})" for w in e["worst"][:3]))
    d = a["decompressor"]
    out += ["", "## Decompressor size", "",
            f"Median decompressor share of enwik9 output {100 * d['median_share']:.3f} %. Over 1 %: "
            + (", ".join(f"{o['program']} ({100 * o['share']:.1f} %)" for o in d["over_1_percent"]) or "none")
            + ".", ""]
    out += ["## Decode/encode symmetry", "", "| Family | n | Q1 | median | Q3 |", "|---|---:|---:|---:|---:|"]
    for family, q in a["symmetry"].items():
        out.append(f"| {ltcb.FAMILY_LABELS[family]} | {q['n']} | {_f(q['q1'])} | {_f(q['median'])} | {_f(q['q3'])} |")
    out += ["", "## Local entries", ""]
    for item in a["local"]:
        out.append(f"### {item['label']}")
        out.append("")
        if "enwik8_rank" in item:
            lo, hi = item["predicted_enwik9_interval"]
            out.append(f"- enwik8 {_f(item['enwik8_bpb'])} bpb, rank #{item['enwik8_rank']} of "
                       f"{item['enwik8_rank_of']} on that column.")
            out.append(f"- **Predicted** enwik9 from the scaling law: {_f(item['predicted_enwik9_bpb'])} bpb "
                       f"(95 % interval {_f(lo)}–{_f(hi)}), which would rank about "
                       f"#{item['predicted_total_rank']} on the total. A prediction, not a measurement.")
        if item["measured_enwik9"]:
            out.append(f"- **Measured** enwik9 {_f(item['enwik9_bpb'])} bpb, total rank "
                       f"#{item['total_rank']} of {item['total_rank_of']}.")
        for axis in ("compression", "decompression"):
            if f"{axis}_saved_mb_per_s" in item:
                out.append(f"- Removes {_f(item[f'{axis}_saved_mb_per_s'], 2)} MB per second of "
                           f"{axis} time, rank #{item[f'{axis}_saved_rank']} on that measure.")
        if "total_time_saved_mb_per_s" in item:
            out.append(f"- Removes {_f(item['total_time_saved_mb_per_s'], 3)} MB per second of total time "
                       f"(rank #{item['total_time_saved_rank']}), {_f(item['per_gb_saved_mb_per_s'], 2)} per GB "
                       f"(rank #{item['per_gb_saved_rank']}). Weighted-cost ranks: "
                       + ", ".join(f"α/β {k}: #{v['rank']}" for k, v in item["weighted_ranks"].items()) + ".")
        if "frontier_gap" in item:
            out.append(f"- {_f(item['frontier_gap'])} bpb above the compression-speed frontier at its "
                       f"own speed ({item['frontier_gap_basis']} ratio).")
            out.append("- Nearest published entries in (log speed, log memory, ratio): "
                       + ", ".join(f"{n['program']} ({n['family']}, d={n['distance']:.2f})"
                                   for n in item["neighbours"]) + ".")
        out.append("")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, nargs="?", default=ltcb.RESEARCH / "ltcb-main-table.csv")
    parser.add_argument("--local", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="JSON path; a .md report is written beside it")
    parser.add_argument("--source", default=ltcb.DEFAULT_SOURCE)
    args = parser.parse_args(argv)
    try:
        rows = ltcb.load_rows(args.table)
        local = ltcb.load_local(args.local) if args.local else []
        result = analyze(rows, local)
    except (OSError, ltcb.TableError, AnalysisError) as error:
        parser.error(str(error))
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    args.output.with_suffix(".md").write_text(report(result, args.source) + "\n")
    print(args.output)
    print(args.output.with_suffix(".md"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
