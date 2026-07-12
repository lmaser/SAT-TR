#!/usr/bin/env python3
"""Fit TS808-facing SAT-TR controls against an existing target render.

This is a diagnostic tool: it does not modify the plugin voicing state.
It answers whether the current SAT-TR Overdrive settings need a different
effective DRIVE, input gain, or output trim to match a reference render.
KNEE support remains available for later experiments, but the default grid
is fixed at 0 so the TS808 pass does not chase that dimension for now.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, signal
from scipy.io import wavfile

EPS = 1.0e-12


def parse_float_list(text: str) -> list[float]:
    return [float(x.strip()) for x in text.split(",") if x.strip()]


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_audio(path: Path) -> tuple[np.ndarray, int]:
    x, sr = sf.read(str(path), always_2d=True, dtype="float32")
    return x.astype(np.float64), int(sr)


def mono(x: np.ndarray) -> np.ndarray:
    return np.mean(x, axis=1)


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.asarray(x, dtype=np.float64) ** 2)))


def smooth_log_curve(freq: np.ndarray, curve: np.ndarray, width_oct: float = 1.0 / 6.0) -> np.ndarray:
    mask = np.isfinite(freq) & np.isfinite(curve) & (freq >= 1.0)
    if int(np.count_nonzero(mask)) < 4:
        return curve.copy()
    logf = np.log2(freq[mask])
    y = curve[mask]
    grid_count = int(np.clip(len(logf) // 4, 1024, 4096))
    grid = np.linspace(float(logf[0]), float(logf[-1]), grid_count)
    yg = np.interp(grid, logf, y)
    step = max(float(grid[1] - grid[0]), 1.0e-6)
    smooth = ndimage.gaussian_filter1d(yg, sigma=max(width_oct / step, 1.0e-6), mode="nearest", truncate=4.0)
    return np.interp(np.log2(np.maximum(freq, 1.0)), grid, smooth, left=float(smooth[0]), right=float(smooth[-1]))


def select_case(plan: dict, case_id: str | None) -> dict:
    cases = plan.get("cases", [])
    if not cases:
        raise SystemExit("render plan has no cases")
    if case_id is None:
        return cases[0]
    for case in cases:
        if case.get("id") == case_id:
            return case
    raise SystemExit(f"unknown case id: {case_id}")


def build_compact_batch(stim_dir: Path, render_dir: Path, case: dict, kind_filter: set[str],
                        trim_ms: float, max_segments_per_kind: int, out_dir: Path) -> tuple[Path, Path, list[dict]]:
    manifest = read_json(stim_dir / "manifest.json")
    dry_path = stim_dir / case.get("source_batch_file", manifest["batch_file"])
    target_path = render_dir / case["files"]["target"]
    if not target_path.exists():
        raise SystemExit(f"missing target render: {target_path}")

    dry, sr = read_audio(dry_path)
    target, target_sr = read_audio(target_path)
    if sr != target_sr:
        raise SystemExit(f"sample-rate mismatch: dry {sr}, target {target_sr}")
    if len(dry) != len(target):
        raise SystemExit(f"length mismatch: dry {len(dry)} frames, target {len(target)} frames")

    trim = int(round(sr * trim_ms / 1000.0))
    counts: dict[str, int] = {}
    compact_dry: list[np.ndarray] = []
    compact_target: list[np.ndarray] = []
    compact_segments: list[dict] = []
    cursor = 0

    for seg in manifest["segments"]:
        kind = seg.get("kind", "")
        if kind_filter and kind not in kind_filter:
            continue
        if counts.get(kind, 0) >= max_segments_per_kind:
            continue
        start = int(seg["start_sample"]) + trim
        end = int(seg["end_sample"]) - trim
        if end <= start or end > min(len(dry), len(target)):
            continue
        d = dry[start:end]
        t = target[start:end]
        if len(d) < 512:
            continue
        compact_dry.append(d)
        compact_target.append(t)
        compact_segments.append({
            "kind": kind,
            "start": cursor,
            "end": cursor + len(d),
            "source_start": start,
            "source_end": end,
        })
        cursor += len(d)
        counts[kind] = counts.get(kind, 0) + 1

    if not compact_segments:
        raise SystemExit("no usable segments selected")

    dry_out = out_dir / "compact_dry.wav"
    target_out = out_dir / "compact_target.wav"
    wavfile.write(str(dry_out), sr, np.vstack(compact_dry).astype(np.float32))
    wavfile.write(str(target_out), sr, np.vstack(compact_target).astype(np.float32))
    return dry_out, target_out, compact_segments


def render_candidate(renderer: Path, dry: Path, out: Path, *, model: str, drive: float, input_db: float,
                     output_db: float, knee: float, type_value: float, raw: bool, block_size: int) -> None:
    cmd = [
        str(renderer),
        "--in", str(dry),
        "--out", str(out),
        "--model", model,
        "--raw", "1" if raw else "0",
        "--drive", str(drive),
        "--type", str(type_value),
        "--char", str(knee),
        "--input-db", str(input_db),
        "--output-db", str(output_db),
        "--block-size", str(block_size),
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def score_candidate(sat_path: Path, target_path: Path, segments: list[dict], nfft: int) -> dict:
    sat, sr = read_audio(sat_path)
    target, sr2 = read_audio(target_path)
    if sr != sr2:
        raise SystemExit("candidate/target sample-rate mismatch")
    sat_m = mono(sat)
    target_m = mono(target)
    spectral_errors = []
    dynamic_errors = []
    level_errors = []
    harmonic_proxy = []

    for seg in segments:
        start = int(seg["start"])
        end = int(seg["end"])
        a = sat_m[start:end]
        b = target_m[start:end]
        gain = rms(b) / max(rms(a), EPS)
        a_matched = a * gain
        level_errors.append(20.0 * math.log10(max(gain, EPS)))

        nperseg = min(nfft, max(256, len(a) // 2))
        f, pa = signal.welch(a_matched, fs=sr, window="hann", nperseg=nperseg,
                             noverlap=nperseg // 2, nfft=nfft, detrend=False, scaling="spectrum")
        _, pb = signal.welch(b, fs=sr, window="hann", nperseg=nperseg,
                             noverlap=nperseg // 2, nfft=nfft, detrend=False, scaling="spectrum")
        delta = smooth_log_curve(f, 10.0 * np.log10(np.maximum(pb, EPS)) - 10.0 * np.log10(np.maximum(pa, EPS)))
        mask = (f >= 60.0) & (f <= 16000.0)
        spectral_errors.append(delta[mask])

        # Crest/absolute-difference proxy catches changes that pure static EQ
        # can hide after RMS matching.
        diff = b - a_matched
        dynamic_errors.append(20.0 * math.log10(max(rms(diff), EPS)) - 20.0 * math.log10(max(rms(b), EPS)))
        harmonic_proxy.append(float(np.mean(np.abs(np.diff(np.tanh(a_matched * 2.0) - np.tanh(b * 2.0))))))

    arr = np.vstack(spectral_errors)
    median_curve = np.median(arr, axis=0)
    spectral = float(np.sqrt(np.mean(median_curve ** 2)))
    variance = float(np.sqrt(np.mean(np.var(arr, axis=0))))
    level_median = float(np.median(np.abs(level_errors)))
    dynamic = float(np.median(dynamic_errors))
    harmonic = float(np.median(harmonic_proxy))
    score = spectral + 0.35 * variance + 0.04 * level_median + 0.5 * harmonic
    return {
        "score": score,
        "spectral_rmse_db": spectral,
        "spectral_variance_db": variance,
        "median_level_match_abs_db": level_median,
        "median_dynamic_error_db": dynamic,
        "harmonic_proxy": harmonic,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--render-plan", default="SAT-TR/tools/overdrive_id_renders/render_plan_ts808.json")
    ap.add_argument("--case-id", default="ts808_drive_drv100_in_p0")
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--render-dir", default="SAT-TR/tools/overdrive_id_renders")
    ap.add_argument("--sat-renderer-exe", default="SAT-TR/tools/sat_overdrive_renderer/SatOverdriveRender.exe")
    ap.add_argument("--out", default="analysis_out/ts808_control_fit")
    ap.add_argument("--drive-grid", default="0.75,0.90,1.00,1.10,1.25")
    ap.add_argument("--input-db-grid", default="-3,-2,-1,0,1,2,3")
    ap.add_argument("--output-db-grid", default="0")
    ap.add_argument("--knee-grid", default="0",
                    help="Advanced/disabled by default for TS808; keep at 0 unless explicitly testing knee.")
    ap.add_argument("--type", type=float, default=0.0)
    ap.add_argument("--raw", action="store_true", help="Fit against raw SAT render instead of voiced.")
    ap.add_argument("--kind-filter", default="pink,brown,white,multitone,stepsine,twotone_lowmid,twotone_himid")
    ap.add_argument("--max-segments-per-kind", type=int, default=3)
    ap.add_argument("--trim-ms", type=float, default=40.0)
    ap.add_argument("--nfft", type=int, default=4096)
    ap.add_argument("--block-size", type=int, default=1024)
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    plan = read_json(Path(args.render_plan))
    case = select_case(plan, args.case_id)
    renderer = Path(args.sat_renderer_exe)
    if not renderer.exists():
        raise SystemExit(f"missing renderer: {renderer}")

    sat_settings = case.get("sat_settings", {})
    engine_model = str(sat_settings.get("engine_model", "")).strip().lower()
    model_name = "klon" if engine_model == "klon" or str(case.get("id", "")).startswith("klon_") else "ts808"

    kind_filter = {x.strip() for x in args.kind_filter.split(",") if x.strip()}
    drive_grid = parse_float_list(args.drive_grid)
    input_grid = parse_float_list(args.input_db_grid)
    output_grid = parse_float_list(args.output_db_grid)
    knee_grid = parse_float_list(args.knee_grid)

    tmp = out / "_work"
    tmp.mkdir(parents=True, exist_ok=True)
    dry, target, segments = build_compact_batch(Path(args.stim_dir), Path(args.render_dir), case,
                                                kind_filter, args.trim_ms, args.max_segments_per_kind, tmp)
    results = []
    total = len(drive_grid) * len(input_grid) * len(output_grid) * len(knee_grid)
    index = 0
    for drive in drive_grid:
        for input_db in input_grid:
            for output_db in output_grid:
                for knee in knee_grid:
                    index += 1
                    candidate = tmp / f"sat_d{drive:.4f}_i{input_db:.2f}_o{output_db:.2f}_k{knee:.4f}.wav"
                    render_candidate(renderer, dry, candidate, model=model_name, drive=drive, input_db=input_db,
                                     output_db=output_db, knee=knee, type_value=args.type, raw=args.raw,
                                     block_size=args.block_size)
                    metrics = score_candidate(candidate, target, segments, args.nfft)
                    row = {
                        "drive": drive,
                        "input_db": input_db,
                        "output_db": output_db,
                        "knee": knee,
                        "raw": bool(args.raw),
                        **metrics,
                    }
                    results.append(row)
                    print(f"{index:4d}/{total} drive={drive:.3f} input={input_db:+.1f} output={output_db:+.1f} knee={knee:.3f} score={metrics['score']:.6f}")

    results.sort(key=lambda r: r["score"])
    csv_path = out / "control_fit_results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(results[0].keys()))
        writer.writeheader()
        writer.writerows(results)

    best = results[0]
    same_best_input_output = [r for r in results
                              if float(r["input_db"]) == float(best["input_db"])
                              and float(r["output_db"]) == float(best["output_db"])
                              and float(r["knee"]) == float(best["knee"])]
    unique_scores_by_drive = sorted({round(float(r["score"]), 9) for r in same_best_input_output})
    drive_grid_warning = ""
    if len(same_best_input_output) > 1 and len(unique_scores_by_drive) == 1:
        drive_grid_warning = (
            "All tested DRIVE values for the best INPUT/OUTPUT produced the same score. "
            "The current renderer/plugin path is likely clamping or saturating DRIVE above the effective maximum; "
            "do not infer useful >100% drive headroom from this run."
        )

    summary = {
        "case_id": case["id"],
        "fit_kind": "overdrive_control_grid",
        "render_plan": args.render_plan,
        "type": args.type,
        "raw": bool(args.raw),
        "kind_filter": sorted(kind_filter),
        "max_segments_per_kind": args.max_segments_per_kind,
        "nfft": args.nfft,
        "best": best,
        "recommended_user_settings": {
            "drive_percent": round(float(best["drive"]) * 100.0, 3),
            "input_db": float(best["input_db"]),
            "output_db": float(best["output_db"]),
            "knee_percent": round(float(best["knee"]) * 100.0, 3),
            "interpretation": "Diagnostic only: set these controls/host gains to match this reference; no voicing state is modified."
        },
        "drive_grid_warning": drive_grid_warning,
        "same_best_input_output_by_drive": [
            {"drive": float(r["drive"]), "score": float(r["score"])}
            for r in same_best_input_output
        ],
        "top_10": results[:10],
        "note": "Diagnostic only. This reports user-facing DRIVE/INPUT/OUTPUT recommendations; it does not modify plugin voicing. Knee is held at 0 by default.",
    }
    summary_path = out / "control_fit_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\nBest: {json.dumps(results[0], indent=2)}")
    print(f"Wrote {summary_path}")
    print(f"Wrote {csv_path}")


if __name__ == "__main__":
    main()
