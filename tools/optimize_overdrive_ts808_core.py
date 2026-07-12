#!/usr/bin/env python3
"""Guarded TS808 core optimizer for SAT-TR Overdrive.

This does not fit residual EQ. It probes a small set of nonlinear/core tuning
parameters from the current voicing state, renders SAT with each candidate via
SatOverdriveRender, scores against the target render, and writes the best core
only if it improves the current state.
"""
from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, signal

EPS = 1.0e-12

PARAMS = {
    "drive_scale": {"cli": "--ts-drive-scale", "mode": "mul", "steps": [0.85, 0.90, 0.95, 1.05, 1.10, 1.15], "fast_steps": [0.90, 0.95, 1.05, 1.10], "lo": 0.55, "hi": 1.35},
    "input_gain_db": {"cli": "--ts-input-gain-db", "mode": "add", "steps": [-3.0, -2.0, -1.0, 1.0, 2.0, 3.0], "fast_steps": [-2.0, -1.0, 1.0, 2.0, 3.0], "lo": -6.0, "hi": 6.0},
    "loop_drive_max": {"cli": "--ts-loop-drive-max", "mode": "mul", "steps": [0.88, 0.94, 1.06, 1.12], "lo": 8.0, "hi": 95.0},
    "loop_capped_gain_at_max_drive": {"cli": "--ts-loop-capped-gain", "mode": "mul", "steps": [0.88, 0.94, 1.06, 1.12], "lo": 0.3, "hi": 6.0},
    "air_gain_at_max_drive": {"cli": "--ts-air-gain", "mode": "add", "steps": [-0.08, -0.04, 0.04, 0.08], "lo": -0.4, "hi": 0.8},
    "solver_knee_start": {"cli": "--ts-solver-knee-start", "mode": "add", "steps": [-0.04, -0.02, 0.02, 0.04], "lo": 0.18, "hi": 0.78},
    "solver_pre_conduct": {"cli": "--ts-solver-pre-conduct", "mode": "mul", "steps": [0.82, 0.92, 1.08, 1.18], "lo": 0.02, "hi": 0.65},
    "upper_blend_hi": {"cli": "--ts-upper-blend-hi", "mode": "add", "steps": [-0.06, -0.03, 0.03, 0.06], "lo": 0.05, "hi": 0.9},
    "upper_air_trim_hi": {"cli": "--ts-upper-air-trim-hi", "mode": "add", "steps": [-0.06, -0.03, 0.03, 0.06], "lo": 0.35, "hi": 1.2},
    "body_feedback_hi": {"cli": "--ts-body-feedback-hi", "mode": "mul", "steps": [0.88, 0.94, 1.06, 1.12], "lo": 0.4, "hi": 5.0},
    "body_hardness_hi": {"cli": "--ts-body-hardness-hi", "mode": "mul", "steps": [0.88, 0.94, 1.06, 1.12], "lo": 0.4, "hi": 3.5},
    "upper_feedback_hi": {"cli": "--ts-upper-feedback-hi", "mode": "mul", "steps": [0.82, 0.92, 1.08, 1.18], "lo": 0.05, "hi": 1.5},
    "upper_hardness_hi": {"cli": "--ts-upper-hardness-hi", "mode": "mul", "steps": [0.82, 0.92, 1.08, 1.18], "lo": 0.05, "hi": 1.2},
}
PARAM_PROFILES = {
    "fast": [
        "drive_scale",
        "input_gain_db",
        "loop_drive_max",
        "solver_pre_conduct",
        "upper_blend_hi",
        "upper_air_trim_hi",
    ],
    "balanced": [
        "drive_scale",
        "input_gain_db",
        "loop_drive_max",
        "loop_capped_gain_at_max_drive",
        "air_gain_at_max_drive",
        "solver_pre_conduct",
        "upper_blend_hi",
        "upper_air_trim_hi",
        "body_feedback_hi",
        "body_hardness_hi",
    ],
    "full": [key for key in PARAMS.keys() if key != "solver_knee_start"],
}

FAST_STEPS = {
    "mul": [0.94, 1.06],
    "add": [-0.03, 0.03],
}


