#!/usr/bin/env python3
"""Fit Overdrive post-cascade residual using Hammerstein diagnostics.

This is an external analysis/fit script. It does not touch the VST3.

Inputs are the same two rendered batch files used by analyze_overdrive_batch.py:
  overdrive_id_batch__sat_raw.wav
  overdrive_id_batch__target.wav

Pipeline:
  1. Segment dry/SAT RAW/target using manifest.json.
  2. Estimate Hammerstein branch filters dry -> SAT RAW and dry -> target.
  3. Fit a constrained static post-cascade spectral residual.

Important limitation:
  This script optimizes only a linear post-cascade correction around the
  already-rendered SAT output. It does not optimize pre layers, because pre
  EQ changes harmonic generation and must be render-verified. It is still
  useful: if residual changes with input level, that is evidence that EQ
  alone is insufficient and the solver/pre-shaping must be changed.
"""

from __future__ import annotations

import argparse
import csv
import json
import hashlib
import math
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, optimize, signal

try:
    import cma
except ImportError:
    cma = None

EPS = 1.0e-12


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    x, sr = sf.read(path, always_2d=True)
    return np.mean(x, axis=1).astype(np.float64), int(sr)


def file_fingerprint(path: Path) -> dict:
    st = path.stat()
    return {"path": str(path.resolve()), "size": int(st.st_size), "mtime_ns": int(st.st_mtime_ns)}


def feature_cache_key(*, target_path: Path, manifest_path: Path, nfft: int, trim_ms: float, kind_filter: str) -> str:
    payload = {
        "version": 1,
        "target": file_fingerprint(target_path),
        "manifest": file_fingerprint(manifest_path),
        "nfft": int(nfft),
        "trim_ms": float(trim_ms),
        "kind_filter": kind_filter,
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_target_mag_cache(cache_dir: Path | None, key: str) -> dict[str, np.ndarray]:
    if cache_dir is None:
        return {}
    path = cache_dir / f"target_mag_{key}.npz"
    if not path.exists():
        return {}
    try:
        data = np.load(path, allow_pickle=False)
        stems = [str(x) for x in data["stems"].tolist()]
        mags = data["mags"]
        return {stem: mags[i].copy() for i, stem in enumerate(stems)}
    except Exception:
        return {}


def save_target_mag_cache(cache_dir: Path | None, key: str, cache: dict[str, np.ndarray]) -> None:
    if cache_dir is None or not cache:
        return
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = cache_dir / f"target_mag_{key}.npz"
    tmp = path.with_suffix(path.suffix + ".tmp")
    stems = np.asarray(list(cache.keys()))
    mags = np.vstack([cache[stem] for stem in cache.keys()])
    with tmp.open("wb") as f:
        np.savez_compressed(f, stems=stems, mags=mags)
    tmp.replace(path)


def rms_db(x: np.ndarray) -> float:
    return 20.0 * math.log10(max(float(np.sqrt(np.mean(x * x))), EPS))


def valid_band(freq: np.ndarray, lo: float = 40.0, hi: float = 18000.0) -> np.ndarray:
    return (freq >= lo) & (freq <= hi)


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


def hammerstein_branches(x: np.ndarray, y: np.ndarray, sr: int, orders: list[int], taps: int,
                         chunk_samples: int = 8192) -> dict[int, np.ndarray]:
    """Least-squares Hammerstein branch FIRs without materialising full Toeplitz matrices.

    y[n] ~= sum_p FIR_p * phi_p(x)[n]
    where phi_p(x)=signed abs(x)^p for odd p and x^p for even p.

    The old implementation built one dense ``n x (orders*taps)`` matrix per
    segment. This accumulates the equivalent normal equations in chunks, keeping
    the model and taps unchanged while avoiding multi-GB temporaries.
    """
    n = min(len(x), len(y))
    x = np.asarray(x[:n], dtype=np.float64)
    y = np.asarray(y[:n], dtype=np.float64)
    y = y - np.mean(y)

    phis = []
    for p in orders:
        phi = np.sign(x) * (np.abs(x) ** p) if p % 2 == 1 else x ** p
        phi = phi - np.mean(phi)
        phi /= max(float(np.sqrt(np.mean(phi * phi))), EPS)
        phis.append(phi)

    # Precompute lag views once per branch. Column 0 is the current sample,
    # column k is phi[n-k]. This removes the Python tap loop from every chunk.
    lag_views = []
    for phi in phis:
        padded = np.concatenate((np.zeros(max(0, taps - 1), dtype=np.float64), phi))
        view = np.lib.stride_tricks.sliding_window_view(padded, taps)[:n, ::-1]
        lag_views.append(view)

    total_cols = len(orders) * taps
    xtx = np.zeros((total_cols, total_cols), dtype=np.float64)
    xty = np.zeros(total_cols, dtype=np.float64)
    chunk_samples = max(1, int(chunk_samples))

    for start in range(0, n, chunk_samples):
        end = min(n, start + chunk_samples)
        rows = end - start
        design = np.zeros((rows, total_cols), dtype=np.float64)

        col_base = 0
        for lag_view in lag_views:
            design[:, col_base:col_base + taps] = lag_view[start:end]
            col_base += taps

        yy = y[start:end]
        xtx += design.T @ design
        xty += design.T @ yy

    # Tiny relative ridge prevents singular edge cases without meaningfully
    # changing a well-conditioned least-squares solution.
    diag_mean = max(float(np.mean(np.diag(xtx))), EPS)
    regularized = xtx + np.eye(total_cols, dtype=np.float64) * (diag_mean * 1.0e-12)
    try:
        coeff = np.linalg.solve(regularized, xty)
    except np.linalg.LinAlgError:
        coeff, *_ = np.linalg.lstsq(regularized, xty, rcond=None)

    branches = {}
    pos = 0
    for p in orders:
        branches[p] = coeff[pos:pos + taps].copy()
        pos += taps
    return branches


def branch_mag_db(branch: np.ndarray, nfft: int) -> tuple[np.ndarray, np.ndarray]:
    h = np.fft.rfft(branch, nfft)
    return np.abs(h), 20.0 * np.log10(np.maximum(np.abs(h), EPS))


def gaussian_basis(freq: np.ndarray, centers: np.ndarray, q: float) -> np.ndarray:
    logf = np.log2(np.maximum(freq, 1.0))
    width = max(0.08, 1.0 / max(q, 0.1))
    return np.vstack([np.exp(-0.5 * ((logf - math.log2(c)) / width) ** 2) for c in centers]).T


def biquad_response_db(freq: np.ndarray, b0: float, b1: float, b2: float, a1: float, a2: float, sr: float) -> np.ndarray:
    w = 2.0 * math.pi * np.asarray(freq, dtype=np.float64) / max(float(sr), 1000.0)
    z1 = np.exp(-1j * w)
    z2 = np.exp(-2j * w)
    h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)
    return 20.0 * np.log10(np.maximum(np.abs(h), EPS))


