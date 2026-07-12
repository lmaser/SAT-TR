#!/usr/bin/env python3
"""Meta-analysis for SAT-TR TS808 Overdrive cascade tuning.

This script does not modify plugin state. It summarizes convergence, timing,
accepted candidates, current cascade shape, partial renders and practical next
steps so the tuning loop is auditable instead of anecdotal.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


def read_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def classify_cmd(cmd: list[str]) -> str:
    joined = " ".join(cmd)
    if "run_overdrive_analysis_suite.py" in joined:
        if "full_guard" in joined and "--skip-hammerstein" not in cmd:
            return "full_guard_hammerstein"
        if "pre_cascade_candidates" in joined:
            return "pre_screen"
        if "verified_candidates" in joined:
            return "verify_candidate"
        if "overdrive_id_renders" in joined:
            return "source_score"
        return "analysis_other"
    if "promote_overdrive_plan_fit.py" in joined:
        return "promote_residual"
    if "optimize_overdrive_ts808_core.py" in joined:
        return "optimize_core"
    if "fit_overdrive_ts808_controls.py" in joined:
        return "control_fit"
    if "build_sat_overdrive_renderer" in joined:
        return "build_renderer"
    if "write_overdrive_voicing_header.py" in joined:
        return "write_header"
    if "set_overdrive_ts808_variant.py" in joined:
        return "set_variant"
    return "other"


def infer_band_from_label(label: str) -> dict[str, Any]:
    """Recover band metadata from compact accepted-candidate labels.

    Older candidate_result.json files sometimes store only the label, not the
    expanded band object. The fit is valid either way, but the meta report must
    still classify kind/layer/frequency so convergence audits are trustworthy.
    """
    match = re.search(
        r"(?P<layer>pre_[ab]|post_[ab])_(?P<slot>\d+)_(?P<kind>[A-Za-z]+)_"
        r"(?P<freq>[0-9]+(?:p[0-9]+)?)_q(?P<q>[0-9]+(?:p[0-9]+)?)_"
        r"s(?P<stages>\d+)_(?P<sign>[pm])(?P<gain>[0-9]+(?:p[0-9]+)?)db",
        label,
    )
    if not match:
        return {}

    def number(value: str) -> float:
        return float(value.replace("p", "."))

    gain = number(match.group("gain"))
    if match.group("sign") == "m":
        gain = -gain
    return {
        "layer": match.group("layer"),
        "band_index": int(match.group("slot")),
        "kind": match.group("kind"),
        "freq_hz": number(match.group("freq")),
        "q": number(match.group("q")),
        "stages": int(match.group("stages")),
        "gain_db": gain,
    }


def load_accepted(root: Path) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    accepted_root = root / "accepted_candidates"
    for path in sorted(accepted_root.glob("*/candidate_result.json"), key=lambda p: p.stat().st_mtime):
        result = read_json(path)
        candidate = result.get("candidate", {}) if isinstance(result, dict) else {}
        band = candidate.get("band", {}) if isinstance(candidate, dict) else {}
        label = candidate.get("label")
        inferred = infer_band_from_label(str(label or ""))
        if not band and inferred:
            band = inferred
        items.append({
            "path": str(path),
            "mtime": path.stat().st_mtime,
            "score": result.get("score"),
            "selection_score": result.get("selection_score"),
            "label": label,
            "application": candidate.get("application"),
            "layer": candidate.get("layer", inferred.get("layer") if inferred else None),
            "band_index": candidate.get("band_index", inferred.get("band_index") if inferred else None),
            "kind": band.get("kind"),
            "freq_hz": band.get("freq_hz"),
            "q": band.get("q"),
            "gain_db": band.get("gain_db"),
            "tone_guard": result.get("tone_guard", {}),
        })
    return items


def load_timing(root: Path) -> list[dict[str, Any]]:
    path = root / "iteration_timing.jsonl"
    rows: list[dict[str, Any]] = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if not line.strip():
            continue
        try:
            rows.append(json.loads(line))
        except Exception:
            pass
    return rows


def summarize_timing(rows: list[dict[str, Any]]) -> dict[str, Any]:
    groups: dict[str, list[float]] = defaultdict(list)
    failures: list[dict[str, Any]] = []
    for row in rows:
        cmd = row.get("cmd", [])
        if not isinstance(cmd, list):
            cmd = []
        elapsed = float(row.get("elapsed_sec", 0.0) or 0.0)
        groups[classify_cmd([str(x) for x in cmd])].append(elapsed)
        if int(row.get("returncode", 0) or 0) != 0:
            failures.append(row)
    by_stage = {}
    for name, vals in sorted(groups.items(), key=lambda kv: -sum(kv[1])):
        by_stage[name] = {
            "count": len(vals),
            "total_sec": round(sum(vals), 3),
            "median_sec": round(statistics.median(vals), 3),
            "max_sec": round(max(vals), 3),
        }
    return {
        "command_count": len(rows),
        "total_sec": round(sum(float(r.get("elapsed_sec", 0.0) or 0.0) for r in rows), 3),
        "by_stage": by_stage,
        "failures": failures[-12:],
    }


def layer_counts(ts: dict[str, Any]) -> dict[str, int]:
    return {name: len(ts.get(name, []) or []) for name in ["pre_a", "pre_ndsp", "pre_b", "post_a", "post_ndsp", "post_b", "post_core"]}


def band_signature(item: dict[str, Any]) -> str:
    if item.get("kind") is None:
        return str(item.get("label"))
    return f"{item.get('layer')}[{item.get('band_index')}]:{item.get('kind')}@{item.get('freq_hz')}Hz q{item.get('q')} {item.get('gain_db')}dB"


def find_partial_wavs(root: Path, max_bytes: int) -> list[dict[str, Any]]:
    partials = []
    for wav in root.rglob("*.wav"):
        try:
            size = wav.stat().st_size
        except OSError:
            continue
        if size <= max_bytes:
            partials.append({"path": str(wav), "bytes": size, "mtime": wav.stat().st_mtime})
    return sorted(partials, key=lambda x: x["mtime"], reverse=True)


def convergence_summary(accepted: list[dict[str, Any]]) -> dict[str, Any]:
    scores = []
    rows = []
    previous = None
    for i, item in enumerate(accepted, 1):
        try:
            score = float(item.get("score"))
        except Exception:
            continue
        improvement = None if previous is None else previous - score
        rows.append({
            "index": i,
            "score": score,
            "improvement": improvement,
            "label": item.get("label"),
            "band": band_signature(item),
        })
        scores.append(score)
        previous = score
    best = min(scores) if scores else None
    last = scores[-1] if scores else None
    last_improvement = rows[-1]["improvement"] if rows else None
    monotonic_after_first_good = True
    if len(scores) > 2:
        best_so_far = scores[0]
        for score in scores[1:]:
            if score > best_so_far and best_so_far < 0.05:
                monotonic_after_first_good = False
            best_so_far = min(best_so_far, score)
    return {
        "accepted_count": len(rows),
        "best_score": best,
        "last_score": last,
        "last_improvement": last_improvement,
        "monotonic_after_first_good": monotonic_after_first_good,
        "trajectory": rows,
    }


def recommendations(convergence: dict[str, Any], timing: dict[str, Any], state: dict[str, Any]) -> list[str]:
    recs = []
    by_stage = timing.get("by_stage", {})
    pre_total = by_stage.get("pre_screen", {}).get("total_sec", 0.0) + by_stage.get("verify_candidate", {}).get("total_sec", 0.0)
    guard_total = by_stage.get("full_guard_hammerstein", {}).get("total_sec", 0.0)
    if pre_total > guard_total * 3:
        recs.append("Optimize PRE candidate generation/cache first; PRE screen+verify dominates runtime more than Hammerstein guard.")
    last_imp = convergence.get("last_improvement")
    if isinstance(last_imp, float) and last_imp < 0.0005:
        recs.append("Plateau is local: run a replay audit from a cleared cascade before adding more bands or widening Q/frequency search.")
    if state.get("last_verified_best_score") is not None:
        recs.append("Current state is a valid checkpoint; compile/test from this state before accepting deeper structural changes.")
    recs.append("For reproducibility, run replay audit with the same command budget and compare final score/candidate family against current best.")
    return recs


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--analysis-root", default="analysis_out/ts808_core_residual")
    ap.add_argument("--state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--partial-max-bytes", type=int, default=8192)
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    root = Path(args.analysis_root)
    state_path = Path(args.state)
    state = read_json(state_path)
    ts = state.get("ts808", {}) if isinstance(state, dict) else {}
    accepted = load_accepted(root)
    timing = summarize_timing(load_timing(root))
    convergence = convergence_summary(accepted)
    partials = find_partial_wavs(root, args.partial_max_bytes)
    kind_counter = Counter(str(item.get("kind")) for item in accepted if item.get("kind"))
    layer_counter = Counter(str(item.get("layer")) for item in accepted if item.get("layer"))

    report = {
        "analysis_root": str(root),
        "state_path": str(state_path),
        "current_score": ts.get("last_verified_best_score"),
        "current_candidate": ts.get("last_verified_best_candidate"),
        "current_layer_counts": layer_counts(ts),
        "convergence": convergence,
        "accepted_kind_counts": dict(kind_counter),
        "accepted_layer_counts": dict(layer_counter),
        "timing": timing,
        "partial_wavs": partials[:20],
        "recommendations": recommendations(convergence, timing, ts),
    }

    if args.json_out:
        out = Path(args.json_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
        print(f"Wrote meta report: {out}")

    print("TS808 META REPORT")
    print(f"  analysis_root: {root}")
    print(f"  current_score: {report['current_score']}")
    print(f"  accepted_count: {convergence['accepted_count']}")
    print(f"  best_score: {convergence['best_score']}")
    print(f"  last_improvement: {convergence['last_improvement']}")
    print(f"  layer_counts: {report['current_layer_counts']}")
    print("\nAccepted trajectory:")
    for row in convergence["trajectory"]:
        imp = row["improvement"]
        imp_text = "" if imp is None else f" improvement={imp:.9f}"
        print(f"  {row['index']:02d} score={row['score']:.9f}{imp_text} {row['band']}")
    print("\nTiming by stage:")
    for name, info in timing["by_stage"].items():
        print(f"  {name:24s} n={info['count']:3d} total={info['total_sec']:8.2f}s med={info['median_sec']:6.2f}s max={info['max_sec']:6.2f}s")
    if partials:
        print("\nPartial WAVs still present (likely old if timestamp predates renderer hardening):")
        for item in partials[:8]:
            print(f"  {item['bytes']:6d} bytes {item['path']}")
    print("\nRecommendations:")
    for rec in report["recommendations"]:
        print(f"  - {rec}")


if __name__ == "__main__":
    main()
