#!/usr/bin/env python3
"""Generate a single batch WAV for SAT-TR Overdrive identification.

Default workflow creates ONE dry file:
  overdrive_id_batch.wav

Render that file twice in the host:
  overdrive_id_batch__sat_raw.wav
  overdrive_id_batch__target.wav

Then run analyze_overdrive_batch.py.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import signal


def db_to_amp(db: float) -> float:
    return float(10.0 ** (db / 20.0))


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x), dtype=np.float64)))


def set_rms(x: np.ndarray, dbfs: float) -> np.ndarray:
    return (x / max(rms(x), 1.0e-12) * db_to_amp(dbfs)).astype(np.float32)


def fade(x: np.ndarray, sr: int, ms: float = 20.0) -> np.ndarray:
    n = min(len(x) // 2, max(1, int(sr * ms / 1000.0)))
    w = np.linspace(0.0, 1.0, n, dtype=np.float32)
    y = x.copy()
    y[:n] *= w
    y[-n:] *= w[::-1]
    return y


def db_ramp(n: int, start_db: float, end_db: float) -> np.ndarray:
    if n <= 1:
        return np.ones(max(1, n), dtype=np.float32) * db_to_amp(end_db)
    db = np.linspace(float(start_db), float(end_db), n, dtype=np.float64)
    return (10.0 ** (db / 20.0)).astype(np.float32)


def normalize_peak(x: np.ndarray, peak_dbfs: float = -0.1) -> np.ndarray:
    peak = float(np.max(np.abs(x)))
    if peak <= 1.0e-12:
        return x.astype(np.float32)
    return (x / peak * db_to_amp(peak_dbfs)).astype(np.float32)


def colored_noise(kind: str, n: int, rng: np.random.Generator) -> np.ndarray:
    white = rng.standard_normal(n).astype(np.float64)
    if kind == "white":
        y = white
    else:
        freqs = np.fft.rfftfreq(n, 1.0)
        spec = np.fft.rfft(white)
        f = np.maximum(freqs, 1.0 / n)
        if kind == "pink":
            spec /= np.sqrt(f)
        elif kind == "brown":
            spec /= f
        else:
            raise ValueError(f"unsupported noise kind: {kind}")
        y = np.fft.irfft(spec, n=n)
    y -= np.mean(y)
    return y.astype(np.float32)


def swept_colored_noise(kind: str, sr: int, seconds: float, rng: np.random.Generator,
                        start_db: float = -144.0, end_db: float = 0.0) -> np.ndarray:
    n = int(sr * seconds)
    y = colored_noise(kind, n, rng).astype(np.float32)
    y = normalize_peak(y, -0.1)
    y = y * db_ramp(n, start_db, end_db)
    return fade(normalize_peak(y, -0.1), sr, 8.0)


def multitone(sr: int, seconds: float, rng: np.random.Generator) -> np.ndarray:
    n = int(sr * seconds)
    t = np.arange(n, dtype=np.float64) / sr
    freqs = np.array([82.41, 110.0, 164.81, 220.0, 329.63, 440.0,
                      659.25, 880.0, 1000.0, 1500.0, 2000.0, 3000.0,
                      5000.0, 8000.0], dtype=np.float64)
    phases = rng.uniform(0.0, 2.0 * np.pi, size=len(freqs))
    weights = 1.0 / np.sqrt(np.maximum(freqs / 440.0, 1.0))
    y = np.zeros(n, dtype=np.float64)
    for f, p, w in zip(freqs, phases, weights):
        y += w * np.sin(2.0 * np.pi * f * t + p)
    y -= np.mean(y)
    return y.astype(np.float32)


def log_sweep(sr: int, seconds: float) -> np.ndarray:
    n = int(sr * seconds)
    t = np.linspace(0.0, seconds, n, endpoint=False)
    return signal.chirp(t, f0=20.0, f1=20000.0, t1=seconds, method="logarithmic").astype(np.float32)


def stepped_sines(sr: int, tone_seconds: float = 1.0, gap_seconds: float = 0.08) -> np.ndarray:
    freqs = [80, 110, 160, 220, 330, 500, 700, 1000, 1500, 2000, 3000, 5000, 8000]
    chunks: list[np.ndarray] = []
    gap = np.zeros(int(sr * gap_seconds), dtype=np.float32)
    for f in freqs:
        n = int(sr * tone_seconds)
        t = np.arange(n, dtype=np.float64) / sr
        chunks.append(fade(np.sin(2.0 * np.pi * f * t).astype(np.float32), sr, 8.0))
        chunks.append(gap)
    return np.concatenate(chunks)


def two_tone(sr: int, seconds: float, f1: float, f2: float) -> np.ndarray:
    n = int(sr * seconds)
    t = np.arange(n, dtype=np.float64) / sr
    y = 0.5 * np.sin(2.0 * np.pi * f1 * t) + 0.5 * np.sin(2.0 * np.pi * f2 * t)
    y -= np.mean(y)
    return y.astype(np.float32)


def tri_tone(sr: int, seconds: float, freqs: tuple[float, float, float]) -> np.ndarray:
    n = int(sr * seconds)
    t = np.arange(n, dtype=np.float64) / sr
    y = np.zeros(n, dtype=np.float64)
    for f in freqs:
        y += np.sin(2.0 * np.pi * f * t)
    y /= max(1, len(freqs))
    y -= np.mean(y)
    return y.astype(np.float32)


def write_wav(path: Path, x: np.ndarray, sr: int) -> None:
    peak = float(np.max(np.abs(x)))
    if peak > 0.999:
        x = x / peak * 0.999
    sf.write(path, x.astype(np.float32), sr, subtype="FLOAT")


PRESETS = {
    "full": {
        "seconds": 10.0,
        "gap_seconds": 0.75,
        "levels": "-72,-60,-48,-36,-30,-24,-18,-12,-9,-6",
        "description": "Original long diagnostic batch.",
    },
    "release": {
        "seconds": 7.5,
        "gap_seconds": 0.30,
        "levels": "-72,-60,-48,-36,-30,-24,-18,-12,-9,-6",
        "description": "Release tuning batch: same levels, 25% shorter stimuli and shorter gaps.",
    },
    "quick": {
        "seconds": 5.0,
        "gap_seconds": 0.20,
        "levels": "-60,-48,-36,-30,-24,-18,-12,-9,-6",
        "description": "Fast direction check; omits only the quietest -72 dB level.",
    },
    "iteration": {
        "seconds": 2.5,
        "gap_seconds": 0.08,
        "levels": "-60,-48,-36,-30,-24,-18,-12,-6",
        "description": "Daily guarded iteration batch: same stimulus families, shorter windows/gaps and fewer redundant drive levels.",
    },
    "adaptive": {
        "seconds": 1.2,
        "gap_seconds": 0.04,
        "levels": "-48,-36,-24,-15,-6",
        "description": "Short adaptive batch: fewer static levels plus continuous-level noise sweeps and tri-tone IMD probes.",
    },
}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--sr", type=int, default=48000)
    ap.add_argument("--preset", choices=sorted(PRESETS), default="release")
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--levels", default=None)
    ap.add_argument("--seed", type=int, default=808)
    ap.add_argument("--batch-name", default="overdrive_id_batch.wav")
    ap.add_argument("--gap-seconds", type=float, default=None)
    ap.add_argument("--write-individual", action="store_true", help="Also write individual stimulus files. Off by default.")
    args = ap.parse_args()

    preset = PRESETS[args.preset]
    seconds = float(args.seconds if args.seconds is not None else preset["seconds"])
    gap_seconds = float(args.gap_seconds if args.gap_seconds is not None else preset["gap_seconds"])
    levels_text = args.levels or preset["levels"]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    levels = [float(x.strip()) for x in levels_text.split(",") if x.strip()]
    rng = np.random.default_rng(args.seed)
    gap = np.zeros(int(args.sr * gap_seconds), dtype=np.float32)

    bases = {
        "white": lambda: colored_noise("white", int(args.sr * seconds), rng),
        "pink": lambda: colored_noise("pink", int(args.sr * seconds), rng),
        "brown": lambda: colored_noise("brown", int(args.sr * seconds), rng),
        "multitone": lambda: multitone(args.sr, seconds, rng),
        "sweep": lambda: log_sweep(args.sr, seconds),
        "stepsine": lambda: stepped_sines(args.sr, min(1.0, seconds / 7.5), min(0.08, gap_seconds * 0.35)),
        "twotone_lowmid": lambda: two_tone(args.sr, seconds, 700.0, 1900.0),
        "twotone_himid": lambda: two_tone(args.sr, seconds, 1000.0, 3000.0),
        "tritone_mid": lambda: tri_tone(args.sr, seconds, (700.0, 1500.0, 2600.0)),
        "tritone_himid": lambda: tri_tone(args.sr, seconds, (1000.0, 2200.0, 4100.0)),
    }

    dynamic_noise_bases = {
        "white_sweep_level": "white",
        "pink_sweep_level": "pink",
        "brown_sweep_level": "brown",
    }

    manifest: dict[str, object] = {
        "sample_rate": args.sr,
        "preset": args.preset,
        "preset_description": preset["description"],
        "seconds": seconds,
        "gap_seconds": gap_seconds,
        "levels_dbfs_rms": levels,
        "batch_file": args.batch_name,
        "batch_render_naming": {
            "sat_raw": args.batch_name.replace(".wav", "__sat_raw.wav"),
            "target": args.batch_name.replace(".wav", "__target.wav"),
            "sat_voiced_optional": args.batch_name.replace(".wav", "__sat_voiced.wav")
        },
        "segments": [],
        "individual_files_written": bool(args.write_individual),
    }

    cursor = 0
    batch_chunks: list[np.ndarray] = []
    for base_name, make in bases.items():
        base = fade(make(), args.sr)
        for level in levels:
            stem = f"{base_name}_{int(abs(level)):02d}dB" if level < 0 else f"{base_name}_p{int(level):02d}dB"
            x = set_rms(base, level)
            start = cursor
            end = start + len(x)
            batch_chunks.append(x)
            batch_chunks.append(gap)
            cursor = end + len(gap)

            if args.write_individual:
                write_wav(out / f"{stem}.wav", x, args.sr)

            manifest["segments"].append({
                "stem": stem,
                "kind": base_name,
                "level_dbfs_rms": level,
                "start_sample": int(start),
                "end_sample": int(end),
                "duration_samples": int(len(x)),
            })

    if args.preset == "adaptive":
        sweep_seconds = max(2.4, seconds * 2.0)
        sweep_marks = [-120.0, -96.0, -72.0, -54.0, -42.0, -30.0, -18.0, -9.0, -3.0]
        window = max(1024, int(args.sr * min(0.25, sweep_seconds / 12.0)))
        for base_name, noise_kind in dynamic_noise_bases.items():
            x = swept_colored_noise(noise_kind, args.sr, sweep_seconds, rng, -144.0, 0.0)
            start = cursor
            end = start + len(x)
            batch_chunks.append(x)
            batch_chunks.append(gap)
            cursor = end + len(gap)

            for mark in sweep_marks:
                t = (mark + 144.0) / 144.0
                center = start + int(np.clip(t, 0.0, 1.0) * max(0, len(x) - 1))
                seg_start = max(start, min(end - window, center - window // 2))
                seg_end = min(end, seg_start + window)
                if seg_end <= seg_start:
                    continue
                stem = f"{base_name}_{int(abs(mark)):03d}dB"
                manifest["segments"].append({
                    "stem": stem,
                    "kind": base_name,
                    "level_dbfs_rms": float(mark),
                    "start_sample": int(seg_start),
                    "end_sample": int(seg_end),
                    "duration_samples": int(seg_end - seg_start),
                    "dynamic_level_sweep": True,
                    "sweep_start_db": -144.0,
                    "sweep_end_db": 0.0,
                })

    batch = np.concatenate(batch_chunks) if batch_chunks else np.zeros(1, dtype=np.float32)
    write_wav(out / args.batch_name, batch, args.sr)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print(f"Wrote batch stimulus: {out / args.batch_name}")
    print(f"Segments: {len(manifest['segments'])}")
    print("Render these files in the host, or provide --sat-renderer-exe to auto-render SAT raw/voiced:")
    print(f"  {manifest['batch_render_naming']['sat_raw']}")
    print(f"  {manifest['batch_render_naming']['sat_voiced_optional']}")
    print(f"  {manifest['batch_render_naming']['target']}")


if __name__ == "__main__":
    main()