def peak_eq_response_db(freq: np.ndarray, sr: float, f0: float, q: float, gain_db: float) -> np.ndarray:
    """Magnitude response matching SaturationEngine updateKlonPeakEqCoeffs()."""
    safe_sr = max(float(sr), 1000.0)
    safe_f0 = min(max(float(f0), 5.0), safe_sr * 0.45)
    safe_q = max(float(q), 0.025)
    a = 10.0 ** (float(gain_db) / 40.0)
    w0 = 2.0 * math.pi * safe_f0 / safe_sr
    alpha = math.sin(w0) / (2.0 * safe_q)
    cos_w0 = math.cos(w0)

    b0 = 1.0 + alpha * a
    b1 = -2.0 * cos_w0
    b2 = 1.0 - alpha * a
    a0 = 1.0 + alpha / a
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha / a
    inv_a0 = 1.0 / max(a0, 1.0e-12)
    return biquad_response_db(freq, b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0, safe_sr)


def shelf_eq_response_db(freq: np.ndarray, sr: float, f0: float, q: float, gain_db: float, *, high_shelf: bool) -> np.ndarray:
    """Magnitude response matching SaturationEngine updateKlonShelfEqCoeffs()."""
    safe_sr = max(float(sr), 1000.0)
    safe_f0 = min(max(float(f0), 5.0), safe_sr * 0.45)
    safe_q = max(float(q), 0.025)
    a = 10.0 ** (float(gain_db) / 40.0)
    w0 = 2.0 * math.pi * safe_f0 / safe_sr
    cos_w = math.cos(w0)
    sin_w = math.sin(w0)
    alpha = sin_w / (2.0 * safe_q)
    beta = 2.0 * math.sqrt(a) * alpha

    if high_shelf:
        b0 = a * ((a + 1.0) + (a - 1.0) * cos_w + beta)
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w)
        b2 = a * ((a + 1.0) + (a - 1.0) * cos_w - beta)
        a0 = (a + 1.0) - (a - 1.0) * cos_w + beta
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w)
        a2 = (a + 1.0) - (a - 1.0) * cos_w - beta
    else:
        b0 = a * ((a + 1.0) - (a - 1.0) * cos_w + beta)
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w)
        b2 = a * ((a + 1.0) - (a - 1.0) * cos_w - beta)
        a0 = (a + 1.0) + (a - 1.0) * cos_w + beta
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w)
        a2 = (a + 1.0) + (a - 1.0) * cos_w - beta

    inv_a0 = 1.0 / max(a0, 1.0e-12)
    return biquad_response_db(freq, b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0, safe_sr)


def lowpass_response_db(freq: np.ndarray, sr: float, f0: float, q: float) -> np.ndarray:
    safe_sr = max(float(sr), 1000.0)
    safe_f0 = min(max(float(f0), 20.0), safe_sr * 0.45)
    safe_q = max(float(q), 0.025)
    w0 = 2.0 * math.pi * safe_f0 / safe_sr
    cos_w = math.cos(w0)
    sin_w = math.sin(w0)
    alpha = sin_w / (2.0 * safe_q)
    b0 = (1.0 - cos_w) * 0.5
    b1 = 1.0 - cos_w
    b2 = (1.0 - cos_w) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w
    a2 = 1.0 - alpha
    inv_a0 = 1.0 / max(a0, 1.0e-12)
    return biquad_response_db(freq, b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0, safe_sr)


def highpass_response_db(freq: np.ndarray, sr: float, f0: float, q: float) -> np.ndarray:
    safe_sr = max(float(sr), 1000.0)
    safe_f0 = min(max(float(f0), 20.0), safe_sr * 0.45)
    safe_q = max(float(q), 0.025)
    w0 = 2.0 * math.pi * safe_f0 / safe_sr
    cos_w = math.cos(w0)
    sin_w = math.sin(w0)
    alpha = sin_w / (2.0 * safe_q)
    b0 = (1.0 + cos_w) * 0.5
    b1 = -(1.0 + cos_w)
    b2 = (1.0 + cos_w) * 0.5
    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w
    a2 = 1.0 - alpha
    inv_a0 = 1.0 / max(a0, 1.0e-12)
    return biquad_response_db(freq, b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0, safe_sr)


def tilt_shelf_response_db(freq: np.ndarray, sr: float, f0: float, q: float, gain_db: float, stages: int) -> np.ndarray:
    # Exact mirror of SaturationEngine processKlonEqBand(TiltShelf): each logical
    # stage consumes two biquad states, low shelf then high shelf, each with half
    # the per-stage gain in opposite directions.
    logical_stages = max(1, min(int(stages), 2))
    stage_gain = float(gain_db) / float(logical_stages)
    out = np.zeros_like(freq, dtype=np.float64)
    for _ in range(logical_stages):
        out += shelf_eq_response_db(freq, sr, f0, q, -stage_gain * 0.5, high_shelf=False)
        out += shelf_eq_response_db(freq, sr, f0, q,  stage_gain * 0.5, high_shelf=True)
    return out


