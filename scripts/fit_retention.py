"""Fit paper Eq. 5 to supplied retention summaries, with within-condition t=0 normalization.

Public API: fit_retention(records) -> JSON-serializable dict.
Each record contains chip, read_mode, pe, age_days, fbc. No measured values are
embedded here. R/B47R_R Default and BestRetry are fitted separately. Other chips
are reported as ignored; the current workbook does not contain complete M curves.

"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Iterable

import numpy as np
import scipy
from scipy.optimize import least_squares


def _chip_is_r(value) -> bool:
    token = str(value).strip().upper().replace("-", "_").replace(" ", "_")
    return token in {"R", "B47R_R", "MICRON_B47R_R"}


def _read_mode(value):
    token = str(value).strip().lower().replace("_", "").replace("-", "").replace(" ", "")
    return {"default": "default", "defaultread": "default", "bestretry": "best_retry", "bestretryread": "best_retry"}.get(token)


def _prepare(records: Iterable[dict]):
    rows, ignored, duplicates = {}, [], 0
    for i, original in enumerate(records):
        if not isinstance(original, dict):
            raise ValueError(f"Record {i} is not a dictionary")
        if not _chip_is_r(original.get("chip")):
            ignored.append({"index": i, "reason": "chip is not explicitly R/B47R_R"})
            continue
        mode = _read_mode(original.get("read_mode"))
        if mode is None:
            ignored.append({"index": i, "reason": "unsupported read mode"})
            continue
        try:
            pe, age, fbc = (float(original[k]) for k in ("pe", "age_days", "fbc"))
        except (KeyError, ValueError, TypeError):
            ignored.append({"index": i, "reason": "missing/nonnumeric condition or FBC; not imputed"})
            continue
        if not all(math.isfinite(x) for x in (pe, age, fbc)) or pe < 0 or age < 0 or fbc <= 0:
            raise ValueError(f"Record {i}: require finite pe>=0, age_days>=0 and fbc>0")
        key = (mode, pe, age)
        if key in rows:
            if rows[key]["fbc"] != fbc:
                raise ValueError(f"Conflicting duplicate condition {key}; replicate aggregation must be specified")
            duplicates += 1
            continue
        rows[key] = {"chip": "R", "read_mode": mode, "pe": pe, "age_days": age, "fbc": fbc}
    return sorted(rows.values(), key=lambda x: (x["read_mode"], x["pe"], x["age_days"])), ignored, duplicates


def _log_prediction(params, pe, age):
    # params = (ln t0days, delta, ln gamma). logaddexp avoids overflow in log(1+x).
    log_t0, delta, log_gamma = params
    log_ratio = np.where(age > 0, np.log(np.maximum(age, 1e-300)) - log_t0 + delta * np.log1p(pe), -np.inf)
    return np.exp(log_gamma) * np.logaddexp(0, log_ratio)


def _fit(pe, age, log_growth, mode="full", fixed_delta=None):
    if mode == "simple":
        fixed_delta = 0.0
    lower = np.array([-12.0, -5.0 if mode == "signed_delta" else 0.0, -9.0])
    upper = np.array([20.0, 5.0, 3.0])
    if fixed_delta is None:
        expand = lambda x: x
        seeds = [np.array([t, d, g]) for t in (-3.0, 0.0, 3.0, 6.0) for d in (0.01, 0.5) for g in (-1.0, 0.0)]
    else:
        expand = lambda x: np.array([x[0], fixed_delta, x[1]])
        lower, upper = lower[[0, 2]], upper[[0, 2]]
        seeds = [np.array([t, g]) for t in (-3.0, 0.0, 3.0, 6.0) for g in (-1.0, 0.0)]
    best = None
    for seed in seeds:
        result = least_squares(
            lambda x: _log_prediction(expand(x), pe, age) - log_growth,
            seed, bounds=(lower, upper), max_nfev=10000,
            xtol=1e-13, ftol=1e-13, gtol=1e-13,
        )
        if best is None or result.cost < best.cost:
            best = result
    params = expand(best.x)
    singular = np.linalg.svd(best.jac, compute_uv=False)
    condition = float(singular[0] / singular[-1]) if singular[-1] > 0 else None
    return params, {
        "t0days": float(np.exp(params[0])), "delta": float(params[1]), "gamma": float(np.exp(params[2])),
        "log_growth_sse": float(2 * best.cost), "optimizer_success": bool(best.success),
        "optimizer_active_bound_mask": best.active_mask.tolist(),
        "jacobian_singular_values": singular.tolist(), "jacobian_condition": condition,
    }


def _metrics(observed, predicted):
    residual = np.asarray(predicted) - np.asarray(observed)
    return {
        "n_condition_cells": int(residual.size),
        "log_growth_rmse": float(np.sqrt(np.mean(residual ** 2))),
        "log_growth_mae": float(np.mean(np.abs(residual))),
        "growth_mape_pct": float(np.mean(np.abs(np.exp(residual) - 1)) * 100),
        "worst_multiplicative_error": float(np.exp(np.max(np.abs(residual)))),
    }


def _cv(pe, age, log_growth, mode, group_by):
    grouping = age if group_by == "age_days" else pe
    folds, observed_all, predicted_all = [], [], []
    for held_out in np.unique(grouping):
        train = grouping != held_out
        # Without two positive age groups the anchored two-parameter time curve is
        # not identifiable. Full model additionally needs at least two PE groups.
        if len(np.unique(age[train])) < 2 or (mode != "simple" and len(np.unique(pe[train])) < 2):
            folds.append({"held_out": float(held_out), "status": "not_identifiable_in_training_subset"})
            continue
        params, details = _fit(pe[train], age[train], log_growth[train], mode)
        prediction = _log_prediction(params, pe[~train], age[~train])
        folds.append({
            "held_out": float(held_out), "status": "evaluated",
            "parameters": {k: details[k] for k in ("t0days", "delta", "gamma")},
            "metrics": _metrics(log_growth[~train], prediction),
            "observed_growth": np.exp(log_growth[~train]).tolist(), "predicted_growth": np.exp(prediction).tolist(),
        })
        observed_all.extend(log_growth[~train])
        predicted_all.extend(prediction)
    return {"pooled": _metrics(observed_all, predicted_all) if observed_all else None, "folds": folds}


def _analyse_mode(rows):
    if not rows:
        return {"status": "no_numeric_records", "models": {}}
    baseline = {r["pe"]: r["fbc"] for r in rows if r["age_days"] == 0}
    positive = [r for r in rows if r["age_days"] > 0]
    missing_baselines = sorted({r["pe"] for r in positive} - set(baseline))
    if missing_baselines:
        raise ValueError(f"Missing same-read-mode t=0 baseline at PE {missing_baselines}; cannot normalize")
    normalized = [dict(r, growth=r["fbc"] / baseline[r["pe"]]) for r in rows]
    if len({r["age_days"] for r in positive}) < 2:
        return {"status": "insufficient_positive_age_groups", "normalized_records": normalized, "models": {}}
    pe = np.array([r["pe"] for r in positive])
    age = np.array([r["age_days"] for r in positive])
    log_growth = np.log([r["fbc"] / baseline[r["pe"]] for r in positive])
    result = {
        "status": "fitted_condition_summaries", "normalized_records": normalized,
        "baseline_condition_cells": len(baseline), "positive_age_condition_cells": len(positive),
        "monotonic_violations": [], "models": {},
    }
    for p in sorted(baseline):
        group = [r for r in rows if r["pe"] == p]
        for first, second in zip(group, group[1:]):
            if second["fbc"] < first["fbc"]:
                result["monotonic_violations"].append({
                    "pe": p, "from_days": first["age_days"], "to_days": second["age_days"],
                    "from_fbc": first["fbc"], "to_fbc": second["fbc"],
                    "relative_drop_pct": (1 - second["fbc"] / first["fbc"]) * 100,
                })
    for mode in ("full", "simple", "signed_delta"):
        if mode != "simple" and len(np.unique(pe)) < 2:
            result["models"][mode] = {"status": "delta_not_identifiable_with_single_pe"}
            continue
        params, details = _fit(pe, age, log_growth, mode)
        details["training"] = _metrics(log_growth, _log_prediction(params, pe, age))
        details["predictions"] = [
            {"pe": float(p), "age_days": float(t), "observed_growth": float(np.exp(g)), "predicted_growth": float(np.exp(pred))}
            for p, t, g, pred in zip(pe, age, log_growth, _log_prediction(params, pe, age))
        ]
        details["holdout_time_days"] = _cv(pe, age, log_growth, mode, "age_days")
        details["holdout_pe"] = _cv(pe, age, log_growth, mode, "pe")
        result["models"][mode] = details
    result["delta_profile"] = [_fit(pe, age, log_growth, fixed_delta=d)[1] for d in (0.0, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0)]
    return result


def _six_month_sensitivity(rows):
    six_months = [r for r in rows if r["age_days"] in (180.0, 182.5)]
    if not six_months:
        return {"applicable": False, "reason": "No supplied 180/182.5-day six-month condition"}
    variants = {}
    for six_month_age in (180.0, 182.5):
        modified = [dict(r, age_days=six_month_age if r["age_days"] in (180.0, 182.5) else r["age_days"]) for r in rows]
        variants[str(six_month_age)] = {}
        for mode in ("default", "best_retry"):
            analysis = _analyse_mode([r for r in modified if r["read_mode"] == mode])
            simple = analysis["models"].get("simple")
            if simple:
                variants[str(six_month_age)][mode] = {
                    "parameters": {k: simple[k] for k in ("t0days", "delta", "gamma")},
                    "training": simple["training"], "holdout_time_days": simple["holdout_time_days"]["pooled"],
                    "holdout_pe": simple["holdout_pe"]["pooled"],
                }
    differences = {}
    for mode in ("default", "best_retry"):
        if mode in variants["180.0"] and mode in variants["182.5"]:
            a, b = variants["180.0"][mode]["parameters"], variants["182.5"][mode]["parameters"]
            differences[mode] = {k: (a[k] / b[k] - 1) * 100 for k in ("t0days", "gamma")}
    return {
        "applicable": True,
        "definition": "Only supplied age_days exactly 180 or 182.5 is interpreted as 6M; 1Y/3Y ages unchanged. No new measured cells.",
        "variants_six_month_days": variants,
        "parameter_change_180_vs_182_5_pct": differences,
    }


def fit_retention(records):
    """Return a descriptive R retention fit and grouped validation diagnostics.

    The returned selected_default is always the parsimonious delta=0 fit for R
    Default, or None when insufficient data exist. It is a candidate, not a claim
    of physical-time calibration. All fitting and CV retain each measured point.
    """
    rows, ignored, duplicates = _prepare(records)
    default = _analyse_mode([r for r in rows if r["read_mode"] == "default"])
    retry = _analyse_mode([r for r in rows if r["read_mode"] == "best_retry"])
    simple = default["models"].get("simple")
    selected = {k: simple[k] for k in ("t0days", "delta", "gamma")} if simple else None
    return {
        "selected_default": selected,
        "selected_default_scope": "B47R_R Default summary growth; fixed delta=0, conditional on supplied age convention and retention protocol",
        "normalization": "For each PE and read mode: G=FBC(t)/FBC(0); cross-temperature BOL is never used. G(0)=1 is not counted as a fitted observation.",
        "objective": "Unweighted least squares of natural-log growth for t>0",
        "constraints": {"t0days": [math.exp(-12), math.exp(20)], "delta": [0, 5], "gamma": [math.exp(-9), math.exp(3)]},
        "signed_delta_status": "Diagnostic only, with delta in [-5,5]; not the selected constrained model",
        "validation_boundary": "Grouped held-out conditions, not independent chip validation; held-out PE normalization uses its known same-mode t=0 baseline.",
        "replication_limit": "Condition summary cells are not independent chip replicates; no inferential confidence intervals or p-values.",
        "protocol_limit": "Input age_days must be declared by caller; fitting does not establish bake temperature, acceleration law or physical elapsed time.",
        "imputation": "none",
        "ignored_records": ignored, "identical_duplicate_records_not_counted_twice": duplicates,
        "R_default": default, "R_best_retry": retry,
        "six_month_sensitivity": _six_month_sensitivity(rows),
        "software": {"python": sys.version, "numpy": np.__version__, "scipy": scipy.__version__},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--records", required=True, type=Path, help="JSON list of retention records, or object with a records list")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = json.loads(args.records.read_text())
    records = source["records"] if isinstance(source, dict) else source
    if not isinstance(records, list):
        raise ValueError("Input must contain a list of record dictionaries")
    result = fit_retention(records)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"selected_default": result["selected_default"], "output": str(args.output)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
