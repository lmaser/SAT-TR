#!/usr/bin/env python3
"""Plot Overdrive A/B cascade voicing responses from overdrive_voicing_state.json.

This is intentionally analysis-only: it mirrors the same biquad magnitude
helpers used by fit_overdrive_hammerstein_cma.py so the plotted curves match the
model used by the fitting scripts.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

from fit_overdrive_hammerstein_cma import band_response_db

LAYERS = ("pre_a", "pre_ndsp", "pre_b", "post_a", "post_ndsp", "post_b")
PRE_LAYERS = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYERS = ("post_a", "post_ndsp", "post_b")


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def layer_response(freq: np.ndarray, sr: float, bands: list[dict]) -> np.ndarray:
    out = np.zeros_like(freq, dtype=np.float64)
    for band in bands:
        out += band_response_db(freq, sr, band, float(band.get("gain_db", 0.0)))
    return out


def write_curve_csv(path: Path, freq: np.ndarray, curves: dict[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        names = list(curves)
        writer.writerow(["freq_hz", *names])
        for i, hz in enumerate(freq):
            writer.writerow([f"{hz:.8f}", *[f"{curves[name][i]:.8f}" for name in names]])


def curve_summary(freq: np.ndarray, curve: np.ndarray) -> dict:
    finite = np.isfinite(curve)
    if not np.any(finite):
        return {"mean_db": 0.0, "mean_abs_db": 0.0, "min_db": 0.0, "min_hz": 0.0, "max_db": 0.0, "max_hz": 0.0}
    f = freq[finite]
    c = curve[finite]
    min_i = int(np.argmin(c))
    max_i = int(np.argmax(c))
    return {
        "mean_db": float(np.mean(c)),
        "mean_abs_db": float(np.mean(np.abs(c))),
        "min_db": float(c[min_i]),
        "min_hz": float(f[min_i]),
        "max_db": float(c[max_i]),
        "max_hz": float(f[max_i]),
    }


def band_means(freq: np.ndarray, curve: np.ndarray) -> dict:
    bands = [(20, 80), (80, 200), (200, 500), (500, 1000),
             (1000, 2000), (2000, 5000), (5000, 10000), (10000, 20000)]
    out = {}
    for lo, hi in bands:
        mask = (freq >= lo) & (freq < hi) & np.isfinite(curve)
        if np.any(mask):
            out[f"{lo}_{hi}_hz"] = float(np.mean(curve[mask]))
    return out


def plot_curves(path: Path, title: str, freq: np.ndarray, curves: dict[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(13, 7), dpi=150)
    for name, curve in curves.items():
        width = 2.6 if name.endswith("_total") or name == "linear_total" else 1.4
        alpha = 0.98 if width > 2.0 else 0.72
        plt.semilogx(freq, curve, label=name, linewidth=width, alpha=alpha)
    plt.axhline(0.0, color="0.35", linewidth=0.8)
    plt.xlim(20.0, 24000.0)
    plt.ylim(-24.0, 12.0)
    plt.grid(True, which="both", alpha=0.22)
    plt.title(title)
    plt.xlabel("Frequency (Hz)")
    plt.ylabel("Magnitude (dB)")
    plt.legend(loc="best", fontsize=8)
    plt.tight_layout()
    plt.savefig(path)
    plt.close()



def plot_residual_csv(path: Path, title: str, out_png: Path, out_csv: Path | None = None) -> None:
    if not path.exists():
        return

    rows: list[dict[str, float]] = []
    with path.open(encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                hz = float(row["freq_hz"])
            except Exception:
                continue
            if hz < 20.0 or hz > 24000.0:
                continue
            rows.append({k: float(v) for k, v in row.items()})

    if not rows:
        return

    freq = np.asarray([r["freq_hz"] for r in rows], dtype=np.float64)
    curves = {
        "target_minus_sat_median": np.asarray([r.get("target_minus_sat_voiced_median_db", 0.0) for r in rows]),
        "static_residual_eq": np.asarray([r.get("static_residual_eq_db", 0.0) for r in rows]),
        "level_delta_high_minus_low": np.asarray([r.get("high_minus_low_db", 0.0) for r in rows]),
    }

    if out_csv is not None:
        write_curve_csv(out_csv, freq, curves)

    write_summary_json(out_png.with_name("residual_summary.json"), {
        name: {**curve_summary(freq, curve), "band_means_db": band_means(freq, curve)}
        for name, curve in curves.items()
    })
    plot_curves(out_png, title, freq, curves)


def write_summary_json(path: Path, summary: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")


def write_band_table(path: Path, pedal: str, node: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["pedal", "layer", "index", "kind", "freq_hz", "gain_db", "q", "stages", "amount"])
        writer.writeheader()
        for layer in LAYERS:
            for i, band in enumerate(node.get(layer, [])):
                writer.writerow({
                    "pedal": pedal,
                    "layer": layer,
                    "index": i,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 1000.0)),
                    "gain_db": float(band.get("gain_db", 0.0)),
                    "q": float(band.get("q", 1.0)),
                    "stages": int(float(band.get("stages", 1))),
                    "amount": band.get("amount", "Classic"),
                })


def plot_pedal(state: dict, pedal: str, out_dir: Path, sr: float, points: int) -> None:
    node = state[pedal]
    freq = np.geomspace(20.0, min(24000.0, sr * 0.499), points)
    layer_curves = {layer: layer_response(freq, sr, list(node.get(layer, []))) for layer in LAYERS}
    pre_total = sum((layer_curves[layer] for layer in PRE_LAYERS), np.zeros_like(freq))
    post_total = sum((layer_curves[layer] for layer in POST_LAYERS), np.zeros_like(freq))
    linear_total = pre_total + post_total

    curves = dict(layer_curves)
    curves["pre_total"] = pre_total
    curves["post_total"] = post_total
    curves["linear_total"] = linear_total

    pedal_dir = out_dir / pedal
    write_curve_csv(pedal_dir / "curves.csv", freq, curves)
    write_band_table(pedal_dir / "bands.csv", pedal, node)
    plot_curves(pedal_dir / "pre.png", f"{pedal.upper()} SAT-TR internal PRE voicing (not reference)", freq, {**{k: layer_curves[k] for k in PRE_LAYERS}, "pre_total": pre_total})
    plot_curves(pedal_dir / "post.png", f"{pedal.upper()} SAT-TR internal POST voicing (not reference)", freq, {**{k: layer_curves[k] for k in POST_LAYERS}, "post_total": post_total})
    plot_curves(pedal_dir / "all.png", f"{pedal.upper()} SAT-TR internal fitted linear voicing", freq, {
        "pre_total": pre_total,
        "post_total": post_total,
        "linear_total": linear_total,
    })
    write_summary_json(pedal_dir / "summary.json", {
        "pedal": pedal,
        "meaning": "SAT-TR internal fitted pre/post voicing response, not the external reference frequency response.",
        "pre_total": curve_summary(freq, pre_total),
        "post_total": curve_summary(freq, post_total),
        "linear_total": curve_summary(freq, linear_total),
        "layer_summaries": {name: curve_summary(freq, curve) for name, curve in layer_curves.items()},
    })
    print(f"Wrote {pedal_dir}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--out", default="analysis_out/overdrive_voicing_plots")
    ap.add_argument("--sample-rate", type=float, default=48000.0)
    ap.add_argument("--points", type=int, default=2048)
    ap.add_argument("--pedals", default="ts808,klon")
    ap.add_argument("--ts808-residual-csv", default="")
    ap.add_argument("--klon-residual-csv", default="")
    args = ap.parse_args()

    state = read_json(Path(args.state))
    out_dir = Path(args.out)
    for pedal in [x.strip() for x in args.pedals.split(",") if x.strip()]:
        if pedal not in state:
            raise SystemExit(f"state has no pedal '{pedal}'")
        plot_pedal(state, pedal, out_dir, args.sample_rate, max(256, int(args.points)))

    residuals = {
        "ts808": args.ts808_residual_csv,
        "klon": args.klon_residual_csv,
    }
    for pedal, residual_path in residuals.items():
        if residual_path:
            plot_residual_csv(Path(residual_path),
                              f"{pedal.upper()} residual diagnostics",
                              out_dir / pedal / "residual.png",
                              out_dir / pedal / "residual_curves_export.csv")


if __name__ == "__main__":
    main()