def make_ndsp_band_eq_layout() -> list[dict]:
    # Human-style fixed grid inspired by common Neural DSP-style graphic EQ
    # points: one low shelf, octave-ish peak bands, and one air shelf.
    return [
        {"kind": "LowShelf",  "freq_hz": 65.0,    "q": 0.707, "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 125.0,   "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 250.0,   "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 500.0,   "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 1000.0,  "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 2000.0,  "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 4000.0,  "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 8000.0,  "q": 1.0,   "stages": 1, "amount": "Classic"},
        {"kind": "HighShelf", "freq_hz": 16000.0, "q": 0.707, "stages": 1, "amount": "Classic"},
    ]


def tag_post_layers(layout: list[dict]) -> list[str]:
    ndsp = make_ndsp_band_eq_layout()
    ndsp_len = len(ndsp)
    for i in range(0, len(layout) + 1):
        tail = layout[i:i + ndsp_len]
        if len(tail) != ndsp_len:
            continue
        ok = True
        for band, ref in zip(tail, ndsp):
            if band.get("kind") != ref.get("kind"):
                ok = False
                break
            if abs(float(band.get("freq_hz", 0.0)) - float(ref.get("freq_hz", 0.0))) > 1.0e-3:
                ok = False
                break
        if ok:
            tail_len = max(0, len(layout) - i - ndsp_len)
            return ["post_a"] * i + ["post_ndsp"] * ndsp_len + ["post_b"] * tail_len
    return ["post_a"] * len(layout)


def make_filter_layout(layout: str, bands: int, q: float) -> list[dict]:
    if layout == "peak":
        return [
            {"kind": "Peak", "freq_hz": float(c), "q": float(q), "stages": 1, "amount": "Classic"}
            for c in np.geomspace(70.0, 14000.0, int(bands))
        ]
    if layout in {"ndsp-band-eq", "ndsp-foundation-eq"}:
        return make_ndsp_band_eq_layout()

    # Coarse musical residual first: broad shelves/peaks that resemble how a
    # human would fit the voicing before adding any narrow correction.
    return [
        {"kind": "LowShelf",  "freq_hz": 95.0,    "q": 0.707, "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 150.0,   "q": 0.70,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 260.0,   "q": 0.70,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 430.0,   "q": 0.75,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 740.0,   "q": 0.80,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 1100.0,  "q": 0.85,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 1800.0,  "q": 0.85,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 2800.0,  "q": 0.85,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 4300.0,  "q": 0.90,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 6800.0,  "q": 0.90,  "stages": 1, "amount": "Classic"},
        {"kind": "Peak",      "freq_hz": 10000.0, "q": 0.90,  "stages": 1, "amount": "Classic"},
        {"kind": "HighShelf", "freq_hz": 12500.0, "q": 0.707, "stages": 1, "amount": "Classic"},
    ]


def make_foundation_candidates() -> list[dict]:
    candidates: list[dict] = []
    # Plus-style foundation space: same musical candidate families, but with
    # wider Q limits so the fitter can choose either broader corrective curves
    # or more focused medium/high-mid moves when the residual genuinely asks.
    shelf_qs = (0.333, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
    peak_qs = (0.333, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
    filter_qs = (0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
    for freq in (70.0, 95.0, 125.0, 180.0, 250.0, 350.0):
        for q in shelf_qs:
            for stages in (1, 2):
                candidates.append({"kind": "LowShelf", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (40.0, 65.0, 95.0, 125.0, 180.0, 250.0, 350.0, 500.0):
        for q in filter_qs:
            for stages in (1, 2):
                candidates.append({"kind": "HighPass", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (125.0, 180.0, 250.0, 350.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0):
        for q in peak_qs:
            for stages in (1, 2):
                candidates.append({"kind": "Peak", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (650.0, 1000.0, 1600.0, 2500.0):
        for q in shelf_qs:
            for stages in (1, 2):
                candidates.append({"kind": "TiltShelf", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (2500.0, 3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 16000.0):
        for q in shelf_qs:
            for stages in (1, 2):
                candidates.append({"kind": "HighShelf", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 16000.0):
        for q in filter_qs:
            for stages in (1, 2):
                candidates.append({"kind": "LowPass", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    return candidates


def make_refine2_candidates() -> list[dict]:
    candidates: list[dict] = []
    shelf_qs = (0.333, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0)
    peak_qs = (0.333, 0.5, 0.75, 1.0, 1.5, 2.25, 3.0)
    for freq in (65.0, 95.0, 125.0, 180.0, 250.0, 350.0):
        for q in shelf_qs:
            for stages in (1, 2):
                candidates.append({"kind": "LowShelf", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (125.0, 180.0, 250.0, 350.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0):
        for q in peak_qs:
            for stages in (1, 2):
                candidates.append({"kind": "Peak", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (650.0, 1000.0, 1600.0, 2500.0):
        for q in shelf_qs:
            for stages in (1, 2):
                candidates.append({"kind": "TiltShelf", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    for freq in (2500.0, 3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 16000.0):
        for q in shelf_qs:
            for stages in (1, 2):
                candidates.append({"kind": "HighShelf", "freq_hz": freq, "q": q, "stages": stages, "amount": "Classic"})
    return candidates


def layouts_too_close(a: dict, b: dict) -> bool:
    if a.get("kind") != b.get("kind"):
        return False
    af = float(a.get("freq_hz", 0.0))
    bf = float(b.get("freq_hz", 0.0))
    if af <= 0.0 or bf <= 0.0:
        return False
    return abs(math.log2(af / bf)) < 0.18


def band_response_db(freq: np.ndarray, sr: float, band: dict, gain_db: float) -> np.ndarray:
    kind = str(band.get("kind", "Peak"))
    f0 = float(band["freq_hz"])
    q = float(band.get("q", 1.0))
    stages = max(1, int(band.get("stages", 1)))
    stage_gain = float(gain_db) / float(stages)
    if kind == "LowShelf":
        return stages * shelf_eq_response_db(freq, sr, f0, q, stage_gain, high_shelf=False)
    if kind == "HighShelf":
        return stages * shelf_eq_response_db(freq, sr, f0, q, stage_gain, high_shelf=True)
    if kind == "LowPass":
        return stages * lowpass_response_db(freq, sr, f0, q)
    if kind == "HighPass":
        return stages * highpass_response_db(freq, sr, f0, q)
    if kind == "TiltShelf":
        return tilt_shelf_response_db(freq, sr, f0, q, gain_db, stages)
    return stages * peak_eq_response_db(freq, sr, f0, q, stage_gain)


def filterbank_response_db(freq: np.ndarray, sr: float, layout: list[dict], gains: np.ndarray) -> np.ndarray:
    out = np.zeros_like(freq, dtype=np.float64)
    for band, gain in zip(layout, gains):
        out += band_response_db(freq, sr, band, float(gain))
    return out

def resample_curves_to_log_grid(freq: np.ndarray, curves: list[np.ndarray], lo_hz: float, hi_hz: float, points: int) -> tuple[np.ndarray, list[np.ndarray]]:
    """Resample residual curves to a uniform log-frequency grid for fitting.

    FFT/Welch bins are linear in Hz. Fitting directly on those bins gives the
    upper spectrum much more objective weight than equivalent low-frequency
    octaves. The promoted filters are still the real DSP peak biquads; this only
    changes the analysis grid used to score and solve them.
    """
    freq = np.asarray(freq, dtype=np.float64)
    mask = np.isfinite(freq) & (freq > 0.0)
    if int(np.count_nonzero(mask)) < 4:
        return freq, [np.asarray(c, dtype=np.float64) for c in curves]

    lo = max(float(lo_hz), float(np.min(freq[mask])), 1.0)
    hi = min(float(hi_hz), float(np.max(freq[mask])))
    if hi <= lo:
        return freq[mask], [np.asarray(c, dtype=np.float64)[mask] for c in curves]

    grid = np.geomspace(lo, hi, max(128, int(points)))
    log_src = np.log2(freq[mask])
    log_grid = np.log2(grid)
    out_curves: list[np.ndarray] = []
    for curve in curves:
        c = np.asarray(curve, dtype=np.float64)
        c_valid = c[mask]
        out_curves.append(np.interp(log_grid, log_src, c_valid, left=float(c_valid[0]), right=float(c_valid[-1])))
    return grid, out_curves

def run_pycma(objective, dim: int, sigma: float, iterations: int, popsize: int, seed: int,
              lower: float, upper: float):
    if cma is None:
        raise RuntimeError(
            "Python package 'cma' is required for this script. Install it with: python -m pip install cma"
        )

    opts = {
        "bounds": [lower, upper],
        "popsize": popsize,
        "seed": seed,
        "maxiter": iterations,
        "verb_disp": 1,
        "verb_log": 0,
    }
    es = cma.CMAEvolutionStrategy([0.0] * dim, sigma, opts)
    while not es.stop():
        xs = es.ask()
        scores = [float(objective(np.asarray(x, dtype=np.float64))) for x in xs]
        es.tell(xs, scores)
        es.disp()
    result = es.result
    return np.asarray(result.xbest, dtype=np.float64), float(result.fbest)


def shortlist_foundation_candidates(freq: np.ndarray, sr: float, current_layout: list[dict],
                                    current_gains: np.ndarray, target_db: np.ndarray,
                                    candidates: list[dict], max_gain_db: float,
                                    limit: int) -> list[dict]:
    if limit <= 0 or len(candidates) <= limit:
        return list(candidates)

    base_pred = filterbank_response_db(freq, sr, current_layout, current_gains)
    residual = target_db - base_pred

    # Fast NumPy pre-rank: evaluate the broad candidate field as a matrix, then
    # exact-rank only an expanded shortlist. This keeps the final selection path
    # faithful to the DSP response while avoiding thousands of Python score loops.
    unit_rows: list[np.ndarray] = []
    row_candidates: list[dict] = []
    row_is_filter: list[bool] = []
    for cand in candidates:
        unit = band_response_db(freq, sr, cand, 1.0)
        if not np.all(np.isfinite(unit)):
            continue
        unit_rows.append(np.asarray(unit, dtype=np.float64))
        row_candidates.append(cand)
        row_is_filter.append(str(cand.get("kind", "Peak")) in {"LowPass", "HighPass"})

    if not unit_rows:
        return list(candidates[:limit])

    units = np.vstack(unit_rows)
    is_filter = np.asarray(row_is_filter, dtype=bool)
    approx_scores = np.empty(len(row_candidates), dtype=np.float64)

    if np.any(is_filter):
        filter_err = units[is_filter] - residual[None, :]
        approx_scores[is_filter] = np.mean(filter_err * filter_err, axis=1)

    if np.any(~is_filter):
        gain_units = units[~is_filter]
        denom = np.sum(gain_units * gain_units, axis=1)
        numer = gain_units @ residual
        gains = np.zeros_like(numer)
        valid = denom > 1.0e-12
        gains[valid] = np.clip(numer[valid] / denom[valid], -max_gain_db, max_gain_db)
        gain_err = gain_units * gains[:, None] - residual[None, :]
        approx_scores[~is_filter] = np.mean(gain_err * gain_err, axis=1)

    # Verify more candidates than requested, because shelving/biquad dB response
    # is not perfectly linear in gain. The exact pass below is still cheap.
    exact_pool_size = min(len(row_candidates), max(int(limit) * 3, int(limit) + 24))
    exact_indices = np.argsort(approx_scores)[:exact_pool_size]

    ranked: list[tuple[float, dict]] = []
    for idx in exact_indices:
        cand = row_candidates[int(idx)]
        kind = str(cand.get("kind", "Peak"))
        unit = units[int(idx)]
        if kind in {"LowPass", "HighPass"}:
            pred = base_pred + unit
        else:
            denom = float(np.dot(unit, unit))
            if denom <= 1.0e-12:
                continue
            gain = float(np.clip(np.dot(residual, unit) / denom, -max_gain_db, max_gain_db))
            pred = base_pred + band_response_db(freq, sr, cand, gain)
        score = float(np.mean((pred - target_db) ** 2))
        ranked.append((score, cand))

    ranked.sort(key=lambda item: item[0])
    return [cand for _, cand in ranked[:limit]]


def linear_layout_score(freq: np.ndarray, sr: float, layout: list[dict],
                        target_db: np.ndarray, max_gain_db: float,
                        smooth_weight: float) -> tuple[np.ndarray, float]:
    gains = linear_prefit_gains(freq, sr, layout, target_db, max_gain_db, smooth_weight)
    score = float(np.mean((filterbank_response_db(freq, sr, layout, gains) - target_db) ** 2))
    return gains, score


def run_foundation_then_grid_fit(freq: np.ndarray, sr: float, base_layout: list[dict],
                                 target_db: np.ndarray, max_gain_db: float,
                                 smooth_weight: float, max_foundation: int = 3,
                                 max_refine: int = 2,
                                 prefilter_limit: int = 96,
                                 exact_limit: int = 12) -> tuple[list[dict], np.ndarray, float]:
    selected: list[dict] = []
    best_layout = list(base_layout)
    best_gains, best_score = run_least_squares_fit(freq, sr, best_layout, target_db, max_gain_db, smooth_weight)
    candidates = make_foundation_candidates()

    for _ in range(max(0, int(max_foundation))):
        eligible = [cand for cand in candidates if not any(layouts_too_close(cand, existing) for existing in selected)]
        shortlist = shortlist_foundation_candidates(freq, sr, best_layout, best_gains, target_db,
                                                    eligible, max_gain_db, prefilter_limit)
        ranked: list[tuple[float, dict, np.ndarray, list[dict]]] = []
        for cand in shortlist:
            trial_layout = selected + [cand] + list(base_layout)
            gains, score = linear_layout_score(freq, sr, trial_layout, target_db, max_gain_db, smooth_weight)
            ranked.append((score, cand, gains, trial_layout))
        ranked.sort(key=lambda item: item[0])
        if exact_limit > 0:
            ranked = ranked[:max(1, int(exact_limit))]

        round_best = None
        for _, cand, _, trial_layout in ranked:
            gains, score = run_least_squares_fit(freq, sr, trial_layout, target_db, max_gain_db, smooth_weight)
            if round_best is None or score < round_best[0]:
                round_best = (score, cand, gains, trial_layout)
        if round_best is None:
            break
        score, cand, gains, trial_layout = round_best
        if score >= best_score * 0.997:
            break
        selected.append(cand)
        best_layout = trial_layout
        best_gains = gains
        best_score = float(score)

    refine_selected: list[dict] = []
    refine_candidates = make_refine2_candidates()
    for _ in range(max(0, int(max_refine))):
        eligible = [cand for cand in refine_candidates
                    if not any(layouts_too_close(cand, existing) for existing in refine_selected)]
        shortlist = shortlist_foundation_candidates(freq, sr, best_layout, best_gains, target_db,
                                                    eligible, max_gain_db, prefilter_limit)
        ranked: list[tuple[float, dict, np.ndarray, list[dict]]] = []
        for cand in shortlist:
            trial_layout = list(best_layout) + [cand]
            gains, score = linear_layout_score(freq, sr, trial_layout, target_db, max_gain_db, smooth_weight)
            ranked.append((score, cand, gains, trial_layout))
        ranked.sort(key=lambda item: item[0])
        if exact_limit > 0:
            ranked = ranked[:max(1, int(exact_limit))]

        round_best = None
        for _, cand, _, trial_layout in ranked:
            gains, score = run_least_squares_fit(freq, sr, trial_layout, target_db, max_gain_db, smooth_weight)
            if round_best is None or score < round_best[0]:
                round_best = (score, cand, gains, trial_layout)
        if round_best is None:
            break
        score, cand, gains, trial_layout = round_best
        if score >= best_score * 0.997:
            break
        refine_selected.append(cand)
        best_layout = trial_layout
        best_gains = gains
        best_score = float(score)

    # Final refit after selection keeps the promoted gains consistent.
    best_gains, best_score = run_least_squares_fit(freq, sr, best_layout, target_db, max_gain_db, smooth_weight)
    return best_layout, best_gains, best_score


def linear_prefit_gains(freq: np.ndarray, sr: float, layout: list[dict],
                        target_db: np.ndarray, max_gain_db: float,
                        smooth_weight: float) -> np.ndarray:
    """Cheap bounded linear prefit used only as a starting point.

    Biquad gain is not perfectly linear in dB for every filter family, so this
    is deliberately not the final answer. It gives scipy's nonlinear refit a
    much better first guess without changing the DSP contract.
    """
    band_count = int(len(layout))
    if band_count <= 0:
        return np.zeros(0, dtype=np.float64)

    basis = np.zeros((len(freq), band_count), dtype=np.float64)
    for i, band in enumerate(layout):
        unit = band_response_db(freq, sr, band, 1.0)
        if not np.all(np.isfinite(unit)):
            unit = np.zeros_like(freq, dtype=np.float64)
        basis[:, i] = unit

    y = np.asarray(target_db, dtype=np.float64)
    a = basis
    b = y
    if smooth_weight > 0.0 and band_count >= 3:
        smooth = np.zeros((band_count - 2, band_count), dtype=np.float64)
        for i in range(band_count - 2):
            smooth[i, i] = 1.0
            smooth[i, i + 1] = -2.0
            smooth[i, i + 2] = 1.0
        a = np.vstack([basis, smooth * smooth_weight])
        b = np.concatenate([y, np.zeros(band_count - 2, dtype=np.float64)])

    try:
        result = optimize.lsq_linear(a, b, bounds=(-max_gain_db, max_gain_db), lsmr_tol="auto")
        if result.success and np.all(np.isfinite(result.x)):
            return np.asarray(result.x, dtype=np.float64)
    except Exception:
        pass
    return np.zeros(band_count, dtype=np.float64)


def run_least_squares_fit(freq: np.ndarray, sr: float, layout: list[dict],
                          target_db: np.ndarray, max_gain_db: float,
                          smooth_weight: float) -> tuple[np.ndarray, float]:
    """Fit the same mixed biquad family that the DSP will apply."""
    band_count = int(len(layout))

    def residual(gains: np.ndarray) -> np.ndarray:
        pred = filterbank_response_db(freq, sr, layout, gains)
        smooth_penalty = np.diff(gains, n=2) * smooth_weight if band_count >= 3 else np.array([])
        return np.concatenate([pred - target_db, smooth_penalty])

    x0 = linear_prefit_gains(freq, sr, layout, target_db, max_gain_db, smooth_weight)
    result = optimize.least_squares(
        residual,
        x0,
        bounds=(-max_gain_db, max_gain_db),
        max_nfev=4000,
        x_scale=np.ones(band_count, dtype=np.float64) * max(max_gain_db * 0.25, 1.0),
    )
    score = float(np.mean((filterbank_response_db(freq, sr, layout, result.x) - target_db) ** 2))
    return np.asarray(result.x, dtype=np.float64), score


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--render-dir", required=True)
    ap.add_argument("--sat-file", default=None)
    ap.add_argument("--target-file", default=None)
    ap.add_argument("--case-meta", default=None, help="Optional JSON object/string stored in fit_summary.json.")
    ap.add_argument("--sat-render", choices=["sat_raw", "sat_voiced"], default="sat_raw",
                    help="SAT render to fit against target. sat_voiced uses overdrive_id_batch__sat_voiced.wav when present.")
    ap.add_argument("--out", default="SAT-TR/tools/overdrive_id_fit")
    ap.add_argument("--nfft", type=int, default=1024)
    ap.add_argument("--orders", default="1,2,3,5,7")
    ap.add_argument("--hammerstein-taps", type=int, default=128)
    ap.add_argument("--hammerstein-chunk-samples", type=int, default=8192,
                    help="Rows per chunk for Hammerstein normal-equation accumulation. Lower uses less RAM; model precision is unchanged.")
    ap.add_argument("--bands", type=int, default=48)
    ap.add_argument("--fit-layout", choices=["tone", "ndsp-band-eq", "ndsp-foundation-eq", "peak"], default="ndsp-foundation-eq",
                    help="tone fits broad shelves/peaks; ndsp-band-eq uses a fixed 65/125..16k grid; ndsp-foundation-eq adds up to 3 broad learned bands before that grid; peak keeps the legacy all-peak bank.")
    ap.add_argument("--basis-q", type=float, default=2.0)
    ap.add_argument("--max-gain-db", type=float, default=18.0)
    ap.add_argument("--optimizer", choices=["least-squares", "cma"], default="least-squares",
                    help="Optimizer for the static residual filterbank. Use CMA only for comparison; it does not tune core DSP params.")
    ap.add_argument("--cma-iters", type=int, default=260)
    ap.add_argument("--popsize", type=int, default=72)
    ap.add_argument("--seed", type=int, default=808)
    ap.add_argument("--trim-ms", type=float, default=40.0)
    ap.add_argument("--kind-filter", default="pink,brown,white,multitone,sweep,stepsine,twotone_lowmid,twotone_himid,tritone_mid,tritone_himid,white_sweep_level,pink_sweep_level,brown_sweep_level")
    ap.add_argument("--skip-hammerstein", action="store_true",
                    help="Skip Hammerstein branch diagnostics; static residual score/fit is unchanged.")
    ap.add_argument("--no-plot", action="store_true",
                    help="Skip PNG plot generation for fast iterative runs.")
    ap.add_argument("--fit-grid", choices=["log", "linear"], default="log",
                    help="Frequency grid used for residual fitting/scoring. Log gives each octave comparable weight.")
    ap.add_argument("--fit-grid-points", type=int, default=512,
                    help="Number of points for --fit-grid log. This is independent of FFT nfft.")
    ap.add_argument("--foundation-prefilter-limit", type=int, default=96,
                    help="Cheap linear shortlist size for foundation-band fitting. 0 disables prefiltering.")
    ap.add_argument("--foundation-exact-limit", type=int, default=12,
                    help="Number of linearly ranked foundation candidates that receive nonlinear least-squares refit. 0 refits the full shortlist.")
    ap.add_argument("--fit-lo-hz", type=float, default=40.0)
    ap.add_argument("--fit-hi-hz", type=float, default=18000.0)
    ap.add_argument("--feature-cache-dir", default=None,
                    help="Shared cache directory for target-side spectral features. Does not change scoring.")
    args = ap.parse_args()

    stim_dir = Path(args.stim_dir)
    render_dir = Path(args.render_dir)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    manifest = json.loads((stim_dir / "manifest.json").read_text(encoding="utf-8"))
    names = manifest["batch_render_naming"]
    dry, sr0 = read_mono(stim_dir / manifest["batch_file"])
    sat_name = names["sat_raw"]
    if args.sat_render == "sat_voiced":
        sat_name = names.get("sat_voiced_optional", manifest["batch_file"].replace(".wav", "__sat_voiced.wav"))
    if args.sat_file:
        sat_name = args.sat_file
    target_name = args.target_file or names["target"]
    sat_path = render_dir / sat_name
    if not sat_path.exists():
        raise RuntimeError(f"SAT render not found for {args.sat_render}: {sat_path}")
    sat, sr1 = read_mono(sat_path)
    target, sr2 = read_mono(render_dir / target_name)
    if len({sr0, sr1, sr2}) != 1:
        raise RuntimeError(f"sample-rate mismatch: {sr0}, {sr1}, {sr2}")

    feature_cache_dir = Path(args.feature_cache_dir) if args.feature_cache_dir else None
    target_cache_key = feature_cache_key(
        target_path=render_dir / target_name,
        manifest_path=stim_dir / "manifest.json",
        nfft=args.nfft,
        trim_ms=args.trim_ms,
        kind_filter=args.kind_filter,
    )
    target_mag_cache = load_target_mag_cache(feature_cache_dir, target_cache_key)
    target_mag_cache_dirty = False

    allowed = {x.strip() for x in args.kind_filter.split(",") if x.strip()}
    orders = [int(x.strip()) for x in args.orders.split(",") if x.strip()]
    edge = int(sr0 * args.trim_ms / 1000.0)

    freq_ref = None
    residual_curves = []
    low_curves = []
    high_curves = []
    levels = []
    hammer_rows = []

    for seg in manifest["segments"]:
        if seg["kind"] not in allowed:
            continue
        start = int(seg["start_sample"]) + edge
        end = int(seg["end_sample"]) - edge
        if end <= start or end > min(len(dry), len(sat), len(target)):
            continue
        x = dry[start:end]
        y_sat = sat[start:end]
        y_tgt = target[start:end]

        f, mag_sat = welch_mag_db(y_sat, sr0, args.nfft)
        seg_key = f"{seg['stem']}|{seg['kind']}|{start}|{end}"
        mag_tgt = target_mag_cache.get(seg_key)
        if mag_tgt is None or len(mag_tgt) != len(mag_sat):
            _, mag_tgt = welch_mag_db(y_tgt, sr0, args.nfft)
            target_mag_cache[seg_key] = mag_tgt.copy()
            target_mag_cache_dirty = True
        delta = smooth_log_curve(f, mag_tgt - mag_sat)
        if freq_ref is None:
            freq_ref = f
        residual_curves.append(delta)
        levels.append(float(seg["level_dbfs_rms"]))

        if not args.skip_hammerstein:
            br_sat = hammerstein_branches(x, y_sat, sr0, orders, args.hammerstein_taps, args.hammerstein_chunk_samples)
            br_tgt = hammerstein_branches(x, y_tgt, sr0, orders, args.hammerstein_taps, args.hammerstein_chunk_samples)
            row = {"stem": seg["stem"], "kind": seg["kind"], "level_dbfs_rms": seg["level_dbfs_rms"],
                   "sat_render": args.sat_render, "sat_rms_db": rms_db(y_sat), "target_rms_db": rms_db(y_tgt)}
            for p in orders:
                _, sat_db = branch_mag_db(br_sat[p], args.nfft)
                _, tgt_db = branch_mag_db(br_tgt[p], args.nfft)
                mask = valid_band(f)
                row[f"order_{p}_branch_median_delta_db"] = float(np.median((tgt_db - sat_db)[mask]))
                row[f"order_{p}_branch_energy_sat_db"] = rms_db(br_sat[p])
                row[f"order_{p}_branch_energy_target_db"] = rms_db(br_tgt[p])
            hammer_rows.append(row)
    if target_mag_cache_dirty:
        save_target_mag_cache(feature_cache_dir, target_cache_key, target_mag_cache)

    if not residual_curves or freq_ref is None:
        raise RuntimeError("No usable segments found")

    curves = np.vstack(residual_curves)
    levels_arr = np.array(levels)
    low_mask = levels_arr <= np.percentile(levels_arr, 35)
    high_mask = levels_arr >= np.percentile(levels_arr, 65)
    invariant = np.median(curves, axis=0)
    low_curve = np.median(curves[low_mask], axis=0)
    high_curve = np.median(curves[high_mask], axis=0)
    dynamic_delta = high_curve - low_curve

    fit_mask = valid_band(freq_ref, args.fit_lo_hz, args.fit_hi_hz)
    layout = make_filter_layout(args.fit_layout, args.bands, args.basis_q)
    centers = np.array([float(b["freq_hz"]) for b in layout], dtype=np.float64)
    linear_fit_freq = freq_ref[fit_mask]
    linear_target_db = invariant[fit_mask]
    linear_dynamic_db = dynamic_delta[fit_mask]
    if args.fit_grid == "log":
        fit_freq, (target_db, dynamic_db) = resample_curves_to_log_grid(
            linear_fit_freq, [linear_target_db, linear_dynamic_db],
            args.fit_lo_hz, args.fit_hi_hz, args.fit_grid_points)
    else:
        fit_freq = linear_fit_freq
        target_db = linear_target_db
        dynamic_db = linear_dynamic_db
    basis = gaussian_basis(fit_freq, centers, args.basis_q)  # compatibility path for --optimizer cma

    def objective(gains: np.ndarray) -> float:
        pred = filterbank_response_db(fit_freq, sr0, layout, gains)
        err = pred - target_db
        smooth_pen = np.diff(gains, n=2) if len(gains) >= 3 else np.array([])
        # Penalize trying to fit level-dependent mismatch with static EQ.
        dynamic_pen = np.maximum(0.0, np.abs(dynamic_db) - 1.5)
        return float(np.mean(err * err) + 0.08 * np.mean(smooth_pen * smooth_pen) + 0.18 * np.mean(dynamic_pen * dynamic_pen))

    if args.optimizer == "cma":
        best, best_score = run_pycma(objective, len(layout), sigma=2.0, iterations=args.cma_iters,
                                     popsize=args.popsize, seed=args.seed,
                                     lower=-args.max_gain_db, upper=args.max_gain_db)
    elif args.fit_layout == "ndsp-foundation-eq":
        layout, best, best_score = run_foundation_then_grid_fit(
            fit_freq, sr0, layout, target_db, args.max_gain_db,
            smooth_weight=0.18, max_foundation=3, max_refine=2,
            prefilter_limit=args.foundation_prefilter_limit,
            exact_limit=args.foundation_exact_limit)
    else:
        best, best_score = run_least_squares_fit(fit_freq, sr0, layout, target_db,
                                                 args.max_gain_db, smooth_weight=0.55 if args.fit_layout == "tone" else 0.35)

    layer_tags = tag_post_layers(layout)
    # Candidate/full-guard folders are disposable and can be recreated between
    # stages, so guarantee the output directory immediately before every write.
    out.mkdir(parents=True, exist_ok=True)
    with (out / "cma_filterbank_fit.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["layer", "kind", "freq_hz", "gain_db", "q", "q_basis", "stages", "amount"])
        writer.writeheader()
        for layer, band, gain in zip(layer_tags, layout, best):
            writer.writerow({
                "layer": layer,
                "kind": band.get("kind", "Peak"),
                "freq_hz": float(band["freq_hz"]),
                "gain_db": float(gain),
                "q": float(band.get("q", args.basis_q)),
                "q_basis": float(band.get("q", args.basis_q)),
                "stages": int(band.get("stages", 1)),
                # The fitted bank is an external residual EQ: its gains are
                # already absolute. Mark it Fixed so the C++ renderer does not
                # rescale the correction by type/classic/drive morph amounts.
                "amount": "Fixed",
            })

    with (out / "hammerstein_branch_summary.csv").open("w", newline="", encoding="utf-8") as f:
        if hammer_rows:
            fields = sorted({k for row in hammer_rows for k in row.keys()})
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(hammer_rows)
        else:
            writer = csv.DictWriter(f, fieldnames=["skipped", "reason"])
            writer.writeheader()
            writer.writerow({"skipped": True, "reason": "--skip-hammerstein"})
    with (out / "residual_curves.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["freq_hz", f"target_minus_{args.sat_render}_median_db", "low_level_db", "high_level_db", "high_minus_low_db", "static_residual_eq_db"])
        pred_full = np.zeros_like(freq_ref)
        pred_full[fit_mask] = filterbank_response_db(linear_fit_freq, sr0, layout, best)
        for i in range(len(freq_ref)):
            writer.writerow([f"{freq_ref[i]:.6f}", f"{invariant[i]:.6f}", f"{low_curve[i]:.6f}",
                             f"{high_curve[i]:.6f}", f"{dynamic_delta[i]:.6f}", f"{pred_full[i]:.6f}"])

    if not args.no_plot:
        try:
            import matplotlib.pyplot as plt
            mask = valid_band(freq_ref)
            pred = np.zeros_like(freq_ref)
            pred[mask] = filterbank_response_db(freq_ref[mask], sr0, layout, best)
            remaining = invariant - pred
            fig, axes = plt.subplots(3, 1, figsize=(13, 12), sharex=True)

            axes[0].semilogx(freq_ref[mask], invariant[mask], label=f"target - {args.sat_render}")
            axes[0].semilogx(freq_ref[mask], pred[mask], label="promoted EQ approximation")
            axes[0].axhline(0.0, color="black", linewidth=0.8)
            axes[0].set_ylabel("dB")
            axes[0].set_title(f"Static residual fit, score={best_score:.4f}")
            axes[0].legend()

            axes[1].semilogx(freq_ref[mask], remaining[mask], label="remaining after EQ")
            axes[1].semilogx(freq_ref[mask], dynamic_delta[mask], label="level-dependent residual", alpha=0.75)
            axes[1].axhline(0.0, color="black", linewidth=0.8)
            axes[1].set_ylabel("dB")
            axes[1].legend()

            band_freqs = [float(b["freq_hz"]) for b in layout]
            band_labels = [str(b.get("kind", "Peak")) for b in layout]
            axes[2].semilogx(freq_ref[mask], pred[mask], label="total EQ")
            axes[2].scatter(band_freqs, best, s=30, label="band gains")
            for f0, gain, label in zip(band_freqs, best, band_labels):
                axes[2].annotate(label[:2], (f0, gain), textcoords="offset points", xytext=(0, 5),
                                 ha="center", fontsize=7)
            axes[2].axhline(0.0, color="black", linewidth=0.8)
            axes[2].set_xlabel("Hz")
            axes[2].set_ylabel("dB")
            axes[2].legend()

            for ax in axes:
                ax.grid(True, which="both", alpha=0.25)
            plt.tight_layout()
            plt.savefig(out / "hammerstein_cma_fit.png", dpi=160)
            plt.close(fig)
        except Exception as exc:
            print(f"plot skipped: {exc}")

    q_tag = str(float(args.basis_q)).replace(".", "p").replace("-", "m")
    gain_tag = str(float(args.max_gain_db)).replace(".", "p").replace("-", "m")
    filter_model = (
        f"dsp_{args.fit_layout}_biquad_{args.fit_grid}grid_b{len(layout)}_q{q_tag}_g{gain_tag}"
    )

    summary = {
        "score": best_score,
        "bands": len(layout),
        "requested_bands": args.bands,
        "fit_layout": args.fit_layout,
        "basis_q": args.basis_q,
        "orders": orders,
        "hammerstein_taps": args.hammerstein_taps,
        "hammerstein_chunk_samples": args.hammerstein_chunk_samples,
        "skip_hammerstein": bool(args.skip_hammerstein),
        "nfft": int(args.nfft),
        "no_plot": bool(args.no_plot),
        "optimizer": args.optimizer,
        "filter_model": filter_model,
        "fit_grid": args.fit_grid,
        "fit_grid_points": int(args.fit_grid_points),
        "foundation_prefilter_limit": int(args.foundation_prefilter_limit),
        "fit_lo_hz": float(args.fit_lo_hz),
        "fit_hi_hz": float(args.fit_hi_hz),
        "cma_iters": args.cma_iters if args.optimizer == "cma" else 0,
        "popsize": args.popsize if args.optimizer == "cma" else 0,
        "sat_render": args.sat_render,
        "sat_render_file": str(sat_path),
        "target_file": str(render_dir / target_name),
        "case_meta": json.loads(args.case_meta) if args.case_meta else None,
        "fit_cascade_output": "post_a -> post_ndsp -> post_b",
        "note": "This static fit emits explicit post cascade layers using the current contract: one 3EQ foundation before the fixed NDSP grid plus a two-band post_b plateau refinement. It does not claim to solve pre/core nonlinearity; pre/core candidates must be rendered and verified because they change harmonic generation."
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / "fit_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Wrote Hammerstein + static residual fit to {out} using {args.sat_render}")
    print(f"Best {args.optimizer} static-EQ score: {best_score:.6f}")


if __name__ == "__main__":
    main()
