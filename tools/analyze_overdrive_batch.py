#!/usr/bin/env python3
"""Analyze rendered batch WAVs for SAT-TR Overdrive identification.

Required files in --render-dir:
  overdrive_id_batch__sat_raw.wav
  overdrive_id_batch__target.wav

Optional file in --render-dir:
  overdrive_id_batch__sat_voiced.wav

If the optional SAT voiced render exists, the script automatically writes a
second comparison for RAW OFF / voiced output. No manual file editing needed.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, optimize, signal

EPS = 1.0e-12


@dataclass
class SegmentResult:
    stem: str
    kind: str
    level: float
    freq: np.ndarray
    delta_db: np.ndarray
    rms_sat_db: float
    rms_target_db: float


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    x, sr = sf.read(path, always_2d=True)
    return np.mean(x, axis=1).astype(np.float64), int(sr)


def rms_db(x: np.ndarray) -> float:
    return 20.0 * math.log10(max(float(np.sqrt(np.mean(x * x))), EPS))


def peak_db(x: np.ndarray) -> float:
    return 20.0 * math.log10(max(float(np.max(np.abs(x))), EPS))


def welch_mag_db(x: np.ndarray, sr: int, nfft: int) -> tuple[np.ndarray, np.ndarray]:
    nperseg = min(nfft, max(256, len(x) // 2))
    f, p = signal.welch(x, fs=sr, window="hann", nperseg=nperseg, noverlap=nperseg // 2,
                        nfft=nfft, detrend=False, scaling="spectrum")
    return f, 10.0 * np.log10(np.maximum(p, EPS))


def smooth_log_curve(freq: np.ndarray, curve: np.ndarray, width_oct: float = 1.0 / 6.0) -> np.ndarray:
    """Fast log-frequency Gaussian smoothing.

    The previous implementation did a full Gaussian sum for every FFT bin
    (O(N^2)); at 65536 FFT size and dozens of segments that dominates the
    whole analysis. Interpolating to a dense uniform log grid and using
    gaussian_filter1d keeps the same perceptual smoothing target with near
    linear cost.
    """
    freq = np.asarray(freq, dtype=np.float64)
    curve = np.asarray(curve, dtype=np.float64)
    mask = np.isfinite(freq) & np.isfinite(curve) & (freq >= 1.0)
    if int(np.count_nonzero(mask)) < 4:
        return curve.copy()

    logf_valid = np.log2(freq[mask])
    curve_valid = curve[mask]
    grid_count = int(np.clip(len(logf_valid) // 4, 2048, 8192))
    grid = np.linspace(float(logf_valid[0]), float(logf_valid[-1]), grid_count)
    grid_curve = np.interp(grid, logf_valid, curve_valid)
    step = max(float(grid[1] - grid[0]), 1.0e-6)
    sigma = max(width_oct / step, 1.0e-6)
    smooth_grid = ndimage.gaussian_filter1d(grid_curve, sigma=sigma, mode="nearest", truncate=4.0)

    logf_all = np.log2(np.maximum(freq, 1.0))
    return np.interp(logf_all, grid, smooth_grid, left=float(smooth_grid[0]), right=float(smooth_grid[-1]))


def valid_band(freq: np.ndarray, lo: float = 40.0, hi: float = 18000.0) -> np.ndarray:
    return (freq >= lo) & (freq <= hi)


def hammerstein_summary(x: np.ndarray, y: np.ndarray, orders: list[int]) -> dict[str, float]:
    n = min(len(x), len(y))
    x = x[:n]
    y = y[:n]
    cols = []
    for p in orders:
        xp = np.sign(x) * (np.abs(x) ** p) if p % 2 == 1 else x ** p
        xp = xp - np.mean(xp)
        r = np.sqrt(np.mean(xp * xp))
        cols.append(xp / max(float(r), EPS))
    mat = np.vstack(cols).T
    coeff, *_ = np.linalg.lstsq(mat, y - np.mean(y), rcond=None)
    recon = mat @ coeff
    err = y - np.mean(y) - recon
    total = max(float(np.mean((y - np.mean(y)) ** 2)), EPS)
    out = {
        "explained_db": 10.0 * math.log10(max(float(np.mean(recon * recon)), EPS) / total),
        "residual_db": 10.0 * math.log10(max(float(np.mean(err * err)), EPS) / total),
    }
    for p, c in zip(orders, coeff):
        out[f"order_{p}_coeff"] = float(c)
    return out


def gaussian_filterbank(freq: np.ndarray, centers: np.ndarray, q: float) -> np.ndarray:
    logf = np.log2(np.maximum(freq, 1.0))
    width = max(0.08, 1.0 / max(q, 0.1))
    return np.vstack([np.exp(-0.5 * ((logf - math.log2(c)) / width) ** 2) for c in centers]).T


def fit_residual_bands(freq: np.ndarray, target_db: np.ndarray, band_count: int, max_gain_db: float, q: float):
    mask = valid_band(freq)
    f = freq[mask]
    y = target_db[mask]
    centers = np.geomspace(80.0, 12000.0, band_count)
    basis = gaussian_filterbank(f, centers, q)

    def residual(gains: np.ndarray) -> np.ndarray:
        pred = basis @ gains
        smooth_penalty = np.diff(gains, n=2) * 0.35 if band_count >= 3 else np.array([])
        return np.concatenate([pred - y, smooth_penalty])

    result = optimize.least_squares(residual, np.zeros(band_count), bounds=(-max_gain_db, max_gain_db), max_nfev=3000)
    return [{"freq_hz": float(c), "gain_db": float(g), "q_basis": float(q)}
            for c, g in zip(centers, result.x) if abs(g) >= 0.05]


def write_curve_csv(path: Path, freq: np.ndarray, cols: dict[str, np.ndarray]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["freq_hz", *cols.keys()])
        for i in range(len(freq)):
            writer.writerow([f"{freq[i]:.6f}", *[f"{v[i]:.6f}" for v in cols.values()]])


def analyze_pair(*, label: str, suffix: str, dry: np.ndarray, sat: np.ndarray, target: np.ndarray,
                 sr: int, manifest: dict, allowed: set[str], edge: int, args, out: Path) -> dict[str, object]:
    results: list[SegmentResult] = []
    hammer_rows: list[dict[str, object]] = []

    for seg in manifest["segments"]:
        if seg["kind"] not in allowed:
            continue
        start = int(seg["start_sample"]) + edge
        end = int(seg["end_sample"]) - edge
        if end <= start:
            continue
        if end > len(dry) or end > len(sat) or end > len(target):
            print(f"skip {label} {seg['stem']}: render too short")
            continue
        dry_seg = dry[start:end]
        sat_seg = sat[start:end]
        target_seg = target[start:end]

        freq, sat_mag = welch_mag_db(sat_seg, sr, args.nfft)
        _, target_mag = welch_mag_db(target_seg, sr, args.nfft)
        delta = smooth_log_curve(freq, target_mag - sat_mag)
        results.append(SegmentResult(seg["stem"], seg["kind"], float(seg["level_dbfs_rms"]), freq, delta,
                                     rms_db(sat_seg), rms_db(target_seg)))

        h_sat = hammerstein_summary(dry_seg, sat_seg, [1, 2, 3, 5, 7])
        h_target = hammerstein_summary(dry_seg, target_seg, [1, 2, 3, 5, 7])
        hammer_rows.append({"stem": seg["stem"], "kind": seg["kind"], "level_dbfs_rms": seg["level_dbfs_rms"],
                            **{f"{label}_{k}": v for k, v in h_sat.items()},
                            **{f"target_{k}": v for k, v in h_target.items()}})

    if not results:
        raise RuntimeError(f"No usable batch segments found for {label}")

    freq = results[0].freq
    curves = np.vstack([r.delta_db for r in results])
    levels = np.array([r.level for r in results])
    low_mask = levels <= np.percentile(levels, 35)
    high_mask = levels >= np.percentile(levels, 65)
    invariant = np.median(curves, axis=0)
    low_curve = np.median(curves[low_mask], axis=0)
    high_curve = np.median(curves[high_mask], axis=0)
    dynamic_delta = high_curve - low_curve

    write_curve_csv(out / f"spectral_residual_summary{suffix}.csv", freq, {
        f"target_minus_{label}_median_db": invariant,
        "low_level_median_db": low_curve,
        "high_level_median_db": high_curve,
        "high_minus_low_db": dynamic_delta,
    })

    with (out / f"per_segment_metrics{suffix}.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["stem", "kind", "level_dbfs_rms",
                                               f"{label}_rms_db", "target_rms_db", f"target_minus_{label}_rms_db",
                                               f"{label}_peak_db", "target_peak_db", f"target_minus_{label}_peak_db"])
        writer.writeheader()
        for r in results:
            start = next(int(seg["start_sample"]) + edge for seg in manifest["segments"] if seg["stem"] == r.stem)
            end = next(int(seg["end_sample"]) - edge for seg in manifest["segments"] if seg["stem"] == r.stem)
            sat_seg = sat[start:end]
            target_seg = target[start:end]
            writer.writerow({"stem": r.stem, "kind": r.kind, "level_dbfs_rms": r.level,
                             f"{label}_rms_db": r.rms_sat_db, "target_rms_db": r.rms_target_db,
                             f"target_minus_{label}_rms_db": r.rms_target_db - r.rms_sat_db,
                             f"{label}_peak_db": peak_db(sat_seg), "target_peak_db": peak_db(target_seg),
                             f"target_minus_{label}_peak_db": peak_db(target_seg) - peak_db(sat_seg)})

    with (out / f"hammerstein_summary{suffix}.csv").open("w", newline="", encoding="utf-8") as f:
        fieldnames = sorted({k for row in hammer_rows for k in row.keys()})
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(hammer_rows)

    bands = fit_residual_bands(freq, invariant, args.band_count, args.max_gain_db, args.basis_q)
    with (out / f"suggested_residual_filterbank{suffix}.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["freq_hz", "gain_db", "q_basis"])
        writer.writeheader()
        writer.writerows(bands)

    try:
        import matplotlib.pyplot as plt
        mask = valid_band(freq)
        plt.figure(figsize=(12, 7))
        plt.semilogx(freq[mask], invariant[mask], label=f"median target - {label}")
        plt.semilogx(freq[mask], low_curve[mask], label="low level")
        plt.semilogx(freq[mask], high_curve[mask], label="high level")
        plt.semilogx(freq[mask], dynamic_delta[mask], label="high - low dynamic delta")
        plt.axhline(0, color="black", linewidth=0.8)
        plt.grid(True, which="both", alpha=0.25)
        plt.legend()
        plt.xlabel("Hz")
        plt.ylabel("dB")
        plt.title(f"Overdrive batch residuals: target - {label}")
        plt.tight_layout()
        plt.savefig(out / f"residual_summary{suffix}.png", dpi=160)
        plt.close()
    except Exception as exc:
        print(f"plot skipped for {label}: {exc}")

    return {"label": label, "suffix": suffix, "segments": len(results), "files_suffix": suffix or "raw/default"}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--render-dir", required=True)
    ap.add_argument("--sat-raw-file", default=None)
    ap.add_argument("--sat-voiced-file", default=None)
    ap.add_argument("--target-file", default=None)
    ap.add_argument("--case-meta", default=None, help="Optional JSON object/string stored in analysis_manifest.json.")
    ap.add_argument("--out", default="SAT-TR/tools/overdrive_id_analysis")
    ap.add_argument("--nfft", type=int, default=65536)
    ap.add_argument("--band-count", type=int, default=48)
    ap.add_argument("--max-gain-db", type=float, default=12.0)
    ap.add_argument("--basis-q", type=float, default=2.0)
    ap.add_argument("--kind-filter", default="pink,brown,white,multitone,sweep,stepsine,twotone_lowmid,twotone_himid,tritone_mid,tritone_himid,white_sweep_level,pink_sweep_level,brown_sweep_level")
    ap.add_argument("--trim-ms", type=float, default=40.0, help="Trim segment edges to avoid fades/gaps/host tails.")
    args = ap.parse_args()

    stim_dir = Path(args.stim_dir)
    render_dir = Path(args.render_dir)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    manifest = json.loads((stim_dir / "manifest.json").read_text(encoding="utf-8"))
    names = manifest["batch_render_naming"]
    dry, sr0 = read_mono(stim_dir / manifest["batch_file"])
    sat_raw_name = args.sat_raw_file or names["sat_raw"]
    target_name = args.target_file or names["target"]
    sat_raw, sr1 = read_mono(render_dir / sat_raw_name)
    target, sr2 = read_mono(render_dir / target_name)
    loaded = {"dry": sr0, "sat_raw": sr1, "target": sr2}

    optional = []
    voiced_name = args.sat_voiced_file or names.get("sat_voiced_optional", manifest["batch_file"].replace(".wav", "__sat_voiced.wav"))
    voiced_path = render_dir / voiced_name
    if voiced_path.exists():
        sat_voiced, sr3 = read_mono(voiced_path)
        loaded["sat_voiced"] = sr3
        optional.append(("sat_voiced", "_sat_voiced", sat_voiced))

    if len(set(loaded.values())) != 1:
        raise RuntimeError(f"sample-rate mismatch: {loaded}")

    allowed = {x.strip() for x in args.kind_filter.split(",") if x.strip()}
    edge = int(sr0 * args.trim_ms / 1000.0)
    summary = []
    summary.append(analyze_pair(label="sat_raw", suffix="", dry=dry, sat=sat_raw, target=target,
                                sr=sr0, manifest=manifest, allowed=allowed, edge=edge, args=args, out=out))
    for label, suffix, sat in optional:
        summary.append(analyze_pair(label=label, suffix=suffix, dry=dry, sat=sat, target=target,
                                    sr=sr0, manifest=manifest, allowed=allowed, edge=edge, args=args, out=out))

    case_meta = json.loads(args.case_meta) if args.case_meta else None
    (out / "analysis_manifest.json").write_text(json.dumps({
        "comparisons": summary,
        "loaded_sample_rates": loaded,
        "files": {"sat_raw": sat_raw_name, "sat_voiced": voiced_name if voiced_path.exists() else None, "target": target_name},
        "case_meta": case_meta
    }, indent=2), encoding="utf-8")
    print(f"Analyzed comparisons: {', '.join(item['label'] for item in summary)}")
    print(f"Wrote analysis to {out}")


if __name__ == "__main__":
    main()
