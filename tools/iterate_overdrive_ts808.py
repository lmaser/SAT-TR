#!/usr/bin/env python3
"""One-command guarded TS808 analysis/promotion iteration for SAT-TR Overdrive.

The important contract is post-apply verification: a candidate residual is never
kept just because the fit that produced it looks good. The script applies each
candidate to a snapshot, rerenders SAT, reruns the analysis, and keeps only the
candidate that improves the verified score.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import soundfile as sf


RUN_TIMING_LOG: Path | None = None


def append_timing_log(cmd: list[str], *, elapsed: float, returncode: int) -> None:
    if RUN_TIMING_LOG is None:
        return
    try:
        RUN_TIMING_LOG.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "time_unix": time.time(),
            "elapsed_sec": elapsed,
            "returncode": int(returncode),
            "cmd": cmd,
        }
        with RUN_TIMING_LOG.open("a", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(payload, sort_keys=True) + "\n")
    except Exception as exc:
        print(f"WARNING: failed to write timing log: {exc}")


def run(cmd: list[str], *, allow_reject: bool = False) -> int:
    print("\n==>", " ".join(cmd))
    start = time.perf_counter()
    completed = subprocess.run(cmd)
    elapsed = time.perf_counter() - start
    append_timing_log(cmd, elapsed=elapsed, returncode=completed.returncode)
    print(f"<== done in {elapsed:.2f}s")
    if allow_reject and completed.returncode == 2:
        return completed.returncode
    completed.check_returncode()
    return completed.returncode


def read_json(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    try:
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except FileNotFoundError:
        return "missing"


def stable_hash_payload(payload: dict) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def fit_signature_payload(args) -> dict:
    return {
        "version": 4,
        "render_plan": str(Path(args.render_plan)),
        "render_plan_sha256": file_sha256(Path(args.render_plan)),
        "stim_manifest_sha256": file_sha256(Path(args.stim_dir) / "manifest.json"),
        "pre_screen_pack": getattr(args, "pre_screen_pack", "off"),
        "pre_screen_render_plan": str(getattr(args, "screen_render_plan", "")),
        "pre_screen_render_plan_sha256": file_sha256(Path(getattr(args, "screen_render_plan", ""))) if getattr(args, "screen_render_plan", "") else "",
        "pre_screen_manifest_sha256": file_sha256(Path(getattr(args, "screen_stim_dir", "")) / "manifest.json") if getattr(args, "screen_stim_dir", "") else "",
        "fit_layout": args.fit_layout,
        "fit_bands": int(args.fit_bands),
        "fit_basis_q": float(args.fit_basis_q),
        "fit_max_gain_db": float(args.fit_max_gain_db),
        "fit_grid_points": int(args.fit_grid_points),
        "verify_nfft": int(args.verify_nfft),
        "foundation_prefilter_limit": int(args.foundation_prefilter_limit),
        "foundation_exact_limit": int(args.foundation_exact_limit),
        "pre_cascade_screen_limit": int(args.pre_cascade_screen_limit),
        "aggregate": args.aggregate,
        "variant": args.variant,
        "sat_render_mode": "voiced",
        "tone_guard": args.tone_guard,
        "tone_guard_low_cut_db": float(args.tone_guard_low_cut_db),
        "tone_guard_air_cut_db": float(args.tone_guard_air_cut_db),
        "tone_guard_sub_boost_db": float(args.tone_guard_sub_boost_db),
        "tone_guard_weight": float(args.tone_guard_weight),
    }


def candidate_evaluation_signature(args, *, state_snapshot: str, candidate: dict, base_label: str | None = None) -> str:
    payload = fit_signature_payload(args)
    payload.update({
        "state_snapshot_sha256": hashlib.sha256(state_snapshot.encode("utf-8")).hexdigest(),
        "candidate": candidate,
        "base_label": base_label or "",
    })
    return stable_hash_payload(payload)


def load_cached_candidate_result(root: str | Path, expected_signature: str) -> dict | None:
    root_path = Path(root)
    report_path = root_path / "candidate_result.json"
    state_path = root_path / "candidate_state.json"
    if not report_path.exists() or not state_path.exists():
        return None
    try:
        report = read_json(report_path)
        if report.get("evaluation_signature") != expected_signature:
            return None
        result = dict(report)
        result["state_snapshot"] = state_path.read_text(encoding="utf-8")
        result["cache_hit"] = True
        return result
    except Exception:
        return None


def load_cached_screen_result(root: str | Path, expected_signature: str) -> dict | None:
    root_path = Path(root)
    report_path = root_path / "candidate_screen_result.json"
    state_path = root_path / "candidate_screen_state.json"
    if not report_path.exists() or not state_path.exists():
        return None
    try:
        report = read_json(report_path)
        if report.get("evaluation_signature") != expected_signature:
            return None
        result = dict(report)
        result["state_snapshot"] = state_path.read_text(encoding="utf-8")
        result["screen_cache_hit"] = True
        return result
    except Exception:
        return None


def persist_screen_candidate_artifacts(root: str | Path, result: dict) -> None:
    root_path = Path(root)
    root_path.mkdir(parents=True, exist_ok=True)
    snapshot = str(result.get("state_snapshot", ""))
    report = {key: value for key, value in result.items() if key != "state_snapshot"}
    report["state_snapshot_file"] = "candidate_screen_state.json" if snapshot else None
    report["written_at_unix"] = time.time()
    write_json(root_path / "candidate_screen_result.json", report)
    if snapshot:
        (root_path / "candidate_screen_state.json").write_text(snapshot, encoding="utf-8", newline="\n")


def persist_candidate_artifacts(root: str | Path, result: dict) -> None:
    root_path = Path(root)
    root_path.mkdir(parents=True, exist_ok=True)
    snapshot = str(result.get("state_snapshot", ""))
    report = {key: value for key, value in result.items() if key != "state_snapshot"}
    report["state_snapshot_file"] = "candidate_state.json" if snapshot else None
    report["written_at_unix"] = time.time()
    write_json(root_path / "candidate_result.json", report)
    if snapshot:
        (root_path / "candidate_state.json").write_text(snapshot, encoding="utf-8", newline="\n")


def source_analysis_signature(args, *, state_snapshot: str, label: str = "source") -> str:
    payload = fit_signature_payload(args)
    payload.update({
        "state_snapshot_sha256": hashlib.sha256(state_snapshot.encode("utf-8")).hexdigest(),
        "label": label,
        "source_cache_version": 1,
    })
    return stable_hash_payload(payload)


def load_cached_source_result(root: str | Path, expected_signature: str) -> tuple[float, dict[str, float], list[str], list[str], list[str]] | None:
    report_path = Path(root) / "source_result.json"
    if not report_path.exists():
        return None
    try:
        report = read_json(report_path)
        if report.get("evaluation_signature") != expected_signature:
            return None
        return (
            float(report["score"]),
            dict(report.get("scores", {})),
            list(report.get("used_cases", [])),
            list(report.get("missing_cases", [])),
            list(report.get("filter_models", [])),
        )
    except Exception:
        return None


def persist_source_result(root: str | Path, *, evaluation_signature: str, score: float, scores: dict[str, float],
                          used_cases: list[str], missing_cases: list[str], filter_models: list[str]) -> None:
    write_json(Path(root) / "source_result.json", {
        "evaluation_signature": evaluation_signature,
        "score": score,
        "scores": scores,
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "filter_models": filter_models,
        "written_at_unix": time.time(),
    })


def load_cached_full_guard_result(root: str | Path, expected_signature: str) -> tuple[float, dict[str, float], list[str], list[str]] | None:
    report_path = Path(root) / "full_guard_result.json"
    if not report_path.exists():
        return None
    try:
        report = read_json(report_path)
        if report.get("evaluation_signature") != expected_signature:
            return None
        return (
            float(report["score"]),
            {str(k): float(v) for k, v in report.get("scores", {}).items()},
            [str(x) for x in report.get("used_cases", [])],
            [str(x) for x in report.get("missing_cases", [])],
        )
    except Exception:
        return None


def persist_full_guard_result(root: str | Path, result: dict) -> None:
    root_path = Path(root)
    root_path.mkdir(parents=True, exist_ok=True)
    report = dict(result)
    report["written_at_unix"] = time.time()
    write_json(root_path / "full_guard_result.json", report)


def snapshot_with_verification_metadata(snapshot: str, *, result: dict, reference_score: float, previous_score: float | None) -> str:
    data = json.loads(snapshot)
    ts = data.setdefault("ts808", {})
    ts["last_verified_best_score"] = float(result["score"])
    ts["last_verified_best_candidate"] = result.get("candidate", {})
    ts["last_verified_reference_score"] = float(reference_score)
    ts["last_verified_previous_baseline"] = previous_score
    ts["last_verified_case_scores"] = result.get("scores", {})
    ts["last_verified_filter_models"] = result.get("filter_models", [])
    ts["last_verified_updated_at_unix"] = time.time()
    return json.dumps(data, indent=2, sort_keys=True)


def snapshot_with_ts808_residual_enabled(snapshot: str, enabled: bool) -> str:
    data = json.loads(snapshot)
    ts = data.setdefault("ts808", {})
    ts["residual_matching_enabled"] = bool(enabled)
    return json.dumps(data, indent=2, sort_keys=True)


def build_renderer_exe() -> None:
    run([
        "powershell",
        "-ExecutionPolicy", "Bypass",
        "-File", "SAT-TR/tools/sat_overdrive_renderer/build_sat_overdrive_renderer.ps1",
    ])


def restore_snapshot(state_snapshot: str, baseline_snapshot: str | None, *, baseline_path: Path,
                     build_renderer: bool) -> None:
    state_path = Path("SAT-TR/tools/overdrive_voicing_state.json")
    state_path.write_text(state_snapshot, encoding="utf-8", newline="\n")
    if baseline_snapshot is not None:
        baseline_path.write_text(baseline_snapshot, encoding="utf-8", newline="\n")
    run([sys.executable, "SAT-TR/tools/write_overdrive_voicing_header.py"])
    if build_renderer:
        build_renderer_exe()


def baseline_score_from_snapshot(snapshot: str | None, *, render_plan: str, variant: str,
                                 aggregate: str, filter_model: str | None = None) -> float | None:
    if not snapshot:
        return None
    try:
        data = json.loads(snapshot)
    except Exception:
        return None
    prefix = f"plan:{Path(render_plan).name}:{variant}:"
    suffix = f":{aggregate}"
    scores: list[float] = []
    for key, value in data.items():
        if not (key.startswith(prefix) and key.endswith(suffix)):
            continue
        if filter_model is not None and f":{filter_model}:" not in key:
            continue
        try:
            scores.append(float(value["aggregate_score"]))
        except Exception:
            pass
    return min(scores) if scores else None


def best_state_score_from_snapshot(snapshot: str | None) -> float | None:
    """Return the best verified score embedded in a voicing state snapshot.

    The external baseline file is keyed by fit model. Local refine passes can
    legitimately change fit layouts, so the state itself also carries a
    hard-checkpoint score. Use it as a guard to avoid accepting a locally better
    candidate that regresses from a stronger already-exported state.
    """
    if not snapshot:
        return None
    try:
        data = json.loads(snapshot)
    except Exception:
        return None
    ts = data.get("ts808", {}) if isinstance(data, dict) else {}
    scores: list[float] = []

    for key in ("last_verified_best_score", "last_verified_reference_score"):
        try:
            value = float(ts.get(key))
        except Exception:
            continue
        if math.isfinite(value):
            scores.append(value)

    for key in ("last_plan_residual_scores", "last_verified_case_scores"):
        values = ts.get(key)
        if not isinstance(values, dict):
            continue
        for value in values.values():
            try:
                numeric = float(value)
            except Exception:
                continue
            if math.isfinite(numeric):
                scores.append(numeric)

    return min(scores) if scores else None


def fit_model_id(args, *, bands: int | None = None) -> str:
    q_tag = str(float(args.fit_basis_q)).replace(".", "p").replace("-", "m")
    gain_tag = str(float(args.fit_max_gain_db)).replace(".", "p").replace("-", "m")
    if bands is None:
        if args.fit_layout == "ndsp-band-eq":
            bands = 9
        elif args.fit_layout == "ndsp-foundation-eq":
            bands = 14
        else:
            bands = int(args.fit_bands)
    return f"dsp_{args.fit_layout}_biquad_loggrid_b{int(bands)}_q{q_tag}_g{gain_tag}"


def normalize_ts808_contract_snapshot(snapshot: str) -> tuple[str, bool]:
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    changed = False

    def set_if_changed(key: str, value):
        nonlocal changed
        if ts.get(key) != value:
            ts[key] = value
            changed = True

    set_if_changed("pre_b", list(ts.get("pre_b", []))[:2])
    set_if_changed("post_b", list(ts.get("post_b", []))[:2])
    pre_layers = ["pre_a", "pre_ndsp", "pre_b"]
    post_layers = ["post_a", "post_ndsp", "post_b"]
    residual_pre = [band for name in pre_layers for band in ts.get(name, [])]
    post_residual = [band for name in post_layers for band in ts.get(name, [])]
    set_if_changed("residual_pre", residual_pre)
    set_if_changed("post_residual", post_residual)
    contract = {
        "core": "ts808_feedback_diode_core",
        "legacy_mirrors": ["residual_pre", "post_residual"],
        "pre_order": pre_layers,
        "post_order": post_layers,
    }
    set_if_changed("voicing_cascade_contract", contract)
    return json.dumps(state, indent=2, sort_keys=True), changed


def plan_fit_score(*, render_plan: str, out_root: str, variant: str, aggregate: str,
                   allow_missing: bool, min_cases: int,
                   fit_subdir: str = "overdrive_id_fit_voiced") -> tuple[float, dict[str, float], list[str], list[str], list[str]]:
    plan = read_json(Path(render_plan))
    root = Path(out_root)
    scores: dict[str, float] = {}
    used_cases: list[str] = []
    missing_cases: list[str] = []
    filter_models: set[str] = set()

    for case in plan.get("cases", []):
        case_id = case["id"]
        summary_path = root / "overdrive_cases" / case_id / fit_subdir / "fit_summary.json"
        if not summary_path.exists():
            missing_cases.append(case_id)
            continue
        summary = read_json(summary_path)
        meta = summary.get("case_meta") or {}
        summary_variant = meta.get("sat_voicing_variant", "unknown_variant")
        if summary_variant != variant:
            raise SystemExit(
                f"verification refused {case_id}: fit variant {summary_variant!r} "
                f"does not match active variant {variant!r}"
            )
        filter_model = summary.get("filter_model", "legacy_unknown_filter_model")
        if filter_model == "legacy_unknown_filter_model":
            raise SystemExit(f"verification refused stale/legacy fit summary: {summary_path}")
        filter_models.add(filter_model)
        scores[case_id] = float(summary["score"])
        used_cases.append(case_id)

    if missing_cases and not allow_missing:
        raise SystemExit("missing fit outputs for case(s): " + ", ".join(missing_cases))
    if len(used_cases) < min_cases:
        raise SystemExit(f"only {len(used_cases)} usable fit(s), min-cases is {min_cases}")

    values = list(scores.values())
    aggregate_score = sum(values) / len(values) if aggregate == "mean" else statistics.median(values)
    return aggregate_score, scores, used_cases, missing_cases, sorted(filter_models)


def _mean(values: list[float]) -> float:
    return sum(values) / max(len(values), 1)


def hammerstein_case_report(csv_path: Path) -> dict:
    order_stats: dict[str, dict[str, list[float]]] = {}
    row_reports: list[dict] = []
    delta_sq: list[float] = []
    energy_sq: list[float] = []

    with csv_path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if str(row.get("skipped", "")).lower() == "true":
                continue
            row_delta_abs = 0.0
            row_energy_abs = 0.0
            row_orders: dict[str, dict[str, float]] = {}
            for key, value in row.items():
                if key.startswith("order_") and key.endswith("_branch_median_delta_db") and value not in (None, ""):
                    order = key.split("_branch_")[0].replace("order_", "")
                    v = float(value)
                    stats = order_stats.setdefault(order, {"delta": [], "energy_error": []})
                    stats["delta"].append(v)
                    delta_sq.append(v * v)
                    row_delta_abs += abs(v)
                    row_orders.setdefault(order, {})["median_delta_db"] = v
                if key.startswith("order_") and key.endswith("_branch_energy_sat_db") and value not in (None, ""):
                    order = key.split("_branch_")[0].replace("order_", "")
                    target_key = key.replace("_branch_energy_sat_db", "_branch_energy_target_db")
                    target_value = row.get(target_key)
                    if target_value not in (None, ""):
                        v = float(target_value) - float(value)
                        stats = order_stats.setdefault(order, {"delta": [], "energy_error": []})
                        stats["energy_error"].append(v)
                        energy_sq.append(v * v)
                        row_energy_abs += abs(v)
                        row_orders.setdefault(order, {})["energy_target_minus_sat_db"] = v
            if row_orders:
                row_reports.append({
                    "kind": row.get("kind", ""),
                    "level_dbfs_rms": float(row.get("level_dbfs_rms", 0.0) or 0.0),
                    "stem": row.get("stem", ""),
                    "score_proxy": row_delta_abs + 0.35 * row_energy_abs,
                    "orders": row_orders,
                })

    branch_score = _mean(delta_sq)
    energy_score = _mean(energy_sq) if energy_sq else 0.0
    per_order = {}
    for order, stats in sorted(order_stats.items(), key=lambda item: int(item[0])):
        deltas = stats.get("delta", [])
        energies = stats.get("energy_error", [])
        per_order[order] = {
            "mean_delta_db": _mean(deltas),
            "median_delta_db": statistics.median(deltas) if deltas else 0.0,
            "mean_abs_delta_db": _mean([abs(v) for v in deltas]),
            "rms_delta_db": (sum(v * v for v in deltas) / max(len(deltas), 1)) ** 0.5,
            "mean_energy_target_minus_sat_db": _mean(energies),
            "mean_abs_energy_error_db": _mean([abs(v) for v in energies]),
            "rms_energy_error_db": (sum(v * v for v in energies) / max(len(energies), 1)) ** 0.5,
        }
    return {
        "branch_score": branch_score,
        "energy_score": energy_score,
        "guard_score": branch_score + 0.35 * energy_score,
        "per_order": per_order,
        "worst_rows": sorted(row_reports, key=lambda item: float(item["score_proxy"]), reverse=True)[:12],
    }


def write_hammerstein_guard_report(*, root: Path, case_reports: dict[str, dict], scores: dict[str, float], aggregate_score: float) -> None:
    payload = {
        "aggregate_score": aggregate_score,
        "case_scores": scores,
        "cases": case_reports,
        "written_at_unix": time.time(),
    }
    write_json(root / "hammerstein_guard_report.json", payload)
    lines = [
        "# Hammerstein Guard Report",
        "",
        f"Aggregate guard score: `{aggregate_score:.9f}`",
        "",
    ]
    for case_id, report in case_reports.items():
        lines.append(f"## {case_id}")
        lines.append(f"Guard score: `{report['guard_score']:.9f}` | branch `{report['branch_score']:.9f}` | energy `{report['energy_score']:.9f}`")
        lines.append("")
        lines.append("| Order | Mean delta dB | RMS delta dB | Mean energy target-sat dB | RMS energy err dB |")
        lines.append("|---:|---:|---:|---:|---:|")
        for order, stats in report["per_order"].items():
            lines.append(
                f"| {order} | {stats['mean_delta_db']:.3f} | {stats['rms_delta_db']:.3f} | "
                f"{stats['mean_energy_target_minus_sat_db']:.3f} | {stats['rms_energy_error_db']:.3f} |"
            )
        lines.append("")
        lines.append("Worst rows:")
        for row in report["worst_rows"][:6]:
            lines.append(f"- `{row['stem']}` kind `{row['kind']}` level `{row['level_dbfs_rms']:.1f}` proxy `{row['score_proxy']:.3f}`")
        lines.append("")
    (root / "hammerstein_guard_report.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def hammerstein_guard_score(*, render_plan: str, out_root: str, aggregate: str,
                            allow_missing: bool, min_cases: int,
                            fit_subdir: str = "overdrive_id_fit_voiced") -> tuple[float, dict[str, float], list[str], list[str]]:
    plan = read_json(Path(render_plan))
    root = Path(out_root)
    scores: dict[str, float] = {}
    used_cases: list[str] = []
    missing_cases: list[str] = []
    case_reports: dict[str, dict] = {}

    for case in plan.get("cases", []):
        case_id = case["id"]
        csv_path = root / "overdrive_cases" / case_id / fit_subdir / "hammerstein_branch_summary.csv"
        if not csv_path.exists():
            missing_cases.append(case_id)
            continue

        report = hammerstein_case_report(csv_path)
        if not report["per_order"]:
            missing_cases.append(case_id)
            continue

        scores[case_id] = float(report["guard_score"])
        case_reports[case_id] = report
        used_cases.append(case_id)

    if missing_cases and not allow_missing:
        raise SystemExit("missing Hammerstein guard outputs for case(s): " + ", ".join(missing_cases))
    if len(used_cases) < min_cases:
        raise SystemExit(f"only {len(used_cases)} usable Hammerstein fit(s), min-cases is {min_cases}")

    values = list(scores.values())
    aggregate_score = sum(values) / len(values) if aggregate == "mean" else statistics.median(values)
    write_hammerstein_guard_report(root=root, case_reports=case_reports, scores=scores, aggregate_score=aggregate_score)
    return aggregate_score, scores, used_cases, missing_cases


def write_verified_baseline(*, baseline_path: Path, render_plan: str, variant: str, aggregate: str,
                            aggregate_score: float, scores: dict[str, float], used_cases: list[str],
                            missing_cases: list[str], filter_models: list[str], candidate: dict) -> None:
    baselines = read_json(baseline_path)
    key = f"plan:{Path(render_plan).name}:{variant}:{','.join(filter_models)}:{aggregate}"
    baselines[key] = {
        "variant": variant,
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "scores": scores,
        "aggregate_score": aggregate_score,
        "aggregate": aggregate,
        "application": "verified-post-apply",
        "candidate": candidate,
        "updated_at_unix": time.time(),
        "note": "Score measured after applying candidate residual and rerendering SAT output.",
    }
    write_json(baseline_path, baselines)


def tone_guard_stats(snapshot: str) -> dict:
    """Cheap perceptual guard against residual fits that win by hollowing tone.

    This is intentionally not a replacement for listening or Hammerstein checks.
    It only biases candidate selection away from repeated residual cuts in the
    low/body and air regions, which were the main failure mode in long TS808 runs.
    """
    try:
        ts = json.loads(snapshot).get("ts808", {})
    except Exception:
        return {"low_cut_db": 0.0, "air_cut_db": 0.0, "sub_boost_db": 0.0}

    low_cut = 0.0
    air_cut = 0.0
    sub_boost = 0.0
    for layer in ("post_a", "post_ndsp"):
        for band in ts.get(layer, []):
            kind = str(band.get("kind", "Peak"))
            freq = float(band.get("freq_hz", 0.0))
            gain = float(band.get("gain_db", 0.0))
            cut = max(0.0, -gain)
            if freq <= 300.0 and kind in {"LowShelf", "Peak"}:
                low_cut += cut
            if freq >= 8000.0 and kind in {"HighShelf", "Peak", "LowPass"}:
                air_cut += cut
    for layer in ("pre_a", "pre_b"):
        for band in ts.get(layer, []):
            kind = str(band.get("kind", "Peak"))
            freq = float(band.get("freq_hz", 0.0))
            gain = float(band.get("gain_db", 0.0))
            if freq < 60.0 and gain > 0.0 and kind in {"LowShelf", "Peak"}:
                sub_boost += gain
    return {"low_cut_db": low_cut, "air_cut_db": air_cut, "sub_boost_db": sub_boost}


def tone_guard_penalty(snapshot: str, args) -> tuple[float, dict]:
    stats = tone_guard_stats(snapshot)
    if args.tone_guard == "off":
        return 0.0, stats

    low_excess = max(0.0, stats["low_cut_db"] - float(args.tone_guard_low_cut_db))
    air_excess = max(0.0, stats["air_cut_db"] - float(args.tone_guard_air_cut_db))
    sub_boost_excess = max(0.0, stats.get("sub_boost_db", 0.0) - float(args.tone_guard_sub_boost_db))
    weight = float(args.tone_guard_weight)
    penalty = weight * ((low_excess * low_excess) + 0.5 * (air_excess * air_excess) + 2.0 * (sub_boost_excess * sub_boost_excess))
    stats = dict(stats)
    stats["low_excess_db"] = low_excess
    stats["air_excess_db"] = air_excess
    stats["sub_boost_excess_db"] = sub_boost_excess
    stats["penalty"] = penalty
    return penalty, stats


def enrich_selection_score(result: dict, args) -> dict:
    penalty, stats = tone_guard_penalty(str(result.get("state_snapshot", "")), args)
    result["tone_guard"] = stats
    result["selection_score"] = float(result["score"]) + penalty
    return result


def make_suite_cmd(args, *, out_root: str, skip_missing: bool, candidate: bool = False,
                   full_guard: bool = False, cascade_csv: str | None = None,
                   render_dir: str | None = None, voicing_state: str | None = None,
                   force_render: bool = True, render_plan: str | None = None,
                   stim_dir: str | None = None) -> list[str]:
    cmd = [
        sys.executable, "SAT-TR/tools/run_overdrive_analysis_suite.py",
        "--render-plan", render_plan or args.render_plan,
        "--stim-dir", stim_dir or args.stim_dir,
        "--render-dir", render_dir or args.render_dir,
        "--out-root", out_root,
        "--sat-renderer-exe", args.sat_renderer_exe,
    ]
    if force_render and not args.no_force_render_sat:
        cmd.append("--force-render-sat")
    if cascade_csv:
        cmd.extend(["--ts-cascade-csv", cascade_csv])
    if voicing_state:
        cmd.extend(["--voicing-state", voicing_state])
    if candidate or full_guard or (not full_guard and not args.full_analysis):
        cmd.extend(["--sat-render-mode", "voiced"])
    cmd.extend(["--fit-nfft", str(args.verify_nfft),
                "--fit-bands", str(args.fit_bands),
                "--fit-layout", args.fit_layout,
                "--fit-basis-q", str(args.fit_basis_q),
                "--fit-max-gain-db", str(args.fit_max_gain_db),
                "--fit-grid-points", str(args.fit_grid_points),
                "--foundation-prefilter-limit", str(args.foundation_prefilter_limit),
                "--foundation-exact-limit", str(args.foundation_exact_limit),
                "--feature-cache-dir", str(args.feature_cache_dir)])
    # Candidate verification must stay on the exact scoring path. The auxiliary
    # batch/matched reports are diagnostics and can fail on case-specific render
    # layouts before the residual fit is reached.
    if full_guard:
        # Full guard keeps Hammerstein diagnostics active, but skips auxiliary
        # reports; acceptance only needs overdrive_id_fit_voiced outputs.
        cmd.extend([
            "--fit-only",
            "--hammerstein-orders", str(args.full_guard_hammerstein_orders),
            "--hammerstein-taps", str(args.full_guard_hammerstein_taps),
            "--hammerstein-chunk-samples", str(args.full_guard_hammerstein_chunk_samples),
            "--no-fit-plot",
        ])
    elif candidate:
        # Candidate verification is the acceptance path, not the diagnostic path.
        # Keep it deterministic/fast even when source analysis uses --full-analysis.
        cmd.extend(["--fit-only", "--skip-hammerstein", "--no-fit-plot"])
    elif not args.full_analysis:
        cmd.extend(["--fit-only", "--skip-hammerstein"])
        if not args.keep_fit_plots:
            cmd.append("--no-fit-plot")
    if skip_missing:
        cmd.append("--skip-missing")
    return cmd


def link_or_copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    # Candidate/full-guard render dirs are disposable. Always refresh the
    # immutable target so a stale/corrupt WAV from an interrupted run cannot
    # poison later guarded analysis.
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def prepare_candidate_render_dir(args, render_dir: Path, *, source_render_dir: str | None = None, source_render_plan: str | None = None) -> None:
    """Create an isolated render dir for a candidate.

    Candidate SAT renders must not be written into the shared render directory,
    otherwise parallel workers race on the same sat_raw/sat_voiced WAV names.
    Target WAVs are immutable, so hardlink/copy them once into the worker dir.
    """
    source_dir = Path(source_render_dir or args.render_dir)
    render_dir.mkdir(parents=True, exist_ok=True)
    plan_path = source_render_plan or args.render_plan
    if plan_path:
        plan = read_json(Path(plan_path))
        for case in plan.get("cases", []):
            files = case.get("files", {})
            target_name = files.get("target")
            if target_name:
                link_or_copy_file(source_dir / target_name, render_dir / target_name)
    else:
        manifest = read_json(Path(args.stim_dir) / "manifest.json")
        names = manifest.get("batch_render_naming", {})
        target_name = names.get("target")
        if target_name:
            link_or_copy_file(source_dir / target_name, render_dir / target_name)


def parse_level_list(text: str) -> list[float]:
    out = []
    for raw in str(text).split(','):
        raw = raw.strip()
        if raw:
            out.append(float(raw))
    return out


def choose_screen_segments(manifest: dict, *, levels: list[float], max_segments: int) -> list[dict]:
    by_kind: dict[str, list[dict]] = {}
    for seg in manifest.get("segments", []):
        kind = str(seg.get("kind", ""))
        if kind:
            by_kind.setdefault(kind, []).append(seg)

    selected: list[dict] = []
    seen: set[tuple[int, int]] = set()
    for kind in sorted(by_kind.keys()):
        rows = by_kind[kind]
        for level in levels:
            best = min(rows, key=lambda s: abs(float(s.get("level_dbfs_rms", 0.0)) - level))
            key = (int(best.get("start_sample", 0)), int(best.get("end_sample", 0)))
            if key not in seen:
                selected.append(best)
                seen.add(key)
            if max_segments > 0 and len(selected) >= max_segments:
                return selected
    return selected


def build_pre_screen_pack(args) -> bool:
    if args.pre_screen_pack != "on":
        return False
    try:
        stim_dir = Path(args.stim_dir)
        render_dir = Path(args.render_dir)
        manifest_path = stim_dir / "manifest.json"
        manifest = read_json(manifest_path)
        plan = read_json(Path(args.render_plan))
        cases = plan.get("cases", [])
        if not cases:
            return False
        case = cases[0]
        files = case.get("files", {})
        dry_path = stim_dir / case.get("source_batch_file", manifest.get("batch_file", "overdrive_id_batch.wav"))
        target_path = render_dir / files.get("target", "")
        if not dry_path.exists() or not target_path.exists():
            print(json.dumps({"pre_screen_pack": False, "reason": "missing dry or target source", "dry": str(dry_path), "target": str(target_path)}, indent=2))
            return False

        screen_root = Path(args.out_root) / "screen_pack"
        screen_stim_dir = screen_root / "stimuli"
        screen_render_dir = screen_root / "renders"
        screen_plan_path = screen_root / "render_plan_ts808_screen.json"
        screen_stim_dir.mkdir(parents=True, exist_ok=True)
        screen_render_dir.mkdir(parents=True, exist_ok=True)

        dry, sr = sf.read(str(dry_path), always_2d=True, dtype="float32")
        target, target_sr = sf.read(str(target_path), always_2d=True, dtype="float32")
        if int(sr) != int(target_sr):
            raise RuntimeError(f"screen pack sample-rate mismatch: {sr} vs {target_sr}")
        total_frames = min(len(dry), len(target))
        levels = parse_level_list(args.pre_screen_levels_db)
        selected = choose_screen_segments(manifest, levels=levels, max_segments=int(args.pre_screen_max_segments))
        seg_len = max(1024, int(float(args.pre_screen_segment_seconds) * int(sr)))
        dry_parts = []
        target_parts = []
        new_segments = []
        cursor = 0
        for seg in selected:
            start = int(seg.get("start_sample", 0))
            end = min(int(seg.get("end_sample", start)), total_frames)
            if end <= start:
                continue
            available = end - start
            take = min(seg_len, available)
            seg_start = start + max(0, (available - take) // 2)
            seg_end = seg_start + take
            dry_parts.append(dry[seg_start:seg_end])
            target_parts.append(target[seg_start:seg_end])
            new_segments.append({
                "stem": f"screen_{seg.get('stem', len(new_segments))}",
                "kind": seg.get("kind"),
                "level_dbfs_rms": float(seg.get("level_dbfs_rms", 0.0)),
                "start_sample": cursor,
                "end_sample": cursor + take,
                "duration_samples": take,
                "source_start_sample": seg_start,
                "source_end_sample": seg_end,
            })
            cursor += take

        if not dry_parts:
            return False
        screen_dry = np.concatenate(dry_parts, axis=0)
        screen_target = np.concatenate(target_parts, axis=0)
        batch_name = "overdrive_id_batch.wav"
        target_name = files.get("target", "ts808_drive_drv100_in_p0__target.wav")
        sf.write(str(screen_stim_dir / batch_name), screen_dry, int(sr), subtype="FLOAT")
        sf.write(str(screen_render_dir / target_name), screen_target, int(sr), subtype="FLOAT")

        screen_manifest = dict(manifest)
        screen_manifest["batch_file"] = batch_name
        screen_manifest["segments"] = new_segments
        screen_manifest["screen_pack"] = {
            "source_manifest": str(manifest_path),
            "source_dry": str(dry_path),
            "source_target": str(target_path),
            "levels_db": levels,
            "segment_seconds": float(args.pre_screen_segment_seconds),
            "segments": len(new_segments),
            "duration_seconds": float(len(screen_dry) / float(sr)),
        }
        write_json(screen_stim_dir / "manifest.json", screen_manifest)

        screen_plan = dict(plan)
        screen_plan["preset"] = "screen-pack"
        screen_plan["preset_description"] = "Short target-aligned pack for PRE candidate screening only; final verification uses the full render plan."
        screen_plan["cases"] = [dict(case)]
        screen_plan["cases"][0]["source_batch_file"] = batch_name
        write_json(screen_plan_path, screen_plan)

        args.screen_stim_dir = str(screen_stim_dir)
        args.screen_render_dir = str(screen_render_dir)
        args.screen_render_plan = str(screen_plan_path)
        print(json.dumps({
            "pre_screen_pack": True,
            "stim_dir": args.screen_stim_dir,
            "render_dir": args.screen_render_dir,
            "render_plan": args.screen_render_plan,
            "segments": len(new_segments),
            "duration_seconds": round(float(len(screen_dry) / float(sr)), 3),
            "source_duration_seconds": round(float(total_frames / float(sr)), 3),
        }, indent=2))
        return True
    except Exception as exc:
        print(json.dumps({"pre_screen_pack": False, "fallback": "full-screen-render-plan", "reason": str(exc)}, indent=2))
        return False


def screen_plan_path(args) -> str:
    return str(getattr(args, "screen_render_plan", "") or args.render_plan)


def screen_stim_dir(args) -> str:
    return str(getattr(args, "screen_stim_dir", "") or args.stim_dir)


def screen_render_dir(args) -> str:
    return str(getattr(args, "screen_render_dir", "") or args.render_dir)


TS808_CORE_JOB_KEYS = {
    "drive_scale": "ts_drive_scale",
    "drive_headroom": "ts_drive_headroom",
    "input_gain_db": "ts_input_gain_db",
    "loop_drive_max": "ts_loop_drive_max",
    "loop_capped_gain_at_max_drive": "ts_loop_capped_gain",
    "air_gain_at_max_drive": "ts_air_gain",
    "solver_knee_start": "ts_solver_knee_start",
    "solver_pre_conduct": "ts_solver_pre_conduct",
    "upper_mid_split_hz": "ts_upper_mid_split",
    "upper_blend_lo": "ts_upper_blend_lo",
    "upper_blend_hi": "ts_upper_blend_hi",
    "upper_air_trim_lo": "ts_upper_air_trim_lo",
    "upper_air_trim_hi": "ts_upper_air_trim_hi",
    "body_feedback_hi": "ts_body_feedback_hi",
    "body_hardness_hi": "ts_body_hardness_hi",
    "body_asymmetry_lo": "ts_body_asymmetry_lo",
    "body_asymmetry_hi": "ts_body_asymmetry_hi",
    "upper_feedback_hi": "ts_upper_feedback_hi",
    "upper_hardness_hi": "ts_upper_hardness_hi",
    "upper_asymmetry_scale": "ts_upper_asymmetry_scale",
}


def ts808_core_job_fields(state_snapshot: str) -> dict:
    try:
        core = json.loads(state_snapshot).get("ts808", {}).get("core", {})
    except Exception:
        return {}
    out = {}
    for src, dst in TS808_CORE_JOB_KEYS.items():
        if src in core:
            out[dst] = str(float(core[src]))
    return out


def render_plan_cases(args) -> list[dict]:
    if args.render_plan:
        return read_json(Path(args.render_plan)).get("cases", [])
    manifest = read_json(Path(args.stim_dir) / "manifest.json")
    names = manifest.get("batch_render_naming", {})
    return [{
        "id": "default",
        "source_batch_file": manifest.get("batch_file", "overdrive_id_batch.wav"),
        "files": {
            "sat_voiced": names.get("sat_voiced_optional", manifest.get("batch_file", "overdrive_id_batch.wav").replace(".wav", "__sat_voiced.wav")),
            "target": names.get("target"),
        },
        "sat_settings": {"drive": 1.0, "type": 0.0, "knee": 0.0, "host_or_plugin_input_db": 0.0, "output_db": 0.0},
    }]



def _mono_float_audio(path: Path) -> tuple[np.ndarray, int]:
    audio, sr = sf.read(str(path), always_2d=True, dtype="float32")
    mono = np.mean(audio, axis=1, dtype=np.float64).astype(np.float32, copy=False)
    return mono, int(sr)


def _log_band_feature(audio: np.ndarray, sr: int, *, chunk_size: int = 4096, bands: int = 48) -> np.ndarray:
    if len(audio) < chunk_size:
        padded = np.zeros(chunk_size, dtype=np.float32)
        padded[:len(audio)] = audio
        audio = padded
    usable = (len(audio) // chunk_size) * chunk_size
    if usable <= 0:
        usable = chunk_size
    x = audio[:usable].reshape(-1, chunk_size).astype(np.float64, copy=False)
    x = x - np.mean(x, axis=1, keepdims=True)
    window = np.hanning(chunk_size).astype(np.float64)
    mag = np.abs(np.fft.rfft(x * window[None, :], axis=1)) + 1.0e-12
    log_mag = 20.0 * np.log10(mag)
    freqs = np.fft.rfftfreq(chunk_size, 1.0 / float(sr))
    edges = np.geomspace(40.0, min(20000.0, sr * 0.49), bands + 1)
    out = np.zeros(bands, dtype=np.float64)
    for i in range(bands):
        mask = (freqs >= edges[i]) & (freqs < edges[i + 1])
        if not np.any(mask):
            out[i] = out[i - 1] if i else 0.0
        else:
            out[i] = float(np.median(log_mag[:, mask]))
    mid = (edges[:-1] >= 100.0) & (edges[1:] <= 8000.0)
    if np.any(mid):
        out -= float(np.median(out[mid]))
    return out


def fast_pre_cascade_screen(args, *, candidates: list[dict], iteration: int, limit: int) -> list[dict]:
    if limit <= 0 or len(candidates) <= limit:
        return candidates
    cases = read_json(Path(screen_plan_path(args))).get("cases", [])
    if not cases:
        return candidates
    case = cases[0]
    files = case.get("files", {}) if isinstance(case, dict) else {}
    sat_name = files.get("sat_voiced", "ts808_drive_drv100_in_p0__sat_voiced.wav")
    target_name = files.get("target", "ts808_drive_drv100_in_p0__target.wav")
    target_path = Path(screen_render_dir(args)) / target_name
    if not target_path.exists():
        return candidates
    target, sr = _mono_float_audio(target_path)
    target_feat = _log_band_feature(target, sr)
    scored: list[tuple[float, int, dict]] = []
    failed = 0
    for index, candidate_info in enumerate(candidates):
        label = str(candidate_info["label"])
        wav_path = Path(args.out_root) / "pre_cascade_candidates" / f"iteration_{iteration}_{label}" / "renders" / sat_name
        try:
            sat, sat_sr = _mono_float_audio(wav_path)
            if sat_sr != sr:
                raise RuntimeError(f"sample-rate mismatch {sat_sr} != {sr}")
            n = min(len(sat), len(target))
            feat = _log_band_feature(sat[:n], sr)
            score = float(np.mean(np.abs(feat - target_feat)))
            scored.append((score, index, candidate_info))
        except Exception as exc:
            failed += 1
            print(json.dumps({
                "pre_cascade_fast_screen_candidate_failed": label,
                "reason": str(exc),
            }, indent=2))
    if not scored:
        return candidates
    scored.sort(key=lambda item: (item[0], item[1]))
    selected = [item[2] for item in scored[:limit]]
    print(json.dumps({
        "pre_cascade_fast_screen": True,
        "input_candidates": len(candidates),
        "selected_candidates": len(selected),
        "failed_candidates": failed,
        "metric": "median log-band spectral shape on pre-screen pack",
        "top": [
            {"label": item[2].get("candidate", {}).get("label", item[2].get("label")), "score": item[0]}
            for item in scored[:min(10, len(scored))]
        ],
        "note": "This only prunes candidates before the existing LS fit; accepted voicing still requires full verification.",
    }, indent=2))
    return selected


def write_pre_cascade_batch_render(args, *, candidates: list[dict], iteration: int) -> int:
    """Render PRE screen candidates in one renderer process.

    The scoring path stays unchanged: run_overdrive_analysis_suite still fits
    each candidate. This only removes one renderer subprocess/read pass per
    candidate before the cheap screen stage.
    """
    jobs = []
    cases = read_json(Path(screen_plan_path(args))).get("cases", [])
    stim_dir = Path(screen_stim_dir(args))
    for candidate_info in candidates:
        label = str(candidate_info["label"])
        state_snapshot = str(candidate_info["state_snapshot"])
        candidate_root_path = Path(args.out_root) / "pre_cascade_candidates" / f"iteration_{iteration}_{label}"
        cascade_csv = candidate_root_path / "candidate_cascade.csv"
        render_dir = candidate_root_path / "renders"
        metadata_state = candidate_root_path / "candidate_voicing_metadata.json"
        eval_signature = candidate_evaluation_signature(
            args,
            state_snapshot=state_snapshot,
            candidate=candidate_info.get("candidate", {"label": label}),
            base_label="pre_cascade",
        )
        if load_cached_screen_result(candidate_root_path, eval_signature) is not None:
            continue

        export_cascade_csv_from_snapshot(state_snapshot, cascade_csv)
        prepare_candidate_render_dir(args, render_dir, source_render_dir=screen_render_dir(args), source_render_plan=screen_plan_path(args))
        metadata_state.parent.mkdir(parents=True, exist_ok=True)
        metadata_state.write_text(state_snapshot, encoding="utf-8", newline="\n")
        core_fields = ts808_core_job_fields(state_snapshot)
        for case in cases:
            files = case.get("files", {})
            sat_name = files.get("sat_voiced")
            if not sat_name:
                continue
            sat_settings = case.get("sat_settings", {}) if isinstance(case, dict) else {}
            dry_name = case.get("source_batch_file") or read_json(Path(args.stim_dir) / "manifest.json").get("batch_file", "overdrive_id_batch.wav")
            job = {
                "in": str(stim_dir / dry_name),
                "out": str(render_dir / sat_name),
                "raw": "0",
                "drive": str(float(sat_settings.get("drive", 1.0))),
                "type": str(float(sat_settings.get("type", sat_settings.get("type_value", 0.0)))),
                "knee": str(float(sat_settings.get("knee", 0.0))),
                "input_db": str(float(sat_settings.get("host_or_plugin_input_db", sat_settings.get("input_db", 0.0)))),
                "output_db": str(float(sat_settings.get("output_db", 0.0))),
                "ts_cascade_csv": str(cascade_csv),
            }
            job.update(core_fields)
            jobs.append(job)

    if not jobs:
        return 0

    batch_root = Path(args.out_root) / "pre_cascade_candidates"
    batch_root.mkdir(parents=True, exist_ok=True)
    render_jobs = max(1, min(int(getattr(args, "jobs", 1)), len(jobs)))
    if render_jobs <= 1:
        batch_path = batch_root / f"iteration_{iteration}_screen_batch.json"
        batch_path.write_text(json.dumps(jobs, indent=2, sort_keys=True), encoding="utf-8", newline="\n")
        print(json.dumps({
            "pre_cascade_batch_render": True,
            "jobs": len(jobs),
            "batch_json": str(batch_path),
            "render_processes": 1,
            "note": "Screen scoring will reuse these WAVs and skip per-candidate renderer launches.",
        }, indent=2))
        run([args.sat_renderer_exe, "--batch-json", str(batch_path)])
        return len(jobs)

    chunks = [jobs[i::render_jobs] for i in range(render_jobs)]
    batch_paths = []
    for index, chunk in enumerate(chunks, start=1):
        if not chunk:
            continue
        batch_path = batch_root / f"iteration_{iteration}_screen_batch_part_{index}.json"
        batch_path.write_text(json.dumps(chunk, indent=2, sort_keys=True), encoding="utf-8", newline="\n")
        batch_paths.append(batch_path)

    print(json.dumps({
        "pre_cascade_batch_render": True,
        "jobs": len(jobs),
        "batch_json_parts": [str(p) for p in batch_paths],
        "render_processes": len(batch_paths),
        "note": "Parallel screen-pack batch rendering; outputs are independent WAV paths and scoring still verifies candidates normally.",
    }, indent=2))

    def render_part(path: Path) -> None:
        run([args.sat_renderer_exe, "--batch-json", str(path)])

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(batch_paths)) as executor:
        futures = [executor.submit(render_part, p) for p in batch_paths]
        for future in concurrent.futures.as_completed(futures):
            future.result()
    return len(jobs)


def run_suite_for_snapshot(args, *, out_root: str, skip_missing: bool, state_snapshot: str,
                           candidate: bool, label: str) -> tuple[float, dict[str, float], list[str], list[str], list[str]]:
    root = Path(out_root)
    cascade_csv = root / f"{label}_cascade.csv"
    render_dir = root / "renders"
    metadata_state = root / f"{label}_voicing_metadata.json"
    export_cascade_csv_from_snapshot(state_snapshot, cascade_csv)
    metadata_state.parent.mkdir(parents=True, exist_ok=True)
    metadata_state.write_text(state_snapshot, encoding="utf-8", newline="\n")
    prepare_candidate_render_dir(args, render_dir)
    run(make_suite_cmd(args, out_root=str(root), skip_missing=skip_missing, candidate=candidate,
                       cascade_csv=str(cascade_csv), render_dir=str(render_dir),
                       voicing_state=str(metadata_state)))
    return plan_fit_score(
        render_plan=args.render_plan,
        out_root=str(root),
        variant=args.variant,
        aggregate=args.aggregate,
        allow_missing=skip_missing,
        min_cases=args.min_cases,
    )


def run_control_fit(args, *, label: str) -> dict:
    safe_label = "".join(ch if ch.isalnum() or ch in "_-" else "_" for ch in label)
    out_dir = Path(args.out_root) / "control_fit" / safe_label
    run([
        sys.executable, "SAT-TR/tools/fit_overdrive_ts808_controls.py",
        "--render-plan", args.render_plan,
        "--render-dir", args.render_dir,
        "--sat-renderer-exe", args.sat_renderer_exe,
        "--case-id", args.control_case_id,
        "--out", str(out_dir),
    ])
    return read_json(out_dir / "control_fit_summary.json")


def clamp_float(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def control_candidate_snapshot(state_snapshot: str, control_summary: dict) -> tuple[str, dict]:
    state = json.loads(state_snapshot)
    ts = state.setdefault("ts808", {})
    core = ts.setdefault("core", {})
    best = control_summary.get("best") or {}

    drive = float(best.get("drive", 1.0))
    input_db = float(best.get("input_db", 0.0))
    output_db = float(best.get("output_db", 0.0))
    knee = float(best.get("knee", 0.0))

    old_drive_scale = float(core.get("drive_scale", 1.0))
    old_input_gain_db = float(core.get("input_gain_db", 0.0))
    new_drive_scale = clamp_float(old_drive_scale * drive, 0.55, 1.35)
    new_input_gain_db = clamp_float(old_input_gain_db + input_db, -6.0, 6.0)

    core["drive_scale"] = new_drive_scale
    core["input_gain_db"] = new_input_gain_db
    ts["last_control_fit_candidate"] = {
        "source_best": best,
        "applied_mapping": {
            "drive_scale": {"old": old_drive_scale, "new": new_drive_scale, "multiplier": drive},
            "input_gain_db": {"old": old_input_gain_db, "new": new_input_gain_db, "delta_db": input_db},
            "output_db": {"suggested_db": output_db, "applied": False, "reason": "No TS808 core output trim field exists; residual/output calibration remains explicit."},
            "knee": {"suggested": knee, "applied": False, "reason": "TS808 knee is intentionally held at 0 for this tuning pass."},
        },
        "score": best.get("score"),
    }
    return json.dumps(state, indent=2, sort_keys=True), ts["last_control_fit_candidate"]



CORE_HAMMER_SPECS = {
    # Hammerstein-first core tuning. User knee and symmetry/asymmetry are
    # intentionally frozen for this TS808 pass; only conduction, hardness,
    # feedback, drive and input can move.
    "solver_pre_conduct": {"mode": "mul", "steps": (1.06, 0.94, 1.12, 0.88), "lo": 0.02, "hi": 0.65},
    "upper_hardness_hi": {"mode": "mul", "steps": (0.92, 1.08, 0.84, 1.16), "lo": 0.05, "hi": 1.2},
    "upper_feedback_hi": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.05, "hi": 1.5},
    "body_hardness_hi": {"mode": "mul", "steps": (0.94, 1.06), "lo": 0.4, "hi": 3.5},
    "body_feedback_hi": {"mode": "mul", "steps": (1.06, 0.94), "lo": 0.4, "hi": 5.0},
    "drive_scale": {"mode": "mul", "steps": (1.025, 0.975), "lo": 0.55, "hi": 1.35},
    "drive_headroom": {"mode": "mul", "steps": (1.10, 1.20, 0.95), "lo": 1.0, "hi": 2.0},
    "input_gain_db": {"mode": "add", "steps": (0.25, -0.25), "lo": -6.0, "hi": 6.0},
}

CORE_HAMMER_PROFILES = {
    "fast": (
        "solver_pre_conduct",
        "upper_hardness_hi",
        "upper_feedback_hi",
        "drive_scale",
        "drive_headroom",
        "input_gain_db",
    ),
    "balanced": tuple(CORE_HAMMER_SPECS.keys()),
}


def core_candidate_value(current: float, spec: dict, step: float) -> float:
    if spec["mode"] == "mul":
        return clamp_float(current * step, spec["lo"], spec["hi"])
    return clamp_float(current + step, spec["lo"], spec["hi"])


def candidate_snapshot_with_core_value(snapshot: str, *, param: str, value: float, step: float) -> str:
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    core = ts.setdefault("core", {})
    old = float(core.get(param, value))
    core[param] = float(value)
    ts["last_core_hammer_candidate"] = {
        "mode": "core_hammer",
        "param": param,
        "old": old,
        "new": float(value),
        "step": float(step),
    }
    return json.dumps(state, indent=2, sort_keys=True)


def make_core_hammer_candidates(snapshot: str, *, max_candidates: int, profile: str) -> list[dict]:
    state = json.loads(snapshot)
    core = state.setdefault("ts808", {}).setdefault("core", {})
    keys = CORE_HAMMER_PROFILES.get(profile, CORE_HAMMER_PROFILES["fast"])
    candidates: list[dict] = []
    for key in keys:
        if key not in core:
            continue
        spec = CORE_HAMMER_SPECS[key]
        current = float(core[key])
        for step in spec["steps"]:
            value = core_candidate_value(current, spec, float(step))
            if abs(value - current) <= 1.0e-9:
                continue
            direction = "p" if value > current else "m"
            safe_value = str(round(value, 8)).replace("-", "m").replace(".", "p")
            label = f"core_hammer_{key}_{direction}_{safe_value}"
            candidate = {
                "label": label,
                "application": "core_hammer",
                "param": key,
                "old": current,
                "new": value,
                "step": float(step),
            }
            candidates.append({
                "label": label,
                "state_snapshot": candidate_snapshot_with_core_value(snapshot, param=key, value=value, step=float(step)),
                "candidate": candidate,
            })
            if pre_candidate_limit_reached(candidates, max_candidates):
                return dedupe_pre_cascade_candidates(candidates)
    return dedupe_pre_cascade_candidates(candidates)



PRE_LAYER_NAMES = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYER_NAMES = ("post_a", "post_ndsp", "post_b")
CASCADE_LAYER_NAMES = (*PRE_LAYER_NAMES, *POST_LAYER_NAMES)
NDSP_PRE_GRID = [
    ("LowShelf", 65.0, 0.707),
    ("Peak", 125.0, 1.0),
    ("Peak", 250.0, 1.0),
    ("Peak", 500.0, 1.0),
    ("Peak", 1000.0, 1.0),
    ("Peak", 2000.0, 1.0),
    ("Peak", 4000.0, 1.0),
    ("Peak", 8000.0, 1.0),
    ("HighShelf", 16000.0, 0.707),
]


def sync_cascade_mirrors(ts: dict) -> None:
    ts["residual_pre"] = [band for name in PRE_LAYER_NAMES for band in ts.get(name, [])]
    ts["post_residual"] = [band for name in POST_LAYER_NAMES for band in ts.get(name, [])]


def neutral_band(kind: str, freq: float, q: float, gain: float) -> dict:
    return {
        "kind": kind,
        "freq_hz": float(freq),
        "q": float(q),
        "gain_db": float(gain),
        "stages": 1,
        "amount": "Classic",
    }


def export_cascade_csv_from_snapshot(snapshot: str, path: Path) -> None:
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["layer", "kind", "freq_hz", "gain_db", "q", "stages", "amount"])
        writer.writeheader()
        for layer in CASCADE_LAYER_NAMES:
            for band in ts.get(layer, []):
                writer.writerow({
                    "layer": layer,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 1000.0)),
                    "gain_db": float(band.get("gain_db", 0.0)),
                    "q": float(band.get("q", 1.0)),
                    "stages": int(float(band.get("stages", 1))),
                    "amount": band.get("amount", "Classic"),
                })


def ensure_ndsp_pre_grid(ts: dict) -> None:
    if ts.get("pre_ndsp"):
        return
    ts["pre_ndsp"] = [neutral_band(kind, freq, q, 0.0) for kind, freq, q in NDSP_PRE_GRID]



def pre_candidate_limit_reached(candidates: list[dict], max_candidates: int) -> bool:
    return int(max_candidates) > 0 and len(candidates) >= int(max_candidates)


def candidate_snapshot_with_band_delta(snapshot: str, *, layer: str, band_index: int, gain_delta: float) -> str:
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    ensure_ndsp_pre_grid(ts)
    bands = list(ts.get(layer, []))
    if band_index < 0 or band_index >= len(bands):
        return snapshot
    band = dict(bands[band_index])
    band["gain_db"] = clamp_float(float(band.get("gain_db", 0.0)) + float(gain_delta), -12.0, 12.0)
    bands[band_index] = band
    ts[layer] = bands
    sync_cascade_mirrors(ts)
    ts["last_pre_cascade_candidate"] = {"mode": "gain_delta", "layer": layer, "band_index": band_index, "gain_delta_db": gain_delta}
    return json.dumps(state, indent=2, sort_keys=True)


def candidate_snapshot_with_band_replace(snapshot: str, *, layer: str, band_index: int, band: dict) -> str:
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    ensure_ndsp_pre_grid(ts)
    bands = list(ts.get(layer, []))
    if band_index < 0:
        return snapshot
    while len(bands) <= band_index:
        bands.append(neutral_band("Peak", 1000.0, 1.0, 0.0))
    replacement = dict(band)
    replacement["gain_db"] = clamp_float(float(replacement.get("gain_db", 0.0)), -12.0, 12.0)
    replacement["stages"] = int(float(replacement.get("stages", 1)))
    replacement["amount"] = replacement.get("amount", "Classic")
    bands[band_index] = replacement
    if layer in {"pre_a", "post_a"}:
        bands = bands[:3]
    elif layer in {"pre_b", "post_b"}:
        bands = bands[:2]
    ts[layer] = bands
    sync_cascade_mirrors(ts)
    ts["last_pre_cascade_candidate"] = {"mode": "structural_replace", "layer": layer, "band_index": band_index, "band": replacement}
    return json.dumps(state, indent=2, sort_keys=True)


def free_3eq_frequency_grid(lo_hz: float, hi_hz: float) -> tuple[float, ...]:
    """Human-resolution 3EQ frequency grid.

    3EQ is the free compensation stage. Unlike the fixed NDSP-style EQ grid,
    its frequency candidates must be able to land between musical anchor points.
    The step size follows perceptual/operational resolution: very fine in lows,
    moderate in mids, coarser in highs.
    """
    values: list[int] = []
    values.extend(range(10, 100, 1))
    values.extend(range(100, 1000, 10))
    values.extend(range(1000, 10000, 50))
    values.extend(range(10000, 20001, 100))
    return tuple(float(v) for v in values if float(lo_hz) <= float(v) <= float(hi_hz))


def quantize_3eq_frequency(freq_hz: float) -> float:
    """Snap an analysis frequency to the same human-resolution 3EQ grid."""
    freq = max(10.0, min(20000.0, float(freq_hz)))
    if freq < 100.0:
        return float(round(freq))
    if freq < 1000.0:
        return float(round(freq / 10.0) * 10)
    if freq < 10000.0:
        return float(round(freq / 50.0) * 50)
    return float(round(freq / 100.0) * 100)


def residual_priority_frequencies(args, *, out_root: str | None = None, max_freqs: int = 18) -> tuple[float, ...]:
    """Read the current source residual curve and return 3EQ-priority frequencies.

    This is only a cheap candidate-ordering hint. It never accepts a voicing;
    every selected candidate still goes through the normal render + fit + guard.
    """
    root = Path(out_root or args.out_root)
    plan = read_json(Path(args.render_plan))
    rows: list[tuple[float, float]] = []
    for case in plan.get("cases", []):
        case_id = str(case.get("id", ""))
        if not case_id:
            continue
        csv_path = root / "overdrive_cases" / case_id / "overdrive_id_fit_voiced" / "residual_curves.csv"
        if not csv_path.exists():
            continue
        try:
            with csv_path.open("r", encoding="utf-8", newline="") as f:
                for row in csv.DictReader(f):
                    freq = float(row.get("freq_hz", 0.0) or 0.0)
                    if not (40.0 <= freq <= 18000.0):
                        continue
                    static_db = abs(float(row.get("static_residual_eq_db", 0.0) or 0.0))
                    median_db = abs(float(row.get("target_minus_sat_voiced_median_db", 0.0) or 0.0))
                    dynamic_db = abs(float(row.get("high_minus_low_db", 0.0) or 0.0))
                    # Static residual drives tonal mismatch; dynamic residual keeps Hammerstein-relevant zones visible.
                    weight = max(static_db, 0.65 * median_db, 0.5 * dynamic_db)
                    rows.append((weight, quantize_3eq_frequency(freq)))
        except Exception as exc:
            print(f"WARNING: failed to read residual priority curve {csv_path}: {exc}")
    if not rows:
        return ()

    best_by_freq: dict[float, float] = {}
    for weight, freq in rows:
        best_by_freq[freq] = max(best_by_freq.get(freq, 0.0), float(weight))

    # Prevent one wide error lobe from consuming the full budget: keep local maxima
    # spread roughly by half an octave, then fall back to strongest remaining bins.
    ordered = sorted(best_by_freq.items(), key=lambda item: item[1], reverse=True)
    selected: list[float] = []
    for freq, _ in ordered:
        if all(abs(__import__("math").log2(freq / other)) >= 0.45 for other in selected):
            selected.append(freq)
        if len(selected) >= max_freqs:
            break
    # Do not force-fill adjacent bins: a shorter, well-spaced priority list is
    # faster and less biased than spending candidates on near-duplicate highs.
    return tuple(sorted(selected))


def make_pre_structural_band_specs(gain_values: tuple[float, ...], *, frequency_grid: str = "free",
                                   priority_freqs_extra: tuple[float, ...] = ()) -> list[dict]:
    specs: list[dict] = []
    shelf_qs = (0.333, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
    peak_qs = (0.333, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
    filter_qs = (0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)

    def add_gainable(kind: str, freqs: tuple[float, ...], qs: tuple[float, ...], stages_values: tuple[int, ...]) -> None:
        for freq in freqs:
            for q in qs:
                for stages in stages_values:
                    for gain in gain_values:
                        if abs(float(gain)) <= 1.0e-9:
                            continue
                        specs.append({"kind": kind, "freq_hz": freq, "q": q, "gain_db": gain, "stages": stages, "amount": "Classic"})

    def add_filter(kind: str, freqs: tuple[float, ...], qs: tuple[float, ...], stages_values: tuple[int, ...]) -> None:
        for freq in freqs:
            for q in qs:
                for stages in stages_values:
                    specs.append({"kind": kind, "freq_hz": freq, "q": q, "gain_db": 0.0, "stages": stages, "amount": "Classic"})

    # Structural 3EQ A/B PRE search: type/frequency/Q/stages/gain, not only gain deltas.
    # "free" is the intended 3EQ contract; "coarse" is kept only for quick A/B/debug.
    if frequency_grid == "coarse":
        low_shelf_freqs = (70.0, 95.0, 125.0, 180.0, 250.0, 350.0)
        highpass_freqs = (40.0, 65.0, 95.0, 125.0, 180.0, 250.0, 350.0, 500.0)
        peak_freqs = (125.0, 180.0, 250.0, 350.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0)
        tilt_freqs = (650.0, 1000.0, 1600.0, 2500.0)
        high_shelf_freqs = (2500.0, 3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 16000.0)
        lowpass_freqs = (3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 16000.0)
    else:
        low_shelf_freqs = free_3eq_frequency_grid(10.0, 600.0)
        highpass_freqs = free_3eq_frequency_grid(10.0, 800.0)
        peak_freqs = free_3eq_frequency_grid(40.0, 12000.0)
        tilt_freqs = free_3eq_frequency_grid(80.0, 8000.0)
        high_shelf_freqs = free_3eq_frequency_grid(1000.0, 20000.0)
        lowpass_freqs = free_3eq_frequency_grid(1000.0, 20000.0)

    add_gainable("LowShelf", low_shelf_freqs, shelf_qs, (1, 2))
    add_filter("HighPass", highpass_freqs, filter_qs, (1, 2))
    add_gainable("Peak", peak_freqs, peak_qs, (1, 2))
    add_gainable("TiltShelf", tilt_freqs, shelf_qs, (1, 2))
    add_gainable("HighShelf", high_shelf_freqs, shelf_qs, (1, 2))
    add_filter("LowPass", lowpass_freqs, filter_qs, (1, 2))
    # Limited runs must still cover all filter families. Put broad, musically
    # useful candidates first, interleaved by filter family, then keep the full
    # exhaustive tail available for --pre-cascade-max-candidates 0.
    priority_freqs = {65.0, 95.0, 125.0, 250.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0, 10000.0, 16000.0}
    priority_freqs.update(float(f) for f in priority_freqs_extra if 10.0 <= float(f) <= 20000.0)
    priority_qs = {0.5, 0.75, 1.0, 1.25, 1.5}
    kind_order = ("Peak", "LowShelf", "HighShelf", "HighPass", "LowPass", "TiltShelf")

    def local_key(spec: dict) -> tuple[int, float, float]:
        stage_rank = int(float(spec.get("stages", 1))) - 1
        reference_gain = abs(float(gain_values[1] if len(gain_values) > 1 else gain_values[0]))
        gain_rank = abs(abs(float(spec.get("gain_db", 0.0))) - reference_gain)
        return (stage_rank, gain_rank, float(spec.get("freq_hz", 0.0)))

    broad_by_kind = {kind: [] for kind in kind_order}
    tail: list[dict] = []
    seen: set[tuple] = set()
    for spec in specs:
        key = (spec.get("kind"), float(spec.get("freq_hz", 0.0)), float(spec.get("q", 1.0)), int(float(spec.get("stages", 1))), float(spec.get("gain_db", 0.0)))
        freq = float(spec.get("freq_hz", 0.0))
        q = float(spec.get("q", 1.0))
        kind = str(spec.get("kind", "Peak"))
        if freq in priority_freqs and q in priority_qs and kind in broad_by_kind:
            broad_by_kind[kind].append(spec)
            seen.add(key)
        else:
            tail.append(spec)

    for kind in kind_order:
        broad_by_kind[kind].sort(key=local_key)
    interleaved: list[dict] = []
    while any(broad_by_kind.values()):
        for kind in kind_order:
            if broad_by_kind[kind]:
                interleaved.append(broad_by_kind[kind].pop(0))

    tail.sort(key=lambda spec: (str(spec.get("kind", "Peak")), local_key(spec)))
    return interleaved + tail


def make_refine2_band_specs(gain_values: tuple[float, ...], *, frequency_grid: str = "free",
                            priority_freqs_extra: tuple[float, ...] = ()) -> list[dict]:
    """Two-band plateau refinement after the fixed NDSP grid."""
    specs: list[dict] = []
    shelf_qs = (0.333, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0)
    peak_qs = (0.333, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0)

    def add_gainable(kind: str, freqs: tuple[float, ...], qs: tuple[float, ...], stages_values: tuple[int, ...]) -> None:
        for freq in freqs:
            for q in qs:
                for stages in stages_values:
                    for gain in gain_values:
                        if abs(float(gain)) <= 1.0e-9:
                            continue
                        specs.append({"kind": kind, "freq_hz": freq, "q": q, "gain_db": gain, "stages": stages, "amount": "Classic"})

    if frequency_grid == "coarse":
        low_shelf_freqs = (65.0, 95.0, 125.0, 180.0, 250.0, 350.0)
        peak_freqs = (125.0, 180.0, 250.0, 350.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0)
        tilt_freqs = (650.0, 1000.0, 1600.0, 2500.0)
        high_shelf_freqs = (2500.0, 3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 16000.0)
    else:
        low_shelf_freqs = free_3eq_frequency_grid(10.0, 600.0)
        peak_freqs = free_3eq_frequency_grid(40.0, 12000.0)
        tilt_freqs = free_3eq_frequency_grid(80.0, 8000.0)
        high_shelf_freqs = free_3eq_frequency_grid(1000.0, 20000.0)

    # No HP/LP here: 2EQ is a local plateau refinement, not another foundation mover.
    add_gainable("LowShelf", low_shelf_freqs, shelf_qs, (1, 2))
    add_gainable("Peak", peak_freqs, peak_qs, (1, 2))
    add_gainable("TiltShelf", tilt_freqs, shelf_qs, (1, 2))
    add_gainable("HighShelf", high_shelf_freqs, shelf_qs, (1, 2))

    priority_freqs = {65.0, 95.0, 125.0, 250.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0, 10000.0, 16000.0}
    priority_freqs.update(float(f) for f in priority_freqs_extra if 10.0 <= float(f) <= 20000.0)
    priority_qs = {0.5, 0.75, 1.0, 1.5}
    kind_order = ("Peak", "LowShelf", "HighShelf", "TiltShelf")

    def local_key(spec: dict) -> tuple[int, float, float]:
        stage_rank = int(float(spec.get("stages", 1))) - 1
        reference_gain = abs(float(gain_values[1] if len(gain_values) > 1 else gain_values[0]))
        gain_rank = abs(abs(float(spec.get("gain_db", 0.0))) - reference_gain)
        return (stage_rank, gain_rank, float(spec.get("freq_hz", 0.0)))

    broad_by_kind = {kind: [] for kind in kind_order}
    tail: list[dict] = []
    for spec in specs:
        freq = float(spec.get("freq_hz", 0.0))
        q = float(spec.get("q", 1.0))
        kind = str(spec.get("kind", "Peak"))
        if freq in priority_freqs and q in priority_qs and kind in broad_by_kind:
            broad_by_kind[kind].append(spec)
        else:
            tail.append(spec)

    for kind in kind_order:
        broad_by_kind[kind].sort(key=local_key)
    interleaved: list[dict] = []
    while any(broad_by_kind.values()):
        for kind in kind_order:
            if broad_by_kind[kind]:
                interleaved.append(broad_by_kind[kind].pop(0))

    tail.sort(key=lambda spec: (str(spec.get("kind", "Peak")), local_key(spec)))
    return interleaved + tail



def balanced_pre_candidate_order(candidates: list[dict]) -> list[dict]:
    """Interleave PRE structural candidates so limited runs cover the full cascade.

    Without this, --pre-cascade-max-candidates can spend the whole budget on
    pre_a/slot0. The candidate set is unchanged; only the truncation order changes.
    """
    # The broad pass is ordered by observed information gain from prior runs:
    # slot 0 changes the whole following cascade most, and HP/LP/tilt/shelves
    # usually correct the foundation faster than narrow peaks.
    kind_order = ["HighPass", "LowPass", "TiltShelf", "HighShelf", "LowShelf", "Peak"]
    layer_order = ["pre_a", "pre_b"]
    slot_order = [0, 1, 2]
    buckets: dict[tuple[str, str, int], list[dict]] = {}
    leftovers: list[dict] = []

    for item in candidates:
        candidate = item.get("candidate", {})
        band = candidate.get("band", {})
        kind = str(band.get("kind", "Peak"))
        layer = str(candidate.get("layer", ""))
        slot = int(candidate.get("band_index", 0))
        if kind in kind_order and layer in layer_order and slot in slot_order:
            buckets.setdefault((kind, layer, slot), []).append(item)
        else:
            leftovers.append(item)

    ordered: list[dict] = []
    while any(buckets.values()):
        for kind in kind_order:
            for layer in layer_order:
                for slot in slot_order:
                    bucket = buckets.get((kind, layer, slot))
                    if bucket:
                        ordered.append(bucket.pop(0))

    leftovers.sort(key=lambda item: item.get("label", ""))
    ordered.extend(leftovers)
    return ordered

def make_pre_cascade_candidates(snapshot: str, *, max_candidates: int, gain_step_db: float,
                                structural: bool = True, frequency_grid: str = "free",
                                include_refine2: bool = True,
                                priority_freqs_extra: tuple[float, ...] = ()) -> list[dict]:
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    ensure_ndsp_pre_grid(ts)
    candidates: list[dict] = []
    gain_step = abs(float(gain_step_db))
    gain_values = tuple(dict.fromkeys((-2.0 * gain_step, -gain_step, gain_step, 2.0 * gain_step)))

    if structural:
        structural_specs = make_pre_structural_band_specs(
            gain_values, frequency_grid=frequency_grid, priority_freqs_extra=priority_freqs_extra)
        refine2_specs = (
            make_refine2_band_specs(gain_values, frequency_grid=frequency_grid, priority_freqs_extra=priority_freqs_extra)
            if include_refine2 else []
        )
        structural_candidates: list[dict] = []
        for layer in ("pre_a",):
            for slot in range(3):
                for spec in structural_specs:
                    sign = 'p' if float(spec.get('gain_db', 0.0)) > 0.0 else 'm'
                    label = (
                        f"{layer}_{slot}_{spec['kind']}_{spec['freq_hz']:g}_q{spec['q']:g}_"
                        f"s{spec['stages']}_{sign}{abs(float(spec.get('gain_db', 0.0))):g}db"
                    ).replace('.', 'p')
                    structural_candidates.append({
                        "label": label,
                        "layer": layer,
                        "band_index": slot,
                        "band": spec,
                        "candidate": {"label": label, "application": "pre_cascade_structural", "layer": layer, "band_index": slot, "band": spec},
                    })
        if include_refine2:
            for layer in ("pre_b",):
                for slot in range(2):
                    for spec in refine2_specs:
                        sign = 'p' if float(spec.get('gain_db', 0.0)) > 0.0 else 'm'
                        label = (
                            f"{layer}_{slot}_{spec['kind']}_{spec['freq_hz']:g}_q{spec['q']:g}_"
                            f"s{spec['stages']}_{sign}{abs(float(spec.get('gain_db', 0.0))):g}db"
                        ).replace('.', 'p')
                        structural_candidates.append({
                            "label": label,
                            "layer": layer,
                            "band_index": slot,
                            "band": spec,
                            "candidate": {"label": label, "application": "pre_cascade_refine2", "layer": layer, "band_index": slot, "band": spec},
                        })
        for candidate_info in balanced_pre_candidate_order(structural_candidates):
            layer = str(candidate_info["layer"])
            slot = int(candidate_info["band_index"])
            spec = candidate_info["band"]
            candidates.append({
                "label": candidate_info["label"],
                "state_snapshot": candidate_snapshot_with_band_replace(snapshot, layer=layer, band_index=slot, band=spec),
                "candidate": candidate_info["candidate"],
            })
            if pre_candidate_limit_reached(candidates, max_candidates):
                return candidates

    # Fixed NDSP grid gain search remains, but it is no longer the only PRE path.
    for i, band in enumerate(ts.get("pre_ndsp", [])):
        freq = float(band.get("freq_hz", 0.0))
        for delta in (-gain_step, gain_step):
            label = f"pre_ndsp_{i}_{'p' if delta > 0 else 'm'}{abs(delta):g}db".replace('.', 'p')
            candidates.append({
                "label": label,
                "state_snapshot": candidate_snapshot_with_band_delta(snapshot, layer="pre_ndsp", band_index=i, gain_delta=delta),
                "candidate": {"label": label, "application": "pre_cascade_gain", "layer": "pre_ndsp", "band_index": i, "gain_delta_db": delta, "freq_hz": freq},
            })
            if pre_candidate_limit_reached(candidates, max_candidates):
                return candidates

    for layer in ("pre_a", "pre_b"):
        for i, band in enumerate(ts.get(layer, [])):
            for delta in (-gain_step, gain_step):
                label = f"{layer}_{i}_{'p' if delta > 0 else 'm'}{abs(delta):g}db".replace('.', 'p')
                candidates.append({
                    "label": label,
                    "state_snapshot": candidate_snapshot_with_band_delta(snapshot, layer=layer, band_index=i, gain_delta=delta),
                    "candidate": {"label": label, "application": "pre_cascade_gain", "layer": layer, "band_index": i, "gain_delta_db": delta, "freq_hz": float(band.get("freq_hz", 0.0))},
                })
                if pre_candidate_limit_reached(candidates, max_candidates):
                    return candidates
    return candidates



def make_post_local_candidates(snapshot: str, *, max_candidates: int, gain_step_db: float) -> list[dict]:
    """Local POST-only refinement around the current accepted voicing.

    This intentionally does not create new bands. It only nudges existing POST
    cascade gains, so PRE/core stay frozen and the search behaves like a careful
    final EQ pass around the current best state.
    """
    state = json.loads(snapshot)
    ts = state.setdefault("ts808", {})
    sync_cascade_mirrors(ts)
    candidates: list[dict] = []
    step = abs(float(gain_step_db))
    deltas = tuple(dict.fromkeys((-step, step, -2.0 * step, 2.0 * step)))

    priority = {
        "post_a": 0,
        "post_b": 1,
        "post_ndsp": 2,
    }
    rows: list[tuple[tuple[int, float, int, float], str, int, float, dict]] = []
    for layer in POST_LAYER_NAMES:
        for index, band in enumerate(ts.get(layer, []) or []):
            freq = float(band.get("freq_hz", 1000.0))
            # Body/upper-mid/air are more likely to matter audibly than infra lows.
            if 350.0 <= freq <= 12000.0:
                freq_rank = 0.0
            elif freq < 350.0:
                freq_rank = 1.0
            else:
                freq_rank = 1.5
            for delta in deltas:
                sort_key = (priority.get(layer, 9), freq_rank, index, abs(delta))
                rows.append((sort_key, layer, index, delta, band))

    rows.sort(key=lambda item: item[0])
    for _, layer, index, delta, band in rows:
        sign = "p" if delta > 0.0 else "m"
        label = (
            f"post_local_{layer}_{index}_{band.get('kind', 'Peak')}_{float(band.get('freq_hz', 0.0)):g}_"
            f"q{float(band.get('q', 1.0)):g}_{sign}{abs(delta):g}db"
        ).replace(".", "p")
        candidate = {
            "label": label,
            "application": "post_local_gain",
            "layer": layer,
            "band_index": index,
            "gain_delta_db": float(delta),
            "freq_hz": float(band.get("freq_hz", 0.0)),
            "kind": band.get("kind", "Peak"),
        }
        candidates.append({
            "label": label,
            "state_snapshot": candidate_snapshot_with_band_delta(snapshot, layer=layer, band_index=index, gain_delta=delta),
            "candidate": candidate,
        })
        if pre_candidate_limit_reached(candidates, max_candidates):
            break
    return dedupe_pre_cascade_candidates(candidates)


def dedupe_pre_cascade_candidates(candidates: list[dict]) -> list[dict]:
    """Drop exact duplicate candidate states before any render/fit work.

    Different labels can occasionally produce the same cascade after previous
    accepted moves. Keeping the first label preserves deterministic ordering
    while avoiding redundant render + residual + guard passes.
    """
    out: list[dict] = []
    seen: set[str] = set()
    for candidate in candidates:
        snapshot = str(candidate.get("state_snapshot", ""))
        signature = hashlib.sha256(snapshot.encode("utf-8")).hexdigest()
        if signature in seen:
            continue
        seen.add(signature)
        out.append(candidate)
    return out


def run_pre_cascade_screen_candidate(args, *, candidate_info: dict, skip_missing: bool,
                                     build_renderer: bool, iteration: int,
                                     pre_rendered: bool = False) -> dict:
    """Cheap first-stage PRE candidate evaluation.

    This renders the PRE candidate and fits the best POST residual for that base,
    but it does not promote/verify the resulting full voicing. It is used only
    to choose which candidates deserve the expensive second stage.
    """
    label = str(candidate_info["label"])
    state_snapshot = str(candidate_info["state_snapshot"])
    candidate_root_path = Path(args.out_root) / "pre_cascade_candidates" / f"iteration_{iteration}_{label}"
    candidate_root = str(candidate_root_path)
    cascade_csv = candidate_root_path / "candidate_cascade.csv"
    render_dir = candidate_root_path / "renders"
    metadata_state = candidate_root_path / "candidate_voicing_metadata.json"
    eval_signature = candidate_evaluation_signature(
        args,
        state_snapshot=state_snapshot,
        candidate=candidate_info.get("candidate", {"label": label}),
        base_label="pre_cascade",
    )

    cached = load_cached_screen_result(candidate_root_path, eval_signature)
    if cached is not None:
        print(json.dumps({
            "pre_screen_cache_hit": True,
            "label": label,
            "artifact_root": candidate_root,
            "score": cached.get("score"),
        }, indent=2))
        return cached

    export_cascade_csv_from_snapshot(state_snapshot, cascade_csv)
    prepare_candidate_render_dir(args, render_dir, source_render_dir=screen_render_dir(args), source_render_plan=screen_plan_path(args))
    metadata_state.parent.mkdir(parents=True, exist_ok=True)
    metadata_state.write_text(state_snapshot, encoding="utf-8", newline="\n")
    if build_renderer:
        build_renderer_exe()
    run(make_suite_cmd(args, out_root=candidate_root, skip_missing=skip_missing,
                       candidate=True, cascade_csv=str(cascade_csv), render_dir=str(render_dir),
                       voicing_state=str(metadata_state), force_render=not pre_rendered,
                       render_plan=screen_plan_path(args), stim_dir=screen_stim_dir(args)))
    score, scores, used_cases, missing_cases, filter_models = plan_fit_score(
        render_plan=screen_plan_path(args),
        out_root=candidate_root,
        variant=args.variant,
        aggregate=args.aggregate,
        allow_missing=skip_missing,
        min_cases=args.min_cases,
    )
    result = {
        "candidate": candidate_info["candidate"],
        "score": score,
        "scores": scores,
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "filter_models": filter_models,
        "state_snapshot": state_snapshot,
        "artifact_root": candidate_root,
        "evaluation_signature": eval_signature,
        "screen_only": True,
    }
    persist_screen_candidate_artifacts(candidate_root, result)
    return result


def run_pre_cascade_fit_candidate(args, *, candidate_info: dict, baseline_snapshot: str | None,
                                  baseline_path: Path, skip_missing: bool, build_renderer: bool,
                                  iteration: int, state_path: Path) -> dict:
    """Evaluate one PRE candidate without touching the global voicing state.

    The candidate state is exported to CSV for the offline renderer. The learned
    POST residual is promoted into a per-candidate JSON file with --skip-header,
    then the complete candidate is verified from another CSV. Only the caller
    writes the winning snapshot back to the real state.
    """
    del baseline_snapshot, baseline_path, state_path  # kept in signature for call-site compatibility
    label = str(candidate_info["label"])
    state_snapshot = str(candidate_info["state_snapshot"])
    candidate_root_path = Path(args.out_root) / "pre_cascade_candidates" / f"iteration_{iteration}_{label}"
    candidate_root = str(candidate_root_path)
    cascade_csv = candidate_root_path / "candidate_cascade.csv"
    render_dir = candidate_root_path / "renders"
    temp_state = candidate_root_path / "candidate_state_for_promotion.json"
    metadata_state = candidate_root_path / "candidate_voicing_metadata.json"

    eval_signature = candidate_evaluation_signature(
        args,
        state_snapshot=state_snapshot,
        candidate=candidate_info.get("candidate", {"label": label}),
        base_label="pre_cascade",
    )
    cached = load_cached_candidate_result(candidate_root_path, eval_signature)
    if cached is not None:
        print(json.dumps({
            "candidate_cache_hit": True,
            "label": label,
            "artifact_root": candidate_root,
            "score": cached.get("score"),
        }, indent=2))
        return cached

    screen_cached = load_cached_screen_result(candidate_root_path, eval_signature)
    if screen_cached is not None:
        print(json.dumps({
            "pre_screen_reuse": True,
            "label": label,
            "artifact_root": candidate_root,
            "screen_score": screen_cached.get("score"),
        }, indent=2))
    else:
        export_cascade_csv_from_snapshot(state_snapshot, cascade_csv)
        prepare_candidate_render_dir(args, render_dir)
        metadata_state.parent.mkdir(parents=True, exist_ok=True)
        metadata_state.write_text(state_snapshot, encoding="utf-8", newline="\n")

        if build_renderer:
            build_renderer_exe()
        run(make_suite_cmd(args, out_root=candidate_root, skip_missing=skip_missing,
                           candidate=True, cascade_csv=str(cascade_csv), render_dir=str(render_dir),
                           voicing_state=str(metadata_state)))

    temp_state.parent.mkdir(parents=True, exist_ok=True)
    temp_state.write_text(state_snapshot, encoding="utf-8", newline="\n")
    promotion_ok = apply_residual_candidate(
        args,
        application="replace",
        scale=1.0,
        skip_missing=skip_missing,
        out_root=candidate_root,
        voicing_state=str(temp_state),
        skip_header=True,
        set_variant=False,
    )
    if not promotion_ok:
        result = {
            "candidate": candidate_info["candidate"],
            "score": float("inf"),
            "scores": {},
            "used_cases": [],
            "missing_cases": [],
            "filter_models": [],
            "state_snapshot": state_snapshot,
            "artifact_root": candidate_root,
            "evaluation_signature": eval_signature,
        }
        persist_candidate_artifacts(candidate_root, result)
        return result

    promoted_snapshot = temp_state.read_text(encoding="utf-8")
    verify_root = str(Path(args.out_root) / "verified_candidates" / label)
    try:
        score, scores, used_cases, missing_cases, filter_models = run_suite_for_snapshot(
            args,
            out_root=verify_root,
            skip_missing=skip_missing,
            state_snapshot=promoted_snapshot,
            candidate=True,
            label=label,
        )
    except subprocess.CalledProcessError as exc:
        print(json.dumps({
            "candidate": candidate_info["candidate"],
            "verified_score": None,
            "rejected": True,
            "reason": f"pre-cascade candidate verification failed with exit code {exc.returncode}",
        }, indent=2))
        score, scores, used_cases, missing_cases, filter_models = float("inf"), {}, [], [], []

    result = {
        "candidate": candidate_info["candidate"],
        "score": score,
        "scores": scores,
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "filter_models": filter_models,
        "state_snapshot": promoted_snapshot,
        "artifact_root": candidate_root,
        "evaluation_signature": eval_signature,
    }
    persist_candidate_artifacts(candidate_root, result)
    return result


def run_source_analysis(args, *, skip_missing: bool, label: str = "source") -> tuple[float, dict[str, float], list[str], list[str], list[str]]:
    state_snapshot = Path("SAT-TR/tools/overdrive_voicing_state.json").read_text(encoding="utf-8")
    signature = source_analysis_signature(args, state_snapshot=state_snapshot, label=label)
    cache_root = Path(args.out_root) / "source_cache" / signature[:16]
    cached = load_cached_source_result(cache_root, signature)
    if cached is not None:
        score, scores, used_cases, missing_cases, filter_models = cached
        print(json.dumps({
            "source_cache_hit": True,
            "label": label,
            "artifact_root": str(cache_root),
            "score": score,
        }, indent=2))
        return score, scores, used_cases, missing_cases, filter_models

    # Source must be measured through the same voicing-state + cascade path as
    # candidates. Otherwise the guard compares "default renderer" against
    # "candidate renderer", and can promote a local improvement that regresses
    # the actual exported voicing.
    source_root = Path(args.out_root) / "source_current" / label
    result = run_suite_for_snapshot(
        args,
        out_root=str(source_root),
        skip_missing=skip_missing,
        state_snapshot=state_snapshot,
        candidate=False,
        label=label,
    )
    persist_source_result(cache_root, evaluation_signature=signature, score=result[0], scores=result[1],
                          used_cases=result[2], missing_cases=result[3], filter_models=result[4])
    return result




def collect_wav_bytes(root: Path) -> tuple[int, int]:
    count = 0
    total = 0
    if not root.exists():
        return count, total
    for wav in root.rglob("*.wav"):
        try:
            count += 1
            total += wav.stat().st_size
        except OSError:
            pass
    return count, total


def cleanup_render_wavs(root: Path, *, dry_run: bool = False) -> dict:
    """Remove disposable candidate WAV renders while preserving JSON/state reports.

    Accepted candidate metadata, fit summaries, CSVs and state snapshots are kept.
    The WAVs are reproducible from the stored candidate state and render plan, and
    they are the dominant disk-pressure source during long tuning runs.
    """
    protected_parts = {"accepted_candidates"}
    removed = 0
    bytes_removed = 0
    kept = 0
    failed: list[str] = []
    if not root.exists():
        return {"removed": 0, "kept": 0, "bytes_removed": 0, "failed": []}
    for wav in root.rglob("*.wav"):
        rel_parts = set(wav.relative_to(root).parts)
        if rel_parts & protected_parts:
            kept += 1
            continue
        try:
            size = wav.stat().st_size
            if not dry_run:
                wav.unlink()
            removed += 1
            bytes_removed += size
        except OSError as exc:
            failed.append(f"{wav}: {exc}")
    return {
        "dry_run": dry_run,
        "removed": 0 if dry_run else removed,
        "would_remove": removed if dry_run else 0,
        "kept": kept,
        "bytes_removed": 0 if dry_run else bytes_removed,
        "bytes_would_remove": bytes_removed if dry_run else 0,
        "failed": failed[:20],
    }


def write_current_state_report(args, *, skip_missing: bool, label: str = "final_current") -> dict:
    """Authoritative quick score for the exact state currently written to disk.

    This avoids stale meta_report confusion: the report is generated after header
    export from the current overdrive_voicing_state.json and records both the
    freshly rendered score and the last verified score embedded in state metadata.
    """
    state_path = Path("SAT-TR/tools/overdrive_voicing_state.json")
    state_snapshot = state_path.read_text(encoding="utf-8")
    report_root = Path(args.out_root) / "current_state_report"
    score, scores, used_cases, missing_cases, filter_models = run_suite_for_snapshot(
        args,
        out_root=str(report_root),
        skip_missing=skip_missing,
        state_snapshot=state_snapshot,
        candidate=True,
        label=label,
    )
    try:
        state = json.loads(state_snapshot)
        ts = state.get("ts808", {})
    except Exception:
        ts = {}
    payload = {
        "current_score": score,
        "current_case_scores": scores,
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "filter_models": filter_models,
        "last_verified_best_score_in_state": ts.get("last_verified_best_score"),
        "last_verified_best_candidate_in_state": ts.get("last_verified_best_candidate"),
        "last_plan_residual_scores_in_state": ts.get("last_plan_residual_scores"),
        "state_sha256": hashlib.sha256(state_snapshot.encode("utf-8")).hexdigest(),
        "state_path": str(state_path),
        "artifact_root": str(report_root),
        "written_at_unix": time.time(),
    }
    write_json(Path(args.out_root) / "current_state_report.json", payload)
    print(json.dumps({"current_state_report": payload}, indent=2))
    return payload

def apply_residual_candidate(args, *, application: str, scale: float, skip_missing: bool,
                             out_root: str | None = None, voicing_state: str | None = None,
                             skip_header: bool = False, set_variant: bool = True) -> bool:
    cmd = [
        sys.executable, "SAT-TR/tools/promote_overdrive_plan_fit.py",
        "--render-plan", args.render_plan,
        "--out-root", out_root or args.out_root,
        "--aggregate", args.aggregate,
        "--residual-application", application,
        "--residual-delta-scale", str(scale),
        "--max-residual-gain-db", str(args.fit_max_gain_db),
        "--min-cases", str(args.min_cases),
        "--force",
        "--no-baseline-update",
    ]
    if voicing_state:
        cmd.extend(["--voicing-state", voicing_state])
    if skip_header:
        cmd.append("--skip-header")
    if skip_missing:
        cmd.append("--allow-missing")
    try:
        run(cmd)
    except subprocess.CalledProcessError as exc:
        print(json.dumps({
            "promotion_failed": True,
            "application": application,
            "scale": scale,
            "reason": f"promote_overdrive_plan_fit exited with {exc.returncode}",
        }, indent=2))
        return False
    if set_variant:
        run([sys.executable, "SAT-TR/tools/set_overdrive_ts808_variant.py", args.variant])
    return True


def verify_candidate(args, *, label: str, skip_missing: bool, build_renderer: bool,
                     state_snapshot: str | None = None) -> tuple[float, dict[str, float], list[str], list[str], list[str]]:
    verify_root = str(Path(args.out_root) / "verified_candidates" / label)
    if state_snapshot is not None:
        return run_suite_for_snapshot(
            args,
            out_root=verify_root,
            skip_missing=skip_missing,
            state_snapshot=state_snapshot,
            candidate=True,
            label=label,
        )
    if build_renderer:
        build_renderer_exe()
    run(make_suite_cmd(args, out_root=verify_root, skip_missing=skip_missing, candidate=True))
    return plan_fit_score(
        render_plan=args.render_plan,
        out_root=verify_root,
        variant=args.variant,
        aggregate=args.aggregate,
        allow_missing=skip_missing,
        min_cases=args.min_cases,
    )


def run_full_guard_analysis(args, *, label: str, state_snapshot: str, baseline_snapshot: str | None,
                            baseline_path: Path, skip_missing: bool, build_renderer: bool,
                            iteration: int) -> tuple[float, dict[str, float], list[str], list[str]]:
    """Run the expensive Hammerstein guard for one concrete voicing snapshot.

    The guard uses CSV/JSON state overrides, so it does not need to rebuild the
    offline renderer for each candidate. Identical guards are cached by exact
    state/fit signature because each call can cost minutes.
    """
    del build_renderer
    guard_root_path = Path(args.out_root) / "full_guard" / f"iteration_{iteration}_{label}"
    guard_root = str(guard_root_path)
    eval_signature = candidate_evaluation_signature(
        args,
        state_snapshot=state_snapshot,
        candidate={"label": label, "application": "full_guard"},
        base_label="full_guard",
    )
    cached = load_cached_full_guard_result(guard_root_path, eval_signature)
    if cached is not None:
        score, scores, used_cases, missing_cases = cached
        print(json.dumps({
            "full_guard_cache_hit": True,
            "label": label,
            "artifact_root": guard_root,
            "score": score,
        }, indent=2))
        return score, scores, used_cases, missing_cases

    cascade_csv = guard_root_path / f"{label}_cascade.csv"
    render_dir = guard_root_path / "renders"
    metadata_state = guard_root_path / f"{label}_voicing_metadata.json"
    export_cascade_csv_from_snapshot(state_snapshot, cascade_csv)
    prepare_candidate_render_dir(args, render_dir)
    metadata_state.parent.mkdir(parents=True, exist_ok=True)
    metadata_state.write_text(state_snapshot, encoding="utf-8", newline="\n")
    run(make_suite_cmd(args, out_root=guard_root, skip_missing=skip_missing, full_guard=True,
                       cascade_csv=str(cascade_csv), render_dir=str(render_dir),
                       voicing_state=str(metadata_state)))
    restore_snapshot(state_snapshot, baseline_snapshot, baseline_path=baseline_path, build_renderer=False)
    score, scores, used_cases, missing_cases = hammerstein_guard_score(
        render_plan=args.render_plan,
        out_root=guard_root,
        aggregate=args.aggregate,
        allow_missing=skip_missing,
        min_cases=args.min_cases,
    )
    persist_full_guard_result(guard_root_path, {
        "evaluation_signature": eval_signature,
        "score": score,
        "scores": scores,
        "used_cases": used_cases,
        "missing_cases": missing_cases,
        "label": label,
    })
    return score, scores, used_cases, missing_cases


def profile_candidate_defaults(profile: str) -> str:
    if profile == "ndsp-overwrite":
        return "replace:0.25,replace:0.5,replace:0.75,replace:1.0,replace:1.25"
    if profile == "ndsp-refine":
        # Foundation bands are learned per fit, so their layout can change.
        # Refinement must verify replacement candidates instead of adding
        # deltas onto a potentially different broad-band structure.
        return "replace:0.75,replace:1.0,replace:1.25"
    # Iteration default: verify the scales that have carried the useful
    # candidates in practice. Wider sweeps remain available via
    # --candidate-residuals when doing a final audit.
    return "replace:0.25,replace:0.5,replace:0.75"


def apply_profile_defaults(args) -> None:
    if args.profile.startswith("ndsp-"):
        args.fit_layout = "ndsp-band-eq" if args.skip_foundation_eq else "ndsp-foundation-eq"
    if args.out_root is None:
        args.out_root = "analysis_out/ts808_ndsp_band_eq" if args.profile.startswith("ndsp-") else "analysis_out/ts808_core_residual"
    if args.feature_cache_dir is None:
        args.feature_cache_dir = str(Path(args.out_root) / "_feature_cache")
    args.screen_stim_dir = ""
    args.screen_render_dir = ""
    args.screen_render_plan = ""
    if args.candidate_residuals is None:
        if args.profile == "ndsp-refine" and not is_ndsp_post_residual_layout(Path("SAT-TR/tools/overdrive_voicing_state.json")):
            print("ndsp-refine requested, but active post_residual is not NDSP layout; using ndsp-overwrite candidates for this run.")
            args.candidate_residuals = profile_candidate_defaults("ndsp-overwrite")
        else:
            args.candidate_residuals = profile_candidate_defaults(args.profile)


def parse_candidates(text: str) -> list[dict]:
    candidates = [{"label": "current", "application": "current", "scale": 0.0}]
    if text.strip().lower() in {"none", "off", "current"}:
        return candidates
    for raw in text.split(","):
        item = raw.strip()
        if not item:
            continue
        if ":" not in item:
            raise SystemExit(f"invalid candidate spec {item!r}; expected add:0.25 or replace:1.0")
        app, scale_text = item.split(":", 1)
        app = app.strip().lower()
        if app not in {"add", "replace"}:
            raise SystemExit(f"invalid candidate application {app!r}")
        scale = float(scale_text)
        label = f"{app}_{str(scale).replace('.', 'p').replace('-', 'm')}"
        candidates.append({"label": label, "application": app, "scale": scale})
    return candidates


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", choices=["tone-guard", "ndsp-overwrite", "ndsp-refine"], default="tone-guard",
                    help="tone-guard is the current broad-fit flow; ndsp-overwrite/refine use the fixed NDSP-style band EQ grid.")
    ap.add_argument("--variant", default="core-residual")
    ap.add_argument("--render-plan", default="SAT-TR/tools/overdrive_id_renders/render_plan_ts808.json")
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--render-dir", default="SAT-TR/tools/overdrive_id_renders")
    ap.add_argument("--out-root", default=None)
    ap.add_argument("--timing-log", default=None,
                    help="JSONL timing log. Default: <out-root>/iteration_timing.jsonl")
    ap.add_argument("--feature-cache-dir", default=None,
                    help="Shared target-feature cache for all candidate/full-guard analyses. Default: <out-root>/_feature_cache")
    ap.add_argument("--no-timing-log", action="store_true",
                    help="Disable per-command timing JSONL logging.")
    ap.add_argument("--final-current-rescore", choices=["fast", "off"], default="fast",
                    help="After the iteration loop, render/score the exact current state and write current_state_report.json.")
    ap.add_argument("--cleanup-render-wavs", choices=["on", "off", "dry-run"], default="on",
                    help="Remove disposable candidate WAV renders after export. JSON/state/accepted metadata are preserved.")
    ap.add_argument("--sat-renderer-exe", default="SAT-TR/tools/sat_overdrive_renderer/SatOverdriveRender.exe")
    ap.add_argument("--baseline", default="SAT-TR/tools/overdrive_fit_baselines.json")
    ap.add_argument("--skip-missing", action="store_true", default=True,
                    help="Skip cases without target renders. Enabled by default.")
    ap.add_argument("--strict-missing", action="store_true",
                    help="Fail if any render-plan case is missing target/fit files.")
    ap.add_argument("--no-build-renderer", action="store_true")
    ap.add_argument("--no-force-render-sat", action="store_true")
    ap.add_argument("--aggregate", choices=["median", "mean"], default="median")
    ap.add_argument("--min-cases", type=int, default=1)
    ap.add_argument("--min-relative-improvement", type=float, default=0.001,
                    help="Required fractional improvement for verified post-apply score.")
    ap.add_argument("--iterations", type=int, default=1,
                    help="Run multiple guarded iterations. Stops when no verified candidate improves.")
    ap.add_argument("--analysis-only", action="store_true",
                    help="Run render/analysis but do not promote residual changes.")
    ap.add_argument("--full-analysis", action="store_true",
                    help="Run full diagnostic reports for the source state. Normal iteration already uses a Hammerstein guard on accepted candidates.")
    ap.add_argument("--keep-fit-plots", action="store_true",
                    help="Keep source fit PNG plots during fast iterations. Candidate verification still skips plots.")
    ap.add_argument("--verify-nfft", type=int, default=1024,
                    help="FFT size for guarded iteration. Default is intentionally small/fast; use 8192+ only for final audit.")
    ap.add_argument("--fit-bands", type=int, default=48,
                    help="Residual peak-EQ band count when --fit-layout peak is used.")
    ap.add_argument("--fit-layout", choices=["tone", "ndsp-band-eq", "ndsp-foundation-eq", "peak"], default="ndsp-foundation-eq",
                    help="ndsp-foundation-eq is the current contract: one 3EQ foundation before the fixed NDSP grid. tone/peak are legacy diagnostics.")
    ap.add_argument("--skip-foundation-eq", action="store_true",
                    help="With ndsp-* profiles, use the fixed NDSP grid without the extra 3-band learned foundation layer.")
    ap.add_argument("--fit-basis-q", type=float, default=0.85,
                    help="Residual peak-EQ Q. Default is intentionally wide to avoid sawtooth overfitting.")
    ap.add_argument("--fit-max-gain-db", type=float, default=12.0,
                    help="Max absolute gain per residual band during fitting and promotion.")
    ap.add_argument("--fit-grid-points", type=int, default=256,
                    help="Log-grid points for residual scoring/fitting. Default is fast because tone layout is broad.")
    ap.add_argument("--foundation-prefilter-limit", type=int, default=48,
                    help="Cheap linear shortlist size for foundation-band fitting. Iteration default is 48; use 96 for slow audit.")
    ap.add_argument("--foundation-exact-limit", type=int, default=12,
                    help="Number of linearly ranked foundation candidates that receive nonlinear least-squares refit. 0 refits the full shortlist.")
    ap.add_argument("--control-fit", choices=["off", "source", "accepted"], default="accepted",
                    help="Run TS808 input/drive/output diagnostic grid on the source state or accepted state.")
    ap.add_argument("--control-case-id", default="ts808_drive_drv100_in_p0")
    ap.add_argument("--candidate-residuals", default=None,
                    help="Comma list of residual candidates to verify after source analysis. If omitted, selected from --profile.")
    ap.add_argument("--pre-cascade-fit", choices=["off", "verified"], default="verified",
                    help="verified evaluates real pre-cascade candidates through --ts-cascade-csv, fits post on top, then verifies full render.")
    ap.add_argument("--pre-cascade-max-candidates", type=int, default=48,
                    help="Maximum pre-cascade candidates to screen per iteration. Use 0 for exhaustive structural PRE search.")
    ap.add_argument("--pre-cascade-screen-limit", type=int, default=8,
                    help="Successive-halving PRE limit: after cheap screening, fully verify only the top N candidates. 0 verifies all screened candidates.")
    ap.add_argument("--pre-cascade-fast-screen-limit", type=int, default=-1,
                    help="Cheap spectral pre-screen size before LS screen. -1=auto 4x screen-limit, 0=off.")
    ap.add_argument("--pre-cascade-residual-base-tolerance", type=float, default=1.25,
                    help="Only try residual-scale verification on PRE bases with selection score <= current selection score * this factor. Use a large value to disable pruning.")
    ap.add_argument("--pre-cascade-batch-render", choices=["on", "off"], default="on",
                    help="Batch-render PRE screen SAT files in one renderer process, then score candidates without rerendering.")
    ap.add_argument("--pre-screen-pack", choices=["on", "off"], default="on",
                    help="Use a short target-aligned dry/target pack for PRE screening. Final verification still uses the full render plan.")
    ap.add_argument("--pre-screen-segment-seconds", type=float, default=1.0,
                    help="Seconds copied from the centre of each selected source segment for the PRE screen pack.")
    ap.add_argument("--pre-screen-levels-db", default="-36,-24,-12,-6",
                    help="Comma-separated source levels retained per kind in the PRE screen pack.")
    ap.add_argument("--pre-screen-max-segments", type=int, default=0,
                    help="Maximum selected PRE screen segments. 0 keeps all chosen level/kind combinations.")
    ap.add_argument("--pre-cascade-gain-step-db", type=float, default=1.5,
                    help="Gain step used for PRE structural and NDSP/grid candidates.")
    ap.add_argument("--pre-cascade-structural", choices=["on", "off"], default="on",
                    help="on tests real 3EQ A/B structural PRE candidates: kind/frequency/Q/stages/gain, not only gain deltas.")
    ap.add_argument("--pre-cascade-frequency-grid", choices=["free", "coarse"], default="free",
                    help="free uses the human-resolution 3EQ frequency grid; coarse keeps the old anchor grid for debugging.")
    ap.add_argument("--pre-cascade-residual-priority", choices=["on", "off"], default="on",
                    help="on reorders free 3EQ PRE candidates around the current residual curve before expensive renders; verification remains unchanged.")
    ap.add_argument("--pre-cascade-residual-priority-count", type=int, default=18,
                    help="Maximum residual-derived frequencies to prioritize in the free 3EQ PRE candidate order.")
    ap.add_argument("--pre-refine2", choices=["auto", "on", "off"], default="auto",
                    help="Enable the two-band PRE plateau refinement layer after the fixed NDSP grid. auto/on are currently equivalent; off disables it.")
    ap.add_argument("--post-local-fit", choices=["off", "verified"], default="off",
                    help="verified tests small gain deltas on existing post_a/post_ndsp/post_b bands with PRE/core frozen.")
    ap.add_argument("--post-local-max-candidates", type=int, default=48,
                    help="Maximum POST-local candidates to verify per iteration. 0 means all local gain deltas.")
    ap.add_argument("--post-local-gain-step-db", type=float, default=0.125,
                    help="Base POST-local gain nudge. Candidates use +/-step and +/-2*step.")
    ap.add_argument("--core-hammer-fit", choices=["off", "verified"], default="off",
                    help="verified tests small TS808 core candidates against Hammerstein first, with static score protected. Knee is not searched.")
    ap.add_argument("--core-hammer-profile", choices=["fast", "balanced"], default="fast",
                    help="Core-Hammer candidate set. fast focuses on conduction/hardness/feedback/drive/input; knee and symmetry/asymmetry stay frozen.")
    ap.add_argument("--core-hammer-max-candidates", type=int, default=24,
                    help="Maximum core-Hammer candidates to test per iteration. 0 means all candidates for the selected profile.")
    ap.add_argument("--core-hammer-static-worsening", type=float, default=0.015,
                    help="Allow this fractional static score worsening when Hammerstein improves. Keeps core fitting from destroying tone.")
    ap.add_argument("--core-hammer-relative-improvement", type=float, default=0.002,
                    help="Required fractional Hammerstein improvement for core-Hammer candidates.")
    ap.add_argument("--core-hammer-absolute-improvement", type=float, default=0.05,
                    help="Required absolute Hammerstein improvement for core-Hammer candidates.")
    ap.add_argument("--core-hammer-residual-base-candidates", type=int, default=2,
                    help="Number of Hammer-improving core candidates that may receive a post/residual refit before final acceptance. 0 disables this bridge.")
    ap.add_argument("--jobs", type=int, default=1,
                    help="Parallel PRE candidate workers. 1 is serial; use 2-4 for normal desktop runs.")
    ap.add_argument("--residual-base-max-candidates", type=int, default=6,
                    help="Maximum non-control PRE bases that receive residual-scale verification. 0 means all PRE bases.")
    ap.add_argument("--tone-guard", choices=["on", "off"], default="on",
                    help="Bias candidate selection away from residual fits that hollow low/body or air balance.")
    ap.add_argument("--tone-guard-low-cut-db", type=float, default=10.0,
                    help="Allowed accumulated low/body residual cut before selection penalty.")
    ap.add_argument("--tone-guard-air-cut-db", type=float, default=16.0,
                    help="Allowed accumulated air residual cut before selection penalty.")
    ap.add_argument("--tone-guard-sub-boost-db", type=float, default=0.25,
                    help="Allowed accumulated PRE boost below 60 Hz before selection penalty. Prevents adaptive sweeps from overfitting subgrave.")
    ap.add_argument("--tone-guard-weight", type=float, default=0.006,
                    help="Quadratic weight for tone-guard excess cuts.")
    ap.add_argument("--core-mode", choices=["verified", "off"], default="verified",
                    help="verified treats core optimization as a guarded candidate scored with the same residual metric.")
    ap.add_argument("--full-guard", choices=["on", "off"], default="on",
                    help="on runs a Hammerstein/full guard on the winning candidate before accepting it.")
    ap.add_argument("--full-guard-relative-worsening", type=float, default=0.05,
                    help="Reject if Hammerstein guard score worsens by more than this fraction.")
    ap.add_argument("--full-guard-absolute-worsening", type=float, default=0.05,
                    help="Reject if Hammerstein guard score worsens by more than this absolute amount.")
    ap.add_argument("--full-guard-hammerstein-orders", default="1,2,3,5",
                    help="Hammerstein orders for the full guard. Fast default is 1,2,3,5; use 1,2,3,5,7 for slow audit.")
    ap.add_argument("--full-guard-hammerstein-taps", type=int, default=64,
                    help="Hammerstein FIR taps for the expensive full guard. Use 64 for faster iterative guards, 128 for final audit.")
    ap.add_argument("--full-guard-hammerstein-chunk-samples", type=int, default=16384,
                    help="Rows per Hammerstein normal-equation chunk for the full guard.")
    ap.add_argument("--enable-core-optimize", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--skip-core-optimize", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--core-passes", type=int, default=1)
    ap.add_argument("--core-profile", choices=["fast", "balanced", "full"], default="fast")
    args = ap.parse_args()

    apply_profile_defaults(args)
    global RUN_TIMING_LOG
    if not args.no_timing_log:
        RUN_TIMING_LOG = Path(args.timing_log) if args.timing_log else Path(args.out_root) / "iteration_timing.jsonl"
        RUN_TIMING_LOG.parent.mkdir(parents=True, exist_ok=True)
        RUN_TIMING_LOG.write_text("", encoding="utf-8")
    skip_missing = args.skip_missing and not args.strict_missing
    baseline_path = Path(args.baseline)
    state_path = Path("SAT-TR/tools/overdrive_voicing_state.json")
    build_renderer = not args.no_build_renderer
    candidates = parse_candidates(args.candidate_residuals)
    current_filter_model = fit_model_id(args)
    build_pre_screen_pack(args)

    for iteration in range(1, args.iterations + 1):
        print(f"\n=== TS808 iteration {iteration}/{args.iterations} ===")
        raw_iteration_snapshot = state_path.read_text(encoding="utf-8")
        iteration_state_snapshot, contract_changed = normalize_ts808_contract_snapshot(raw_iteration_snapshot)
        if contract_changed:
            print(json.dumps({
                "contract_normalized": True,
                "contract": "pre_a -> pre_ndsp -> pre_b -> core -> post_a -> post_ndsp -> post_b",
                "note": "pre_b/post_b are active two-band plateau refinement layers.",
            }, indent=2))
            restore_snapshot(iteration_state_snapshot, None, baseline_path=baseline_path, build_renderer=build_renderer)
        iteration_baseline_snapshot = baseline_path.read_text(encoding="utf-8") if baseline_path.exists() else None
        previous_score = baseline_score_from_snapshot(
            iteration_baseline_snapshot,
            render_plan=args.render_plan,
            variant=args.variant,
            aggregate=args.aggregate,
            filter_model=current_filter_model,
        )

        run([sys.executable, "SAT-TR/tools/set_overdrive_ts808_variant.py", args.variant])
        if build_renderer:
            build_renderer_exe()

        source_score, source_scores, source_used, source_missing, source_models = run_source_analysis(args, skip_missing=skip_missing, label=f"iteration_{iteration}_source")
        print(json.dumps({
            "source_score_current_state": source_score,
            "previous_verified_baseline": previous_score,
            "state_best_score": best_state_score_from_snapshot(iteration_state_snapshot),
            "source_case_scores": source_scores,
        }, indent=2))
        source_control_summary = None
        if args.control_fit == "source":
            source_control_summary = run_control_fit(args, label=f"iteration_{iteration}_source")

        if args.analysis_only:
            print("\nAnalysis-only mode: candidate promotion skipped.")
            continue

        # The freshly rendered state is the local reference, but verified historical
        # scores are hard guards when present. This prevents a local source/candidate
        # pair from drifting away after a better exported checkpoint has already been
        # found. previous_score is the last verified baseline for this active state and
        # must always guard against regressions. state_best_score may be an old/global
        # checkpoint from a different scorer or renderer; keep it visible but only let
        # it guard when it is close enough to the freshly rendered source.
        reference_score = source_score
        state_best_score = best_state_score_from_snapshot(iteration_state_snapshot)
        stale_state_best = (
            state_best_score is not None
            and state_best_score < reference_score * 0.95
        )
        historical_guard_scores = [reference_score]
        if previous_score is not None:
            historical_guard_scores.append(previous_score)
        if state_best_score is not None and not stale_state_best:
            historical_guard_scores.append(state_best_score)
        historical_best_score = min([s for s in (previous_score, state_best_score) if s is not None], default=None)
        stale_historical_best = stale_state_best
        best_known_reference = min(historical_guard_scores, default=reference_score)
        global_required_score = best_known_reference * (1.0 - args.min_relative_improvement)
        global_required = global_required_score
        best = {
            "candidate": candidates[0],
            "score": source_score,
            "scores": source_scores,
            "used_cases": source_used,
            "missing_cases": source_missing,
            "filter_models": source_models,
            "state_snapshot": state_path.read_text(encoding="utf-8"),
        }
        best = enrich_selection_score(best, args)
        reference_selection_score = float(best["selection_score"])
        candidate_results = [best]
        residual_base_options = [{"label": "current", "state_snapshot": str(best["state_snapshot"]), "score": float(best["score"])}]
        pre_residual_base_candidates: list[dict] = []
        core_hammer_residual_base_candidates: list[dict] = []

        run_core_optimizer = (
            args.core_mode == "verified"
            and not args.skip_core_optimize
            and not args.analysis_only
        )
        if run_core_optimizer:
            print("\n--- Verifying core candidate ---")
            restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                             baseline_path=baseline_path, build_renderer=False)
            core_cmd = [
                sys.executable, "SAT-TR/tools/optimize_overdrive_ts808_core.py",
                "--render-plan", args.render_plan,
                "--render-dir", args.render_dir,
                "--sat-renderer-exe", args.sat_renderer_exe,
                "--passes", str(args.core_passes),
                "--profile", args.core_profile,
            ]
            core_result = run(core_cmd, allow_reject=True)
            if core_result == 0:
                try:
                    score, scores, used_cases, missing_cases, filter_models = verify_candidate(
                        args, label="core", skip_missing=skip_missing, build_renderer=False,
                        state_snapshot=state_path.read_text(encoding="utf-8"))
                except subprocess.CalledProcessError as exc:
                    score = float("inf")
                    scores = {}
                    used_cases = []
                    missing_cases = []
                    filter_models = []
                    print(json.dumps({
                        "candidate": {"label": "core", "application": "core", "scale": 0.0},
                        "verified_score": None,
                        "rejected": True,
                        "reason": f"core verification command failed with exit code {exc.returncode}",
                    }, indent=2))
                print(json.dumps({
                    "candidate": {"label": "core", "application": "core", "scale": 0.0},
                    "verified_score": score,
                    "case_scores": scores,
                }, indent=2))
                core_candidate_result = {
                    "candidate": {"label": "core", "application": "core", "scale": 0.0},
                    "score": score,
                    "scores": scores,
                    "used_cases": used_cases,
                    "missing_cases": missing_cases,
                    "filter_models": filter_models,
                    "state_snapshot": state_path.read_text(encoding="utf-8"),
                }
                core_candidate_result = enrich_selection_score(core_candidate_result, args)
                candidate_results.append(core_candidate_result)
                if score < float("inf"):
                    residual_base_options.append({"label": "core", "state_snapshot": str(core_candidate_result["state_snapshot"])})
                if float(core_candidate_result["selection_score"]) < float(best["selection_score"]):
                    best = core_candidate_result
                else:
                    restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                                     baseline_path=baseline_path, build_renderer=False)
            else:
                restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)
        else:
            print("\nCore optimizer disabled by configuration; selecting among verified residual candidates only.")

        if args.control_fit in {"source", "accepted"}:
            print("\n--- Verifying control-fit candidate ---")
            restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                             baseline_path=baseline_path, build_renderer=build_renderer)
            control_summary = source_control_summary or run_control_fit(args, label=f"iteration_{iteration}_control_candidate")
            candidate_snapshot, control_report = control_candidate_snapshot(str(best["state_snapshot"]), control_summary)
            restore_snapshot(candidate_snapshot, iteration_baseline_snapshot,
                             baseline_path=baseline_path, build_renderer=False)
            try:
                score, scores, used_cases, missing_cases, filter_models = verify_candidate(
                    args, label="control", skip_missing=skip_missing, build_renderer=False,
                    state_snapshot=state_path.read_text(encoding="utf-8"))
            except subprocess.CalledProcessError as exc:
                score = float("inf")
                scores = {}
                used_cases = []
                missing_cases = []
                filter_models = []
                print(json.dumps({
                    "candidate": {"label": "control", "application": "control", "scale": 0.0},
                    "verified_score": None,
                    "rejected": True,
                    "reason": f"control verification command failed with exit code {exc.returncode}",
                    "control_fit": control_report,
                }, indent=2))
            print(json.dumps({
                "candidate": {"label": "control", "application": "control", "scale": 0.0},
                "verified_score": score,
                "case_scores": scores,
                "control_fit": control_report,
            }, indent=2))
            control_candidate_result = {
                "candidate": {"label": "control", "application": "control", "scale": 0.0},
                "score": score,
                "scores": scores,
                "used_cases": used_cases,
                "missing_cases": missing_cases,
                "filter_models": filter_models,
                "state_snapshot": state_path.read_text(encoding="utf-8"),
            }
            control_candidate_result = enrich_selection_score(control_candidate_result, args)
            candidate_results.append(control_candidate_result)
            if score < float("inf"):
                residual_base_options.append({"label": "control", "state_snapshot": str(control_candidate_result["state_snapshot"])})
            if float(control_candidate_result["selection_score"]) < float(best["selection_score"]):
                best = control_candidate_result
            else:
                restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)

        if args.pre_cascade_fit == "verified":
            print("\n--- Verifying pre-cascade candidates ---")
            pre_source_snapshot = str(best["state_snapshot"])
            priority_freqs_extra: tuple[float, ...] = ()
            if args.pre_cascade_residual_priority == "on":
                priority_freqs_extra = residual_priority_frequencies(
                    args, max_freqs=max(0, int(args.pre_cascade_residual_priority_count)))
                print(json.dumps({
                    "pre_cascade_residual_priority": True,
                    "priority_frequencies_hz": list(priority_freqs_extra),
                    "note": "These frequencies only reorder candidate generation; render/fit/guard still decide acceptance.",
                }, indent=2))
            pre_candidates_raw = make_pre_cascade_candidates(
                pre_source_snapshot,
                max_candidates=max(0, int(args.pre_cascade_max_candidates)),
                gain_step_db=float(args.pre_cascade_gain_step_db),
                structural=args.pre_cascade_structural == "on",
                frequency_grid=args.pre_cascade_frequency_grid,
                include_refine2=args.pre_refine2 != "off",
                priority_freqs_extra=priority_freqs_extra,
            )
            pre_candidates = dedupe_pre_cascade_candidates(pre_candidates_raw)
            if len(pre_candidates) != len(pre_candidates_raw):
                print(json.dumps({
                    "pre_cascade_deduped": True,
                    "input_candidates": len(pre_candidates_raw),
                    "unique_candidates": len(pre_candidates),
                    "dropped_duplicates": len(pre_candidates_raw) - len(pre_candidates),
                }, indent=2))
            screen_limit = int(args.pre_cascade_screen_limit)
            if screen_limit > 0 and len(pre_candidates) > screen_limit:
                print(json.dumps({
                    "pre_cascade_successive_halving": True,
                    "stage": "screen",
                    "input_candidates": len(pre_candidates),
                    "top_k": screen_limit,
                    "jobs": max(1, int(args.jobs)),
                    "note": "PRE candidates are first ranked by cheap rendered fit; only top-K receive full promote/verify.",
                }, indent=2))
                screen_results: list[dict] = []
                batch_screen_rendered = False
                if args.pre_cascade_batch_render == "on":
                    try:
                        batch_screen_rendered = write_pre_cascade_batch_render(args, candidates=pre_candidates, iteration=iteration) > 0
                    except Exception as exc:
                        print(json.dumps({
                            "pre_cascade_batch_render": False,
                            "fallback": "per-candidate-render",
                            "reason": str(exc),
                        }, indent=2))
                fast_screen_limit = int(args.pre_cascade_fast_screen_limit)
                if fast_screen_limit < 0:
                    fast_screen_limit = max(screen_limit, screen_limit * 4)
                if batch_screen_rendered and fast_screen_limit > 0 and len(pre_candidates) > fast_screen_limit:
                    pre_candidates = fast_pre_cascade_screen(args, candidates=pre_candidates, iteration=iteration, limit=fast_screen_limit)
                jobs = max(1, int(args.jobs))
                if jobs > 1 and pre_candidates:
                    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
                        futures = {
                            executor.submit(
                                run_pre_cascade_screen_candidate,
                                args,
                                candidate_info=pre_candidate,
                                skip_missing=skip_missing,
                                build_renderer=False,
                                iteration=iteration,
                                pre_rendered=batch_screen_rendered,
                            ): pre_candidate
                            for pre_candidate in pre_candidates
                        }
                        for future in concurrent.futures.as_completed(futures):
                            pre_candidate = futures[future]
                            try:
                                screen_results.append(future.result())
                            except Exception as exc:
                                print(json.dumps({
                                    "candidate": pre_candidate["candidate"],
                                    "screen_score": None,
                                    "rejected": True,
                                    "reason": f"PRE screen failed: {exc}",
                                }, indent=2))
                else:
                    for pre_candidate in pre_candidates:
                        try:
                            screen_results.append(run_pre_cascade_screen_candidate(
                                args,
                                candidate_info=pre_candidate,
                                skip_missing=skip_missing,
                                build_renderer=False,
                                iteration=iteration,
                                pre_rendered=batch_screen_rendered,
                            ))
                        except Exception as exc:
                            print(json.dumps({
                                "candidate": pre_candidate["candidate"],
                                "screen_score": None,
                                "rejected": True,
                                "reason": f"PRE screen failed: {exc}",
                            }, indent=2))
                screen_results.sort(key=lambda item: float(item.get("score", float("inf"))))
                selected_labels = {str(item.get("candidate", {}).get("label", "")) for item in screen_results[:screen_limit]}
                print(json.dumps({
                    "pre_cascade_successive_halving": True,
                    "stage": "selected",
                    "selected": [
                        {"label": item.get("candidate", {}).get("label"), "score": item.get("score")}
                        for item in screen_results[:screen_limit]
                    ],
                    "discarded_count": max(0, len(screen_results) - len(screen_results[:screen_limit])),
                }, indent=2))
                pre_candidates = [
                    candidate for candidate in pre_candidates
                    if str(candidate.get("candidate", {}).get("label", "")) in selected_labels
                ]
                restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)

            def collect_pre_result(result: dict) -> None:
                nonlocal best
                result = enrich_selection_score(result, args)
                if result.get("artifact_root"):
                    persist_candidate_artifacts(result["artifact_root"], result)
                print(json.dumps({
                    "candidate": result["candidate"],
                    "verified_score": result["score"],
                    "selection_score": result["selection_score"],
                    "tone_guard": result["tone_guard"],
                    "case_scores": result["scores"],
                }, indent=2))
                candidate_results.append(result)
                if float(result["score"]) < float("inf"):
                    residual_base_allowed = float(result["selection_score"]) <= reference_selection_score * float(args.pre_cascade_residual_base_tolerance)
                    if residual_base_allowed:
                        pre_residual_base_candidates.append({
                            "label": result["candidate"]["label"],
                            "state_snapshot": str(result["state_snapshot"]),
                            "score": float(result["score"]),
                            "selection_score": float(result["selection_score"]),
                        })
                    else:
                        print(json.dumps({
                            "pre_cascade_residual_base_skipped": result["candidate"].get("label"),
                            "reason": "verified PRE base is too far worse than current state",
                            "candidate_selection_score": float(result["selection_score"]),
                            "reference_selection_score": reference_selection_score,
                            "tolerance_factor": float(args.pre_cascade_residual_base_tolerance),
                        }, indent=2))
                if float(result["selection_score"]) < float(best["selection_score"]):
                    best = result

            jobs = max(1, int(args.jobs))
            if jobs > 1 and pre_candidates:
                print(json.dumps({
                    "pre_cascade_parallel": True,
                    "jobs": jobs,
                    "candidates": len(pre_candidates),
                    "note": "Each worker uses isolated render/state files; only the selected winner updates global state.",
                }, indent=2))
                with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
                    futures = {
                        executor.submit(
                            run_pre_cascade_fit_candidate,
                            args,
                            candidate_info=pre_candidate,
                            baseline_snapshot=iteration_baseline_snapshot,
                            baseline_path=baseline_path,
                            skip_missing=skip_missing,
                            build_renderer=False,
                            iteration=iteration,
                            state_path=state_path,
                        ): pre_candidate
                        for pre_candidate in pre_candidates
                    }
                    for future in concurrent.futures.as_completed(futures):
                        pre_candidate = futures[future]
                        try:
                            collect_pre_result(future.result())
                        except Exception as exc:
                            result = {
                                "candidate": pre_candidate["candidate"],
                                "score": float("inf"),
                                "scores": {},
                                "used_cases": [],
                                "missing_cases": [],
                                "filter_models": [],
                                "state_snapshot": pre_candidate["state_snapshot"],
                                "error": str(exc),
                            }
                            print(json.dumps({
                                "candidate": pre_candidate["candidate"],
                                "verified_score": None,
                                "rejected": True,
                                "reason": f"parallel PRE candidate failed: {exc}",
                            }, indent=2))
                            collect_pre_result(result)
                restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)
            else:
                for pre_candidate in pre_candidates:
                    print(f"\n--- Verifying pre-cascade candidate {pre_candidate['label']} ---")
                    result = run_pre_cascade_fit_candidate(
                        args,
                        candidate_info=pre_candidate,
                        baseline_snapshot=iteration_baseline_snapshot,
                        baseline_path=baseline_path,
                        skip_missing=skip_missing,
                        build_renderer=False,
                        iteration=iteration,
                        state_path=state_path,
                    )
                    collect_pre_result(result)
                restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)


        if args.post_local_fit == "verified":
            print("\n--- Verifying post-local candidates ---")
            post_source_snapshot = str(best["state_snapshot"])
            post_candidates = make_post_local_candidates(
                post_source_snapshot,
                max_candidates=max(0, int(args.post_local_max_candidates)),
                gain_step_db=float(args.post_local_gain_step_db),
            )
            print(json.dumps({
                "post_local_fit": True,
                "candidates": len(post_candidates),
                "gain_step_db": float(args.post_local_gain_step_db),
                "note": "POST-only local gain nudges; PRE/core/control remain frozen.",
            }, indent=2))
            def evaluate_post_local_candidate(post_candidate: dict) -> dict:
                label = str(post_candidate["label"])
                verified_root = Path(args.out_root) / "verified_candidates" / label
                eval_signature = candidate_evaluation_signature(
                    args,
                    state_snapshot=str(post_candidate["state_snapshot"]),
                    candidate=post_candidate.get("candidate", {"label": label}),
                    base_label="post_local",
                )
                cached = load_cached_candidate_result(verified_root, eval_signature)
                if cached is not None:
                    result = enrich_selection_score(cached, args)
                    result["post_local_cache_hit"] = True
                    return result
                try:
                    score, scores, used_cases, missing_cases, filter_models = run_suite_for_snapshot(
                        args,
                        out_root=str(verified_root),
                        skip_missing=skip_missing,
                        state_snapshot=str(post_candidate["state_snapshot"]),
                        candidate=True,
                        label=label,
                    )
                except subprocess.CalledProcessError as exc:
                    result = {
                        "candidate": post_candidate["candidate"],
                        "score": float("inf"),
                        "scores": {},
                        "used_cases": [],
                        "missing_cases": [],
                        "filter_models": [],
                        "state_snapshot": str(post_candidate["state_snapshot"]),
                        "evaluation_signature": eval_signature,
                        "error": f"post-local verification failed with exit code {exc.returncode}",
                    }
                else:
                    result = {
                        "candidate": post_candidate["candidate"],
                        "score": score,
                        "scores": scores,
                        "used_cases": used_cases,
                        "missing_cases": missing_cases,
                        "filter_models": filter_models,
                        "state_snapshot": str(post_candidate["state_snapshot"]),
                        "evaluation_signature": eval_signature,
                    }
                result = enrich_selection_score(result, args)
                persist_candidate_artifacts(verified_root, result)
                return result

            post_results = []
            jobs = max(1, int(args.jobs))
            if jobs > 1 and post_candidates:
                print(json.dumps({
                    "post_local_parallel": True,
                    "jobs": jobs,
                    "candidates": len(post_candidates),
                    "note": "POST-local candidates are independent and use isolated render dirs.",
                }, indent=2))
                with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
                    futures = {executor.submit(evaluate_post_local_candidate, pc): pc for pc in post_candidates}
                    for future in concurrent.futures.as_completed(futures):
                        try:
                            post_results.append(future.result())
                        except Exception as exc:
                            pc = futures[future]
                            post_results.append({
                                "candidate": pc.get("candidate", {}),
                                "score": float("inf"),
                                "selection_score": float("inf"),
                                "scores": {},
                                "used_cases": [],
                                "missing_cases": [],
                                "filter_models": [],
                                "state_snapshot": str(pc.get("state_snapshot", "")),
                                "error": f"parallel post-local candidate failed: {exc}",
                            })
            else:
                for post_candidate in post_candidates:
                    post_results.append(evaluate_post_local_candidate(post_candidate))

            post_results.sort(key=lambda item: float(item.get("selection_score", item.get("score", float("inf")))))
            for result in post_results:
                if result.get("post_local_cache_hit"):
                    print(json.dumps({
                        "post_local_cache_hit": True,
                        "candidate": result["candidate"],
                        "verified_score": result["score"],
                        "selection_score": result["selection_score"],
                    }, indent=2))
                else:
                    print(json.dumps({
                        "candidate": result["candidate"],
                        "verified_score": result["score"],
                        "selection_score": result["selection_score"],
                        "tone_guard": result.get("tone_guard"),
                        "case_scores": result.get("scores", {}),
                    }, indent=2))
                candidate_results.append(result)
                if float(result["selection_score"]) < float(best["selection_score"]):
                    best = result
            restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                             baseline_path=baseline_path, build_renderer=False)


        if args.core_hammer_fit == "verified":
            print("\n--- Verifying core-Hammer candidates ---")
            core_source_snapshot = str(best["state_snapshot"])
            core_candidates = make_core_hammer_candidates(
                core_source_snapshot,
                max_candidates=max(0, int(args.core_hammer_max_candidates)),
                profile=args.core_hammer_profile,
            )
            print(json.dumps({
                "core_hammer_fit": True,
                "candidates": len(core_candidates),
                "profile": args.core_hammer_profile,
                "static_worsening": float(args.core_hammer_static_worsening),
                "relative_improvement": float(args.core_hammer_relative_improvement),
                "absolute_improvement": float(args.core_hammer_absolute_improvement),
                "note": "Hammerstein-first TS808 core search; knee and symmetry/asymmetry are excluded, PRE/POST are frozen.",
            }, indent=2))
            if core_candidates:
                source_hammer_for_core, source_hammer_case_scores, _, _ = run_full_guard_analysis(
                    args,
                    label="source_core_hammer",
                    state_snapshot=core_source_snapshot,
                    baseline_snapshot=iteration_baseline_snapshot,
                    baseline_path=baseline_path,
                    skip_missing=skip_missing,
                    build_renderer=build_renderer,
                    iteration=iteration,
                )
                hammer_required = min(
                    source_hammer_for_core * (1.0 - float(args.core_hammer_relative_improvement)),
                    source_hammer_for_core - float(args.core_hammer_absolute_improvement),
                )
                static_allowed = min(
                    reference_score * (1.0 + float(args.core_hammer_static_worsening)),
                    global_required,
                )
                for core_candidate in core_candidates:
                    label = str(core_candidate["label"])
                    hammer_score, hammer_scores, _, _ = run_full_guard_analysis(
                        args,
                        label=f"candidate_{label}",
                        state_snapshot=str(core_candidate["state_snapshot"]),
                        baseline_snapshot=iteration_baseline_snapshot,
                        baseline_path=baseline_path,
                        skip_missing=skip_missing,
                        build_renderer=build_renderer,
                        iteration=iteration,
                    )
                    hammer_ok = hammer_score <= hammer_required
                    result = {
                        "candidate": core_candidate["candidate"],
                        "score": float("inf"),
                        "scores": {},
                        "used_cases": [],
                        "missing_cases": [],
                        "filter_models": [],
                        "state_snapshot": str(core_candidate["state_snapshot"]),
                        "core_hammer_source_score": source_hammer_for_core,
                        "core_hammer_score": hammer_score,
                        "core_hammer_required_score": hammer_required,
                        "core_hammer_case_scores": hammer_scores,
                        "core_hammer_source_case_scores": source_hammer_case_scores,
                        "core_hammer_static_allowed_score": static_allowed,
                        "core_hammer_eligible": False,
                    }
                    if hammer_ok:
                        verified_root = Path(args.out_root) / "core_hammer_candidates" / label
                        try:
                            score, scores, used_cases, missing_cases, filter_models = run_suite_for_snapshot(
                                args,
                                out_root=str(verified_root),
                                skip_missing=skip_missing,
                                state_snapshot=str(core_candidate["state_snapshot"]),
                                candidate=True,
                                label=label,
                            )
                        except subprocess.CalledProcessError as exc:
                            result["error"] = f"core-Hammer static verification failed with exit code {exc.returncode}"
                        else:
                            result.update({
                                "score": score,
                                "scores": scores,
                                "used_cases": used_cases,
                                "missing_cases": missing_cases,
                                "filter_models": filter_models,
                            })
                            result = enrich_selection_score(result, args)
                            result["core_hammer_eligible"] = (
                                float(result["score"]) <= static_allowed
                                and float(result.get("selection_score", result["score"])) <= static_allowed
                            )
                            # Important: a Hammer-improving core move can make the
                            # existing post residual temporarily worse. Do not accept it
                            # directly, but do allow the normal residual stage to refit
                            # POST on top of this core snapshot and judge the combined result.
                            if hammer_ok and int(args.core_hammer_residual_base_candidates) != 0:
                                core_hammer_residual_base_candidates.append({
                                    "label": label,
                                    "state_snapshot": str(core_candidate["state_snapshot"]),
                                    "score": float(result.get("score", float("inf"))),
                                    "selection_score": float(result.get("selection_score", result.get("score", float("inf")))),
                                    "hammer_score": float(hammer_score),
                                    "hammer_source_score": float(source_hammer_for_core),
                                    "candidate": result["candidate"],
                                })
                            persist_candidate_artifacts(verified_root, result)
                    if "selection_score" not in result:
                        result["selection_score"] = result["score"]
                    print(json.dumps({
                        "candidate": result["candidate"],
                        "hammer_score": hammer_score,
                        "hammer_required": hammer_required,
                        "hammer_ok": hammer_ok,
                        "static_score": result["score"],
                        "static_allowed": static_allowed,
                        "eligible": result.get("core_hammer_eligible", False),
                        "case_scores": result.get("scores", {}),
                    }, indent=2))
                    candidate_results.append(result)
            restore_snapshot(str(best["state_snapshot"]), iteration_baseline_snapshot,
                             baseline_path=baseline_path, build_renderer=False)


        if pre_residual_base_candidates:
            pre_residual_base_candidates.sort(key=lambda item: float(item.get("selection_score", item.get("score", float("inf")))))
            max_pre_residual_bases = int(args.residual_base_max_candidates)
            selected_pre_bases = (pre_residual_base_candidates if max_pre_residual_bases <= 0
                                  else pre_residual_base_candidates[:max_pre_residual_bases])
            residual_base_options.extend(selected_pre_bases)
            print(json.dumps({
                "pre_residual_base_selection": {
                    "available": len(pre_residual_base_candidates),
                    "selected": len(selected_pre_bases),
                    "max": max_pre_residual_bases,
                    "selected_labels": [item["label"] for item in selected_pre_bases],
                    "selected_scores": {item["label"]: item["score"] for item in selected_pre_bases},
                    "selected_selection_scores": {item["label"]: item.get("selection_score", item["score"]) for item in selected_pre_bases},
                }
            }, indent=2))

        if core_hammer_residual_base_candidates:
            core_hammer_residual_base_candidates.sort(key=lambda item: (
                float(item.get("hammer_score", float("inf"))),
                float(item.get("selection_score", item.get("score", float("inf"))))
            ))
            max_core_hammer_bases = int(args.core_hammer_residual_base_candidates)
            selected_core_hammer_bases = (
                core_hammer_residual_base_candidates if max_core_hammer_bases <= 0
                else core_hammer_residual_base_candidates[:max_core_hammer_bases]
            )
            residual_base_options.extend({
                "label": f"core_{item['label']}",
                "state_snapshot": item["state_snapshot"],
                "score": item["score"],
                "selection_score": item.get("selection_score", item["score"]),
                "hammer_score": item.get("hammer_score"),
            } for item in selected_core_hammer_bases)
            print(json.dumps({
                "core_hammer_residual_base_selection": {
                    "available": len(core_hammer_residual_base_candidates),
                    "selected": len(selected_core_hammer_bases),
                    "max": max_core_hammer_bases,
                    "selected_labels": [item["label"] for item in selected_core_hammer_bases],
                    "selected_static_scores": {item["label"]: item["score"] for item in selected_core_hammer_bases},
                    "selected_hammer_scores": {item["label"]: item.get("hammer_score") for item in selected_core_hammer_bases},
                }
            }, indent=2))

        has_replace_candidates = any(candidate["application"] == "replace" for candidate in candidates[1:])
        for base_option in residual_base_options:
            base_label = str(base_option["label"])
            residual_base_snapshot = str(base_option["state_snapshot"])
            candidate_prefix = "" if base_label == "current" else f"{base_label}_"

            base_fit_root = Path(args.out_root) / "residual_base_fits" / base_label
            if has_replace_candidates:
                print(f"\n--- Preparing absolute residual fit from residual-off base: {base_label} ---")
                fit_source_snapshot = snapshot_with_ts808_residual_enabled(residual_base_snapshot, False)
                # Critical: each residual base must get its own fit root. A core-Hammer
                # candidate changes the nonlinear core, so reusing the global/current
                # fit would judge `core + old post`, not `core + refit post`.
                fit_source_score, fit_source_scores, _, _, _ = run_suite_for_snapshot(
                    args,
                    out_root=str(base_fit_root),
                    skip_missing=skip_missing,
                    state_snapshot=fit_source_snapshot,
                    candidate=True,
                    label=f"iteration_{iteration}_{base_label}_residual_off",
                )
                print(json.dumps({
                    "fit_source": f"{base_label}_residual_off_base_for_replace",
                    "fit_source_root": str(base_fit_root),
                    "fit_source_score": fit_source_score,
                    "fit_source_case_scores": fit_source_scores,
                }, indent=2))
                # The residual-off state is only a measurement source. Restore
                # the actual candidate base immediately so normal exits never
                # leave the working voicing with residual matching disabled.
                restore_snapshot(residual_base_snapshot, iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)

            for candidate in candidates[1:]:
                label = candidate_prefix + candidate["label"]
                candidate_report = dict(candidate)
                candidate_report["label"] = label
                candidate_report["base"] = base_label
                verified_root = Path(args.out_root) / "verified_candidates" / label
                eval_signature = candidate_evaluation_signature(
                    args,
                    state_snapshot=residual_base_snapshot,
                    candidate=candidate_report,
                    base_label=base_label,
                )
                cached = load_cached_candidate_result(verified_root, eval_signature)
                if cached is not None:
                    cached = enrich_selection_score(cached, args)
                    print(json.dumps({
                        "candidate_cache_hit": True,
                        "candidate": candidate_report,
                        "verified_score": cached.get("score"),
                        "selection_score": cached.get("selection_score", cached.get("score")),
                        "artifact_root": str(verified_root),
                    }, indent=2))
                    candidate_results.append(cached)
                    if float(cached.get("selection_score", cached.get("score", float("inf")))) < float(best["selection_score"]):
                        best = cached
                    continue

                print(f"\n--- Verifying candidate {label} ---")
                restore_snapshot(residual_base_snapshot, iteration_baseline_snapshot,
                                 baseline_path=baseline_path, build_renderer=False)
                promotion_ok = apply_residual_candidate(
                    args,
                    application=candidate["application"],
                    scale=float(candidate["scale"]),
                    skip_missing=skip_missing,
                    out_root=str(base_fit_root),
                )
                if not promotion_ok:
                    residual_candidate_result = {
                        "candidate": candidate_report,
                        "score": float("inf"),
                        "scores": {},
                        "used_cases": [],
                        "missing_cases": [],
                        "filter_models": [],
                        "state_snapshot": residual_base_snapshot,
                        "evaluation_signature": eval_signature,
                    }
                    candidate_results.append(residual_candidate_result)
                    continue
                try:
                    score, scores, used_cases, missing_cases, filter_models = verify_candidate(
                        args, label=label, skip_missing=skip_missing, build_renderer=False,
                        state_snapshot=state_path.read_text(encoding="utf-8"))
                except subprocess.CalledProcessError as exc:
                    score = float("inf")
                    scores = {}
                    used_cases = []
                    missing_cases = []
                    filter_models = []
                    print(json.dumps({
                        "candidate": candidate_report,
                        "verified_score": None,
                        "rejected": True,
                        "reason": f"candidate verification command failed with exit code {exc.returncode}",
                    }, indent=2))
                residual_candidate_result = {
                    "candidate": candidate_report,
                    "score": score,
                    "scores": scores,
                    "used_cases": used_cases,
                    "missing_cases": missing_cases,
                    "filter_models": filter_models,
                    "state_snapshot": state_path.read_text(encoding="utf-8"),
                    "evaluation_signature": eval_signature,
                }
                residual_candidate_result = enrich_selection_score(residual_candidate_result, args)
                print(json.dumps({
                    "candidate": candidate_report,
                    "verified_score": score,
                    "selection_score": residual_candidate_result["selection_score"],
                    "tone_guard": residual_candidate_result["tone_guard"],
                    "case_scores": scores,
                }, indent=2))
                persist_candidate_artifacts(Path(args.out_root) / "verified_candidates" / label, residual_candidate_result)
                candidate_results.append(residual_candidate_result)
                if float(residual_candidate_result["selection_score"]) < float(best["selection_score"]):
                    best = residual_candidate_result

        # A candidate must improve both the local current state and the best known
        # verified baseline for this filter model. The global gate is deliberately
        # strict: if a candidate helps Hammerstein but worsens the final static fit
        # versus the best known checkpoint, it must not become the new state.
        required = reference_selection_score * (1.0 - args.min_relative_improvement)
        def regular_candidate_eligible(result: dict) -> bool:
            return (
                result["candidate"]["label"] != "current"
                and result.get("candidate", {}).get("application") != "core_hammer"
                and float(result.get("score", float("inf"))) <= reference_score * (1.0 - args.min_relative_improvement)
                and float(result.get("selection_score", result["score"])) <= required
                and float(result.get("score", float("inf"))) <= global_required
                and float(result.get("selection_score", result["score"])) <= global_required
            )

        def core_hammer_candidate_eligible(result: dict) -> bool:
            if result.get("candidate", {}).get("application") != "core_hammer":
                return False
            return (
                bool(result.get("core_hammer_eligible", False))
                and float(result.get("score", float("inf"))) <= global_required
                and float(result.get("selection_score", result.get("score", float("inf")))) <= global_required
            )

        eligible = [
            result for result in candidate_results
            if regular_candidate_eligible(result) or core_hammer_candidate_eligible(result)
        ]

        def eligible_sort_key(result: dict) -> float:
            if result.get("candidate", {}).get("application") == "core_hammer":
                source = max(float(result.get("core_hammer_source_score", 1.0)), 1.0e-9)
                hammer_ratio = float(result.get("core_hammer_score", source)) / source
                static_ratio = float(result.get("selection_score", result.get("score", reference_score))) / max(reference_selection_score, 1.0e-9)
                return hammer_ratio + max(0.0, static_ratio - 1.0)
            return float(result.get("selection_score", result["score"])) / max(reference_selection_score, 1.0e-9)

        eligible.sort(key=eligible_sort_key)

        source_hammer_score = None
        source_hammer_scores = {}
        accepted_result = None
        rejected_by_full_guard = []

        for result in eligible:
            if args.full_guard == "on":
                if source_hammer_score is None:
                    source_hammer_score, source_hammer_scores, _, _ = run_full_guard_analysis(
                        args,
                        label="source",
                        state_snapshot=iteration_state_snapshot,
                        baseline_snapshot=iteration_baseline_snapshot,
                        baseline_path=baseline_path,
                        skip_missing=skip_missing,
                        build_renderer=build_renderer,
                        iteration=iteration,
                    )
                candidate_label = str(result["candidate"]["label"])
                candidate_hammer_score, candidate_hammer_scores, _, _ = run_full_guard_analysis(
                    args,
                    label=f"candidate_{candidate_label}",
                    state_snapshot=str(result["state_snapshot"]),
                    baseline_snapshot=iteration_baseline_snapshot,
                    baseline_path=baseline_path,
                    skip_missing=skip_missing,
                    build_renderer=build_renderer,
                    iteration=iteration,
                )
                allowed_hammer_score = (
                    source_hammer_score * (1.0 + args.full_guard_relative_worsening)
                    + args.full_guard_absolute_worsening
                )
                full_guard_report = {
                    "candidate": result["candidate"],
                    "source_hammer_score": source_hammer_score,
                    "candidate_hammer_score": candidate_hammer_score,
                    "allowed_hammer_score": allowed_hammer_score,
                    "source_hammer_case_scores": source_hammer_scores,
                    "candidate_hammer_case_scores": candidate_hammer_scores,
                }
                if candidate_hammer_score <= allowed_hammer_score:
                    print(json.dumps({"full_guard": "passed", **full_guard_report}, indent=2))
                    accepted_result = result
                    break
                print(json.dumps({"full_guard": "rejected", **full_guard_report}, indent=2))
                rejected_by_full_guard.append(full_guard_report)
            else:
                accepted_result = result
                break

        if accepted_result is not None:
            accepted_score = float(accepted_result.get("score", float("inf")))
            accepted_selection = float(accepted_result.get("selection_score", accepted_score))
            if accepted_result.get("candidate", {}).get("application") == "core_hammer":
                static_allowed = reference_score * (1.0 + float(args.core_hammer_static_worsening))
                core_internal_ok = (
                    bool(accepted_result.get("core_hammer_eligible", False))
                    and accepted_score <= static_allowed
                    and accepted_selection <= static_allowed
                )
                if not core_internal_ok:
                    print(json.dumps({
                        "accepted": False,
                        "reason": "internal guard blocked core-Hammer candidate",
                        "candidate": accepted_result.get("candidate", {}),
                        "candidate_score": accepted_score,
                        "candidate_selection_score": accepted_selection,
                        "static_allowed_score": static_allowed,
                        "core_hammer_source_score": accepted_result.get("core_hammer_source_score"),
                        "core_hammer_score": accepted_result.get("core_hammer_score"),
                        "core_hammer_required_score": accepted_result.get("core_hammer_required_score"),
                    }, indent=2))
                    accepted_result = None
            elif (accepted_score > reference_score * (1.0 - args.min_relative_improvement)
                  or accepted_selection > required
                  or accepted_score > global_required
                  or accepted_selection > global_required):
                print(json.dumps({
                    "accepted": False,
                    "reason": "internal guard blocked non-improving candidate",
                    "candidate": accepted_result.get("candidate", {}),
                    "candidate_score": accepted_score,
                    "candidate_selection_score": accepted_selection,
                    "reference_score": reference_score,
                    "reference_selection_score": reference_selection_score,
                    "required_max_score": reference_score * (1.0 - args.min_relative_improvement),
                    "required_max_selection_score": required,
                    "global_required_score": global_required,
                    "best_known_reference": best_known_reference,
                }, indent=2))
                accepted_result = None

        if accepted_result is None:
            print(json.dumps({
                "accepted": False,
                "reason": "no candidate improved both fast residual score and full/Hammerstein guard",
                "reference_score": reference_score,
                "previous_verified_baseline": previous_score,
                "best_known_reference": best_known_reference,
                "state_best_score": state_best_score,
                "historical_best_score": historical_best_score,
                "required_max_selection_score": required,
                "best_fast_candidate": best["candidate"],
                "best_fast_score": best["score"],
                "best_selection_score": best.get("selection_score", best["score"]),
                "source_selection_score": reference_selection_score,
                "source_score": source_score,
                "rejected_by_full_guard": rejected_by_full_guard,
            }, indent=2))
            restore_snapshot(iteration_state_snapshot, iteration_baseline_snapshot,
                             baseline_path=baseline_path, build_renderer=False)
            print("Stopping guarded iteration loop without applying a worse or unproven voicing.")
            break

        accepted_snapshot = snapshot_with_verification_metadata(
            str(accepted_result["state_snapshot"]),
            result=accepted_result,
            reference_score=reference_score,
            previous_score=previous_score,
        )
        state_path.write_text(accepted_snapshot, encoding="utf-8", newline="\n")
        accepted_result["state_snapshot"] = accepted_snapshot
        persist_candidate_artifacts(Path(args.out_root) / "accepted_candidates" / str(accepted_result["candidate"]["label"]), accepted_result)
        run([sys.executable, "SAT-TR/tools/write_overdrive_voicing_header.py"])
        if build_renderer:
            build_renderer_exe()
        write_verified_baseline(
            baseline_path=baseline_path,
            render_plan=args.render_plan,
            variant=args.variant,
            aggregate=args.aggregate,
            aggregate_score=float(accepted_result["score"]),
            scores=accepted_result["scores"],
            used_cases=accepted_result["used_cases"],
            missing_cases=accepted_result["missing_cases"],
            filter_models=accepted_result["filter_models"],
            candidate=accepted_result["candidate"],
        )
        if args.control_fit == "accepted":
            run_control_fit(args, label=f"iteration_{iteration}_accepted")
        print(json.dumps({
            "accepted": True,
            "reason": "candidate improved fast residual score and passed full/Hammerstein guard",
            "reference_score": reference_score,
            "previous_verified_baseline": previous_score,
            "best_known_reference": best_known_reference,
            "state_best_score": state_best_score,
                "historical_best_score": historical_best_score,
            "accepted_candidate": accepted_result["candidate"],
            "accepted_score": accepted_result["score"],
            "accepted_selection_score": accepted_result.get("selection_score", accepted_result["score"]),
            "accepted_tone_guard": accepted_result.get("tone_guard", {}),
        }, indent=2))

    if args.final_current_rescore == "fast":
        try:
            write_current_state_report(args, skip_missing=skip_missing)
        except subprocess.CalledProcessError as exc:
            print(json.dumps({
                "current_state_report_failed": True,
                "returncode": exc.returncode,
                "note": "Header/state export remains untouched; rerun with --final-current-rescore off if only cleanup/export is needed.",
            }, indent=2))

    if args.cleanup_render_wavs != "off":
        before_count, before_bytes = collect_wav_bytes(Path(args.out_root))
        cleanup = cleanup_render_wavs(Path(args.out_root), dry_run=args.cleanup_render_wavs == "dry-run")
        after_count, after_bytes = collect_wav_bytes(Path(args.out_root))
        print(json.dumps({
            "cleanup_render_wavs": cleanup,
            "before": {"wav_count": before_count, "bytes": before_bytes},
            "after": {"wav_count": after_count, "bytes": after_bytes},
        }, indent=2))


if __name__ == "__main__":
    main()
