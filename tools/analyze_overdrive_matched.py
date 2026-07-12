#!/usr/bin/env python3
"""Level-matched analysis for SAT-TR Overdrive renders.

Use this when the host/plugin output level was manually adjusted and the goal is
pre/post EQ or voicing comparison rather than absolute trim calibration.

The script keeps a per-segment gain report so dynamics are not hidden: if the
required match gain changes by level, that is dynamic/core behavior, not EQ.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, optimize, signal

EPS = 1.0e-12


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    x, sr = sf.read(path, always_2d=True)
    return np.mean(x, axis=1).astype(np.float64), int(sr)


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x * x)))


def rms_db(x: np.ndarray) -> float:
    return 20.0 * math.log10(max(rms(x), EPS))


def peak(x: np.ndarray) -> float:
    return float(np.max(np.abs(x)))


def gain_to_match(src: np.ndarray, ref: np.ndarray, mode: str) -> float:
    if mode.endswith('rms'):
        return max(rms(ref), EPS) / max(rms(src), EPS)
    if mode.endswith('peak'):
        return max(peak(ref), EPS) / max(peak(src), EPS)
    return 1.0


def gain_db(g: float) -> float:
    return 20.0 * math.log10(max(float(g), EPS))


def welch_mag_db(x: np.ndarray, sr: int, nfft: int) -> tuple[np.ndarray, np.ndarray]:
    nperseg = min(nfft, max(256, len(x) // 2))
    f, p = signal.welch(x, fs=sr, window='hann', nperseg=nperseg, noverlap=nperseg // 2,
                        nfft=nfft, detrend=False, scaling='spectrum')
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
    return [(float(c), float(g), float(q)) for c, g in zip(centers, result.x)]


def write_curve_csv(path: Path, freq: np.ndarray, cols: dict[str, np.ndarray]) -> None:
    with path.open('w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['freq_hz', *cols.keys()])
        for i in range(len(freq)):
            writer.writerow([f'{freq[i]:.6f}', *[f'{v[i]:.6f}' for v in cols.values()]])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--stim-dir', default='SAT-TR/tools/overdrive_id_stimuli')
    ap.add_argument('--render-dir', required=True)
    ap.add_argument('--sat-file', default=None)
    ap.add_argument('--target-file', default=None)
    ap.add_argument('--case-meta', default=None, help='Optional JSON object/string stored in level_match_summary.json.')
    ap.add_argument('--out', default='SAT-TR/tools/overdrive_id_analysis_matched')
    ap.add_argument('--sat-render', choices=['sat_raw', 'sat_voiced'], default='sat_voiced')
    ap.add_argument('--level-match', choices=['global-rms', 'global-peak', 'segment-rms', 'segment-peak'], default='global-rms')
    ap.add_argument('--nfft', type=int, default=65536)
    ap.add_argument('--band-count', type=int, default=48)
    ap.add_argument('--max-gain-db', type=float, default=12.0)
    ap.add_argument('--basis-q', type=float, default=2.0)
    ap.add_argument('--kind-filter', default='pink,brown,white,multitone,sweep,stepsine,twotone_lowmid,twotone_himid,tritone_mid,tritone_himid,white_sweep_level,pink_sweep_level,brown_sweep_level')
    ap.add_argument('--trim-ms', type=float, default=40.0)
    args = ap.parse_args()

    stim_dir = Path(args.stim_dir)
    render_dir = Path(args.render_dir)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    manifest = json.loads((stim_dir / 'manifest.json').read_text(encoding='utf-8'))
    names = manifest['batch_render_naming']
    sat_name = args.sat_file or (names['sat_raw'] if args.sat_render == 'sat_raw' else names.get('sat_voiced_optional', manifest['batch_file'].replace('.wav', '__sat_voiced.wav')))
    target_name = args.target_file or names['target']

    dry, sr0 = read_mono(stim_dir / manifest['batch_file'])
    sat, sr1 = read_mono(render_dir / sat_name)
    target, sr2 = read_mono(render_dir / target_name)
    if len({sr0, sr1, sr2}) != 1:
        raise RuntimeError(f'sample-rate mismatch: dry={sr0}, sat={sr1}, target={sr2}')

    allowed = {x.strip() for x in args.kind_filter.split(',') if x.strip()}
    edge = int(sr0 * args.trim_ms / 1000.0)
    global_gain = gain_to_match(sat, target, args.level_match) if args.level_match.startswith('global') else 1.0

    curves = []
    levels = []
    metric_rows = []
    freq_ref = None

    for seg in manifest['segments']:
        if seg['kind'] not in allowed:
            continue
        start = int(seg['start_sample']) + edge
        end = int(seg['end_sample']) - edge
        if end <= start or end > min(len(dry), len(sat), len(target)):
            continue
        sat_seg = sat[start:end]
        target_seg = target[start:end]
        local_gain = gain_to_match(sat_seg, target_seg, args.level_match) if args.level_match.startswith('segment') else global_gain
        sat_matched = sat_seg * local_gain
        freq, sat_mag = welch_mag_db(sat_matched, sr0, args.nfft)
        _, target_mag = welch_mag_db(target_seg, sr0, args.nfft)
        delta = smooth_log_curve(freq, target_mag - sat_mag)
        if freq_ref is None:
            freq_ref = freq
        curves.append(delta)
        levels.append(float(seg['level_dbfs_rms']))
        metric_rows.append({
            'stem': seg['stem'],
            'kind': seg['kind'],
            'level_dbfs_rms': seg['level_dbfs_rms'],
            'sat_original_rms_db': rms_db(sat_seg),
            'target_rms_db': rms_db(target_seg),
            'sat_original_peak_db': gain_db(peak(sat_seg)),
            'target_peak_db': gain_db(peak(target_seg)),
            'sat_match_gain_db': gain_db(local_gain),
            'sat_matched_rms_db': rms_db(sat_matched),
            'sat_matched_peak_db': gain_db(peak(sat_matched)),
            'target_minus_sat_original_rms_db': rms_db(target_seg) - rms_db(sat_seg),
            'target_minus_sat_matched_rms_db': rms_db(target_seg) - rms_db(sat_matched),
            'target_minus_sat_original_peak_db': gain_db(peak(target_seg)) - gain_db(peak(sat_seg)),
            'target_minus_sat_matched_peak_db': gain_db(peak(target_seg)) - gain_db(peak(sat_matched)),
        })

    if not curves or freq_ref is None:
        raise RuntimeError('No usable segments found')

    curves_arr = np.vstack(curves)
    levels_arr = np.asarray(levels)
    low_mask = levels_arr <= np.percentile(levels_arr, 35)
    high_mask = levels_arr >= np.percentile(levels_arr, 65)
    invariant = np.median(curves_arr, axis=0)
    low_curve = np.median(curves_arr[low_mask], axis=0)
    high_curve = np.median(curves_arr[high_mask], axis=0)
    dynamic_delta = high_curve - low_curve

    write_curve_csv(out / 'spectral_residual_level_matched.csv', freq_ref, {
        f'target_minus_{args.sat_render}_{args.level_match}_median_db': invariant,
        'low_level_median_db': low_curve,
        'high_level_median_db': high_curve,
        'high_minus_low_db': dynamic_delta,
    })

    with (out / 'per_segment_level_match.csv').open('w', newline='', encoding='utf-8') as f:
        fields = list(metric_rows[0].keys())
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(metric_rows)

    bands = fit_residual_bands(freq_ref, invariant, args.band_count, args.max_gain_db, args.basis_q)
    with (out / 'suggested_filterbank_level_matched.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['freq_hz', 'gain_db', 'q_basis'])
        for c, g, q in bands:
            writer.writerow([f'{c:.6f}', f'{g:.6f}', f'{q:.6f}'])

    summary = {
        'sat_render': args.sat_render,
        'sat_render_file': str(render_dir / sat_name),
        'target_file': str(render_dir / target_name),
        'case_meta': json.loads(args.case_meta) if args.case_meta else None,
        'level_match': args.level_match,
        'global_match_gain_db': gain_db(global_gain),
        'segments': len(metric_rows),
        'note': 'Use spectral residual for pre/post voicing. Use sat_match_gain_db variation by level to inspect dynamic/core differences.'
    }
    (out / 'level_match_summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')

    try:
        import matplotlib.pyplot as plt
        mask = valid_band(freq_ref)
        plt.figure(figsize=(12, 7))
        plt.semilogx(freq_ref[mask], invariant[mask], label='median residual, level matched')
        plt.semilogx(freq_ref[mask], low_curve[mask], label='low level')
        plt.semilogx(freq_ref[mask], high_curve[mask], label='high level')
        plt.semilogx(freq_ref[mask], dynamic_delta[mask], label='high-low dynamic delta')
        plt.axhline(0.0, color='black', linewidth=0.8)
        plt.grid(True, which='both', alpha=0.25)
        plt.legend()
        plt.xlabel('Hz')
        plt.ylabel('dB')
        plt.title(f'{args.sat_render} vs target, {args.level_match}')
        plt.tight_layout()
        plt.savefig(out / 'residual_level_matched.png', dpi=160)
        plt.close()
    except Exception as exc:
        print(f'plot skipped: {exc}')

    print(f'Wrote level-matched analysis to {out}')
    print(f'Global match gain dB: {gain_db(global_gain):.3f}')


if __name__ == '__main__':
    main()
