#!/usr/bin/env python3
"""Promote an aggregate TS808 residual fit from a render-plan run.

This is the safe path for iterative work: each case can produce a residual
delta, but the plugin should receive one aggregate delta for the whole plan,
not whichever single case happened to run last.
"""
from __future__ import annotations

import argparse
import csv
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path


def read_json(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def read_bands(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            band = {
                "kind": row.get("kind") or "Peak",
                "freq_hz": float(row["freq_hz"]),
                "q": float(row.get("q") or row.get("q_basis") or 2.0),
                "gain_db": float(row["gain_db"]),
                "stages": int(float(row.get("stages") or 1)),
                "amount": row.get("amount") or "Classic",
            }
            if row.get("layer"):
                band["layer"] = row["layer"]
            rows.append(band)
    return rows


def same_layout(a: list[dict], b: list[dict]) -> bool:
    if len(a) != len(b):
        return False
    for left, right in zip(a, b):
        if left.get("kind") != right.get("kind"):
            return False
        if left.get("amount", "Classic") != right.get("amount", "Classic"):
            return False
        if int(float(left.get("stages", 1))) != int(float(right.get("stages", 1))):
            return False
        if abs(float(left.get("freq_hz", 0.0)) - float(right.get("freq_hz", 0.0))) > 1.0e-3:
            return False
        if abs(float(left.get("q", 0.0)) - float(right.get("q", 0.0))) > 1.0e-4:
            return False
    return True


def clamp(value: float, limit: float) -> float:
    return max(-limit, min(limit, value))


def aggregate_band_sets(band_sets: list[list[dict]], mode: str) -> list[dict]:
    if not band_sets:
        return []
    reference = band_sets[0]
    for bands in band_sets[1:]:
        if not same_layout(reference, bands):
            raise SystemExit("cannot aggregate residual fits: band layouts differ")

    aggregated: list[dict] = []
    for band_index, ref in enumerate(reference):
        gains = [float(bands[band_index]["gain_db"]) for bands in band_sets]
        if mode == "mean":
            gain = sum(gains) / len(gains)
        else:
            gain = statistics.median(gains)
        band = dict(ref)
        band["gain_db"] = gain
        aggregated.append(band)
    return aggregated


NDSP_TAIL = [
    ("LowShelf", 65.0),
    ("Peak", 125.0),
    ("Peak", 250.0),
    ("Peak", 500.0),
    ("Peak", 1000.0),
    ("Peak", 2000.0),
    ("Peak", 4000.0),
    ("Peak", 8000.0),
    ("HighShelf", 16000.0),
]
PRE_LAYER_NAMES = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYER_NAMES = ("post_a", "post_ndsp", "post_b")
CASCADE_LAYER_NAMES = (*PRE_LAYER_NAMES, *POST_LAYER_NAMES)


def strip_layer(band: dict) -> dict:
    out = dict(band)
    out.pop("layer", None)
    return out


def is_ndsp_tail(bands: list[dict], start: int) -> bool:
    if len(bands) - start < len(NDSP_TAIL):
        return False
    for band, (kind, freq) in zip(bands[start:start + len(NDSP_TAIL)], NDSP_TAIL):
        if str(band.get("kind")) != kind:
            return False
        if abs(float(band.get("freq_hz", -1.0)) - freq) > 1.0e-3:
            return False
    return True


def split_cascade(bands: list[dict]) -> dict[str, list[dict]]:
    explicit = {name: [] for name in CASCADE_LAYER_NAMES}
    has_explicit = False
    for band in bands:
        layer = str(band.get("layer", "")).strip()
        if layer in explicit:
            explicit[layer].append(strip_layer(band))
            has_explicit = True
    if has_explicit:
        unknown = [band for band in bands if str(band.get("layer", "")).strip() not in explicit]
        if unknown:
            raise SystemExit("mixed explicit/implicit cascade layer fit is not allowed")
        return explicit

    # Legacy/implicit fits are post-only. Foundation bands go to post_a,
    # the fixed NDSP grid goes to post_ndsp, and a final two-band plateau
    # refinement goes to post_b.
    layers = {name: [] for name in CASCADE_LAYER_NAMES}
    for i in range(0, len(bands) + 1):
        if is_ndsp_tail(bands, i):
            layers["post_a"] = [strip_layer(b) for b in bands[:i]]
            layers["post_ndsp"] = [strip_layer(b) for b in bands[i:i + len(NDSP_TAIL)]]
            tail = [strip_layer(b) for b in bands[i + len(NDSP_TAIL):]]
            if len(tail) > 2:
                raise SystemExit("fit emitted more than two post_b refinement bands")
            layers["post_b"] = tail
            return layers
    layers["post_a"] = [strip_layer(b) for b in bands]
    return layers


def combine_cascade(ts: dict, reference: list[dict]) -> list[dict]:
    if any(str(band.get("layer", "")).strip() in CASCADE_LAYER_NAMES for band in reference):
        out = []
        wanted_layers = [name for name in CASCADE_LAYER_NAMES if any(str(b.get("layer", "")).strip() == name for b in reference)]
        for name in wanted_layers:
            out.extend({**band, "layer": name} for band in ts.get(name, []))
        return out
    if all(name in ts for name in POST_LAYER_NAMES):
        return [band for name in POST_LAYER_NAMES for band in ts.get(name, [])]
    return list(ts.get("post_residual", []))


def write_cascade(ts: dict, bands: list[dict]) -> None:
    explicit_layers = {
        str(band.get("layer", "")).strip()
        for band in bands
        if str(band.get("layer", "")).strip() in CASCADE_LAYER_NAMES
    }
    layers = split_cascade(bands)
    if explicit_layers:
        update_layers = explicit_layers
    else:
        update_layers = set(POST_LAYER_NAMES)
    for name in update_layers:
        ts[name] = layers[name]
    ts["residual_pre"] = [band for name in PRE_LAYER_NAMES for band in ts.get(name, [])]
    ts["post_residual"] = [band for name in POST_LAYER_NAMES for band in ts.get(name, [])]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--render-plan", required=True)
    ap.add_argument("--out-root", required=True)
    ap.add_argument("--voicing-state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--header-generator", default="SAT-TR/tools/write_overdrive_voicing_header.py")
    ap.add_argument("--baseline", default="SAT-TR/tools/overdrive_fit_baselines.json")
    ap.add_argument("--sat-render", default="sat_voiced")
    ap.add_argument("--fit-subdir", default="overdrive_id_fit_voiced")
    ap.add_argument("--aggregate", choices=["median", "mean"], default="median")
    ap.add_argument("--residual-application", choices=["auto", "replace", "add"], default="auto")
    ap.add_argument("--residual-delta-scale", type=float, default=0.5)
    ap.add_argument("--max-residual-gain-db", type=float, default=18.0)
    ap.add_argument("--min-relative-improvement", type=float, default=0.001,
                    help="Required fractional aggregate score improvement over current best. Default 0.1%%.")
    ap.add_argument("--force", action="store_true", help="Apply even when aggregate score is not better.")
    ap.add_argument("--min-cases", type=int, default=1)
    ap.add_argument("--allow-missing", action="store_true",
                    help="Use available case fits and report missing cases instead of failing.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Report aggregate promotion details without writing JSON/header/baseline.")
    ap.add_argument("--no-baseline-update", action="store_true",
                    help="Apply/write state but do not update the aggregate baseline file.")
    ap.add_argument("--skip-header", action="store_true",
                    help="Write only the voicing JSON. Used for isolated candidate evaluation.")
    args = ap.parse_args()

    plan = read_json(Path(args.render_plan))
    cases = plan.get("cases", [])
    out_root = Path(args.out_root)
    state_path = Path(args.voicing_state)
    state = read_json(state_path)
    pedal = str(plan.get("pedal", "ts808")).lower()
    state_key = "klon" if pedal == "klon" else "ts808"
    ts = state.setdefault(state_key, {})
    variant = ts.get("active_variant", "unknown_variant")

    band_sets: list[list[dict]] = []
    used_cases: list[str] = []
    missing_cases: list[str] = []
    scores: dict[str, float] = {}
    filter_models: set[str] = set()

    for case in cases:
        case_id = case["id"]
        fit_dir = out_root / "overdrive_cases" / case_id / args.fit_subdir
        summary_path = fit_dir / "fit_summary.json"
        csv_path = fit_dir / "cma_filterbank_fit.csv"
        if not summary_path.exists() or not csv_path.exists():
            missing_cases.append(case_id)
            continue

        summary = read_json(summary_path)
        case_meta = summary.get("case_meta") or {}
        summary_pedal = case_meta.get("sat_voicing_pedal", pedal)
        if summary_pedal != pedal:
            raise SystemExit(f"refusing to aggregate {case_id}: fit pedal {summary_pedal!r} does not match plan pedal {pedal!r}")
        summary_variant = case_meta.get("sat_voicing_variant", "unknown_variant")
        if summary_variant != variant:
            raise SystemExit(
                f"refusing to aggregate {case_id}: fit variant {summary_variant!r} "
                f"does not match active variant {variant!r}"
            )
        filter_model = summary.get("filter_model", "legacy_unknown_filter_model")
        if filter_model == "legacy_unknown_filter_model":
            raise SystemExit(f"refusing to aggregate stale/legacy fit summary: {summary_path}")
        filter_models.add(filter_model)
        scores[case_id] = float(summary["score"])
        band_sets.append(read_bands(csv_path))
        used_cases.append(case_id)

    if missing_cases and not args.allow_missing:
        raise SystemExit("missing fit outputs for case(s): " + ", ".join(missing_cases))
    if len(used_cases) < args.min_cases:
        raise SystemExit(f"only {len(used_cases)} usable case fit(s), min-cases is {args.min_cases}")

    score_values = list(scores.values())
    aggregate_score = (sum(score_values) / len(score_values)
                       if args.aggregate == "mean"
                       else statistics.median(score_values))
    baseline_key = f"plan:{Path(args.render_plan).name}:{pedal}:{variant}:{','.join(sorted(filter_models))}:{args.aggregate}"
    baselines = read_json(Path(args.baseline))
    previous = baselines.get(baseline_key)
    accepted = False
    reason = ""
    previous_score = None
    if previous is None:
        accepted = True
        reason = "no previous aggregate baseline"
    else:
        previous_score = float(previous["aggregate_score"])
        required = previous_score * (1.0 - args.min_relative_improvement)
        if aggregate_score <= required:
            accepted = True
            reason = f"aggregate score improved from {previous_score:.9g} to {aggregate_score:.9g}"
        elif args.force:
            accepted = True
            reason = f"forced accept; previous aggregate score was {previous_score:.9g}, new is {aggregate_score:.9g}"
        else:
            reason = f"rejected; previous aggregate score {previous_score:.9g}, new {aggregate_score:.9g}"

    if not accepted:
        print(json.dumps({
            "applied": False,
            "accepted": False,
            "dry_run": bool(args.dry_run),
            "reason": reason,
            "pedal": pedal,
            "variant": variant,
            "aggregate_score": aggregate_score,
            "previous_aggregate_score": previous_score,
            "aggregate": args.aggregate,
            "used_cases": used_cases,
            "missing_cases": missing_cases,
            "scores": scores,
        }, indent=2))
        raise SystemExit(2)

    delta = aggregate_band_sets(band_sets, args.aggregate)
    application = args.residual_application
    if application == "auto":
        application = "add" if variant == "core-residual" else "replace"

    limit = abs(float(args.max_residual_gain_db))
    if application == "add":
        existing = combine_cascade(ts, delta)
        if not same_layout(existing, delta):
            raise SystemExit("cannot add aggregate residual delta: existing post cascade layout differs")
        scale = float(args.residual_delta_scale)
        combined = []
        for old_band, delta_band in zip(existing, delta):
            band = dict(delta_band)
            band["gain_db"] = clamp(float(old_band.get("gain_db", 0.0)) + scale * float(delta_band["gain_db"]), limit)
            combined.append(band)
        write_cascade(ts, combined)
    else:
        scale = float(args.residual_delta_scale)
        write_cascade(ts, [
            {**band, "gain_db": clamp(scale * float(band["gain_db"]), limit)}
            for band in delta
        ])

    ts["residual_matching_enabled"] = True
    ts["last_plan_residual_application"] = application
    ts["last_plan_residual_delta_scale"] = float(args.residual_delta_scale)
    ts["last_plan_residual_aggregate"] = args.aggregate
    ts["last_plan_residual_cases"] = used_cases
    ts["last_plan_residual_scores"] = scores
    ts["last_plan_residual_filter_models"] = sorted(filter_models)
    ts["last_plan_residual_updated_at_unix"] = time.time()

    if not args.dry_run:
        write_json(state_path, state)
        if not args.skip_header:
            subprocess.run([sys.executable, args.header_generator], check=True)

        if not args.no_baseline_update:
            baselines[baseline_key] = {
                "pedal": pedal,
            "variant": variant,
                "used_cases": used_cases,
                "missing_cases": missing_cases,
                "scores": scores,
                "aggregate_score": aggregate_score,
                "aggregate": args.aggregate,
                "application": application,
                "delta_scale": float(args.residual_delta_scale),
                "updated_at_unix": time.time(),
                "note": "Aggregate promotion writes one residual delta for the whole render plan.",
            }
            write_json(Path(args.baseline), baselines)

    print(json.dumps({
        "applied": not args.dry_run,
        "accepted": True,
        "dry_run": bool(args.dry_run),
        "reason": reason,
        "variant": variant,
        "aggregate_score": aggregate_score,
        "previous_aggregate_score": previous_score,
        "application": application,
        "aggregate": args.aggregate,
        "delta_scale": float(args.residual_delta_scale),
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "scores": scores,
    }, indent=2))


if __name__ == "__main__":
    main()