CPP_NAMES = {
    "drive_scale": "driveScale",
    "input_gain_db": "inputGainDb",
    "loop_drive_max": "loopDriveMax",
    "loop_capped_gain_at_max_drive": "loopCappedGainAtMaxDrive",
    "air_gain_at_max_drive": "airGainAtMaxDrive",
    "solver_knee_start": "solverKneeStart",
    "solver_pre_conduct": "solverPreConduct",
    "upper_blend_hi": "upperBlendHi",
    "upper_air_trim_hi": "upperAirTrimHi",
    "body_feedback_hi": "bodyFeedbackHi",
    "body_hardness_hi": "bodyHardnessHi",
    "upper_feedback_hi": "upperFeedbackHi",
    "upper_hardness_hi": "upperHardnessHi",
}


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    x, sr = sf.read(path, always_2d=True)
    return np.mean(x, axis=1).astype(np.float64), int(sr)


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x * x)))


def smooth_log_curve(freq: np.ndarray, curve: np.ndarray, width_oct: float = 1.0 / 6.0) -> np.ndarray:
    mask = np.isfinite(freq) & np.isfinite(curve) & (freq >= 1.0)
    if int(np.count_nonzero(mask)) < 4:
        return curve.copy()
    logf_valid = np.log2(freq[mask])
    curve_valid = curve[mask]
    grid_count = int(np.clip(len(logf_valid) // 4, 2048, 8192))
    grid = np.linspace(float(logf_valid[0]), float(logf_valid[-1]), grid_count)
    grid_curve = np.interp(grid, logf_valid, curve_valid)
    step = max(float(grid[1] - grid[0]), 1.0e-6)
    smooth_grid = ndimage.gaussian_filter1d(grid_curve, sigma=max(width_oct / step, 1.0e-6), mode="nearest", truncate=4.0)
    return np.interp(np.log2(np.maximum(freq, 1.0)), grid, smooth_grid, left=float(smooth_grid[0]), right=float(smooth_grid[-1]))


def score_render(stim_dir: Path, sat_file: Path, target_file: Path, kind_filter: set[str], nfft: int, trim_ms: float) -> float:
    manifest = read_json(stim_dir / "manifest.json")
    sat, sr_sat = read_mono(sat_file)
    target, sr_target = read_mono(target_file)
    if sr_sat != sr_target:
        raise RuntimeError(f"sample-rate mismatch: {sr_sat} vs {sr_target}")
    edge = int(sr_sat * trim_ms / 1000.0)
    curves = []
    level_errors = []
    for seg in manifest["segments"]:
        if seg["kind"] not in kind_filter:
            continue
        start = int(seg["start_sample"]) + edge
        end = int(seg["end_sample"]) - edge
        if end <= start or end > min(len(sat), len(target)):
            continue
        a = sat[start:end]
        b = target[start:end]
        gain = rms(b) / max(rms(a), EPS)
        a = a * gain
        nperseg = min(nfft, max(256, len(a) // 2))
        f, pa = signal.welch(a, fs=sr_sat, window="hann", nperseg=nperseg, noverlap=nperseg // 2, nfft=nfft, detrend=False, scaling="spectrum")
        _, pb = signal.welch(b, fs=sr_sat, window="hann", nperseg=nperseg, noverlap=nperseg // 2, nfft=nfft, detrend=False, scaling="spectrum")
        delta = smooth_log_curve(f, 10.0 * np.log10(np.maximum(pb, EPS)) - 10.0 * np.log10(np.maximum(pa, EPS)))
        mask = (f >= 60.0) & (f <= 16000.0)
        curves.append(delta[mask])
        level_errors.append(abs(20.0 * math.log10(max(gain, EPS))))
    if not curves:
        raise RuntimeError("no usable segments for scoring")
    arr = np.vstack(curves)
    spectral = float(np.mean(np.median(arr, axis=0) ** 2))
    dynamic = float(np.mean(np.var(arr, axis=0)))
    level = float(np.mean(level_errors))
    return spectral + 0.35 * dynamic + 0.04 * level * level


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def candidate_value(current: float, spec: dict, step: float) -> float:
    if spec["mode"] == "mul":
        return clamp(current * step, spec["lo"], spec["hi"])
    return clamp(current + step, spec["lo"], spec["hi"])


def render_candidate(renderer: Path, dry: Path, out: Path, case: dict, core: dict, keys: list[str]) -> None:
    sat_settings = case.get("sat_settings", {})
    cmd = [
        str(renderer),
        "--in", str(dry),
        "--out", str(out),
        "--raw", "0",
        "--drive", str(float(sat_settings.get("drive", 1.0))),
        "--type", str(float(sat_settings.get("type", 0.0))),
        "--knee", str(float(sat_settings.get("knee", 0.0))),
        "--input-db", str(float(sat_settings.get("host_or_plugin_input_db", 0.0))),
        "--output-db", str(float(sat_settings.get("output_db", 0.0))),
    ]
    for key in keys:
        cmd.extend([PARAMS[key]["cli"], str(float(core[key]))])
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--render-plan", required=True)
    ap.add_argument("--render-dir", default="SAT-TR/tools/overdrive_id_renders")
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--sat-renderer-exe", default="SAT-TR/tools/sat_overdrive_renderer/SatOverdriveRender.exe")
    ap.add_argument("--voicing-state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--header-generator", default="SAT-TR/tools/write_overdrive_voicing_header.py")
    ap.add_argument("--case-id", default=None)
    ap.add_argument("--passes", type=int, default=1)
    ap.add_argument("--profile", choices=sorted(PARAM_PROFILES), default="fast",
                    help="Core search profile. fast is intended for repeated autonomous iterations.")
    ap.add_argument("--min-relative-improvement", type=float, default=0.001)
    ap.add_argument("--nfft", type=int, default=16384)
    ap.add_argument("--trim-ms", type=float, default=40.0)
    ap.add_argument("--kind-filter", default="pink,brown,multitone,twotone_lowmid,twotone_himid")
    args = ap.parse_args()

    plan = read_json(Path(args.render_plan))
    cases = plan.get("cases", [])
    if args.case_id:
        cases = [c for c in cases if c["id"] == args.case_id]
    if not cases:
        raise SystemExit("no render-plan case selected")
    case = cases[0]
    state_path = Path(args.voicing_state)
    state = read_json(state_path)
    core = dict(state["ts808"]["core"])
    render_dir = Path(args.render_dir)
    stim_dir = Path(args.stim_dir)
    manifest = read_json(stim_dir / "manifest.json")
    dry = stim_dir / manifest["batch_file"]
    target = render_dir / case["files"]["target"]
    if not target.exists():
        raise SystemExit(f"missing target render: {target}")
    renderer = Path(args.sat_renderer_exe)
    if not renderer.exists():
        raise SystemExit(f"missing renderer: {renderer}")
    kind_filter = {x.strip() for x in args.kind_filter.split(",") if x.strip()}
    keys = list(PARAM_PROFILES[args.profile])

    with tempfile.TemporaryDirectory(prefix="ts808_core_fit_") as td:
        tmp = Path(td)
        current_render = tmp / "current.wav"
        render_candidate(renderer, dry, current_render, case, core, keys)
        best_score = score_render(stim_dir, current_render, target, kind_filter, args.nfft, args.trim_ms)
        original_score = best_score
        best_core = dict(core)
        accepted_moves = []

        for _pass in range(args.passes):
            improved_this_pass = False
            for key in keys:
                spec = PARAMS[key]
                local_best_score = best_score
                local_best_core = None
                steps = spec.get("fast_steps", FAST_STEPS[spec["mode"]]) if args.profile == "fast" else spec["steps"]
                for step in steps:
                    trial_core = dict(best_core)
                    trial_core[key] = candidate_value(float(best_core[key]), spec, float(step))
                    if abs(float(trial_core[key]) - float(best_core[key])) < 1.0e-9:
                        continue
                    out = tmp / f"candidate_{key}_{str(step).replace('.', 'p').replace('-', 'm')}.wav"
                    render_candidate(renderer, dry, out, case, trial_core, keys)
                    score = score_render(stim_dir, out, target, kind_filter, args.nfft, args.trim_ms)
                    if score < local_best_score:
                        local_best_score = score
                        local_best_core = trial_core
                required = best_score * (1.0 - args.min_relative_improvement)
                if local_best_core is not None and local_best_score <= required:
                    old = best_core[key]
                    best_core = local_best_core
                    best_score = local_best_score
                    improved_this_pass = True
                    accepted_moves.append({"param": key, "old": old, "new": best_core[key], "score": best_score})
            if not improved_this_pass:
                break

    accepted = best_score <= original_score * (1.0 - args.min_relative_improvement)
    if accepted:
        state["ts808"]["core"].update(best_core)
        state["ts808"]["last_core_optimizer"] = {
            "case_id": case["id"],
            "original_score": original_score,
            "best_score": best_score,
            "accepted_moves": accepted_moves,
            "profile": args.profile,
        }
        write_json(state_path, state)
        subprocess.run([sys.executable, args.header_generator], check=True)

    print(json.dumps({
        "accepted": accepted,
        "case_id": case["id"],
        "original_score": original_score,
        "best_score": best_score,
        "accepted_moves": accepted_moves,
        "profile": args.profile,
        "searched_params": keys,
    }, indent=2))
    if not accepted:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
