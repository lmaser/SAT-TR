#!/usr/bin/env python3
"""Numerical design probe for SAT-TR's proposed editable waveshaper."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "sat-waveshape-probe-results.json"


def soft_curve(x: np.ndarray) -> np.ndarray:
    return np.tanh(2.5 * x) / np.tanh(2.5)


def hard_curve(x: np.ndarray) -> np.ndarray:
    return np.tanh(12.0 * x) / np.tanh(12.0)


def fold_curve(x: np.ndarray) -> np.ndarray:
    return np.clip(0.82 * np.sin(0.78 * np.pi * x) + 0.18 * x, -1.0, 1.0)


def asymmetric_curve(x: np.ndarray) -> np.ndarray:
    shifted = np.tanh(3.0 * (x + 0.18))
    zero = np.tanh(3.0 * 0.18)
    positive_scale = np.tanh(3.0 * 1.18) - zero
    negative_scale = zero - np.tanh(3.0 * -0.82)
    return np.where(x >= 0.0,
                    (shifted - zero) / positive_scale,
                    (shifted - zero) / negative_scale)


def make_lut(function, size: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    axis = np.linspace(-1.0, 1.0, size)
    values = function(axis)
    integral = np.zeros_like(values)
    integral[1:] = np.cumsum(0.5 * (values[1:] + values[:-1]) * np.diff(axis))
    return axis, values, integral


def evaluate(axis: np.ndarray, values: np.ndarray, x: np.ndarray) -> np.ndarray:
    return np.interp(x, axis, values)


def evaluate_integral(axis: np.ndarray, values: np.ndarray, integral: np.ndarray,
                      x: np.ndarray) -> np.ndarray:
    result = np.interp(x, axis, integral)
    below = x < axis[0]
    above = x > axis[-1]
    result[below] = integral[0] + values[0] * (x[below] - axis[0])
    result[above] = integral[-1] + values[-1] * (x[above] - axis[-1])
    return result


def process_adaa(axis: np.ndarray, values: np.ndarray, integral: np.ndarray,
                 signal: np.ndarray) -> np.ndarray:
    previous = np.roll(signal, 1)
    delta = signal - previous
    current_integral = evaluate_integral(axis, values, integral, signal)
    previous_integral = evaluate_integral(axis, values, integral, previous)
    midpoint = 0.5 * (signal + previous)
    direct = evaluate(axis, values, midpoint)
    return np.where(np.abs(delta) > 1.0e-5,
                    (current_integral - previous_integral) / delta, direct)


def alias_ratio_db(signal: np.ndarray, sample_rate: int, fundamental: int) -> float:
    spectrum = np.fft.rfft(signal)
    power = np.abs(spectrum) ** 2
    valid = np.zeros(power.size, dtype=bool)
    valid[0] = True
    harmonic = 1
    while harmonic * fundamental <= sample_rate // 2:
        if harmonic % 2 == 1:
            bin_index = harmonic * fundamental * len(signal) // sample_rate
            valid[max(0, bin_index - 1):min(power.size, bin_index + 2)] = True
        harmonic += 1
    alias = float(np.sum(power[~valid]))
    total = float(np.sum(power)) + 1.0e-30
    return 10.0 * np.log10(max(alias / total, 1.0e-30))


def antialias_probe() -> dict[str, float]:
    sample_rate = 48000
    seconds = 3
    fundamental = 997
    time = np.arange(sample_rate * seconds) / sample_rate
    source = 1.15 * np.sin(2.0 * np.pi * fundamental * time)
    axis, values, integral = make_lut(hard_curve, 2049)
    results: dict[str, float] = {}
    for factor in (1, 2, 4, 8, 16):
        up = source if factor == 1 else resample_poly(source, factor, 1)
        processed = evaluate(axis, values, up)
        down = processed if factor == 1 else resample_poly(processed, 1, factor)
        centre = down[sample_rate:2 * sample_rate]
        results[f"direct_x{factor}_alias_db"] = alias_ratio_db(
            centre, sample_rate, fundamental)
        up_axis, up_values, up_integral = make_lut(hard_curve, 2049)
        processed_adaa = process_adaa(up_axis, up_values, up_integral, up)
        down_adaa = processed_adaa if factor == 1 else resample_poly(processed_adaa, 1, factor)
        results[f"adaa_x{factor}_alias_db"] = alias_ratio_db(
            down_adaa[sample_rate:2 * sample_rate], sample_rate, fundamental)
    return results


def lut_probe() -> list[dict[str, float | int | str]]:
    dense = np.linspace(-1.0, 1.0, 1_000_001)
    rows = []
    for name, function in (("soft", soft_curve), ("hard", hard_curve), ("fold", fold_curve)):
        reference = function(dense)
        for size in (257, 1024, 1025, 2049, 4097):
            axis, values, _ = make_lut(function, size)
            error = evaluate(axis, values, dense) - reference
            rows.append({
                "curve": name,
                "size": size,
                "max_error": float(np.max(np.abs(error))),
                "rms_error": float(np.sqrt(np.mean(error * error))),
                "contains_exact_zero": bool(size % 2 == 1),
                "memory_per_curve_bytes": int(size * 2 * 4),
            })
    return rows


def spectral_morph_probe() -> dict[str, float]:
    axis = np.linspace(-1.0, 1.0, 4097)
    first = soft_curve(axis)
    second = asymmetric_curve(axis)
    amount = 0.5
    direct = (1.0 - amount) * first + amount * second

    spectrum_a = np.fft.rfft(first)
    spectrum_b = np.fft.rfft(second)
    magnitude = (1.0 - amount) * np.abs(spectrum_a) + amount * np.abs(spectrum_b)
    phase_a = np.angle(spectrum_a)
    phase_delta = np.angle(np.exp(1j * (np.angle(spectrum_b) - phase_a)))
    spectral = np.fft.irfft(magnitude * np.exp(1j * (phase_a + amount * phase_delta)), n=axis.size)

    direct_second_difference = np.diff(direct, n=2)
    spectral_second_difference = np.diff(spectral, n=2)
    return {
        "direct_peak": float(np.max(np.abs(direct))),
        "spectral_peak": float(np.max(np.abs(spectral))),
        "direct_zero_error": float(abs(direct[axis.size // 2])),
        "spectral_zero_error": float(abs(spectral[axis.size // 2])),
        "direct_odd_symmetry_error": float(np.max(np.abs(direct + direct[::-1]))),
        "spectral_odd_symmetry_error": float(np.max(np.abs(spectral + spectral[::-1]))),
        "direct_roughness_rms": float(np.sqrt(np.mean(direct_second_difference ** 2))),
        "spectral_roughness_rms": float(np.sqrt(np.mean(spectral_second_difference ** 2))),
        "complex_spectral_interpolation_equals_direct_max_error": float(np.max(np.abs(
            np.fft.irfft((1.0 - amount) * spectrum_a + amount * spectrum_b, n=axis.size)
            - direct))),
    }


def bipolar_mirror_probe() -> dict[str, float | int]:
    positive_axis = np.linspace(0.0, 1.0, 2049)
    original = np.clip(1.55 * positive_axis - 0.55 * positive_axis ** 3,
                       0.0, 1.0)
    bipolar_axis = np.linspace(-1.0, 1.0, 4097)
    bipolar = np.where(
        bipolar_axis >= 0.0,
        np.interp(bipolar_axis, positive_axis, original),
        -np.interp(-bipolar_axis, positive_axis, original),
    )
    restored = bipolar[bipolar_axis >= 0.0]
    return {
        "source_point_count": int(original.size),
        "bipolar_point_count": int(bipolar.size),
        "origin_error": float(abs(bipolar[bipolar.size // 2])),
        "mirror_xy_max_error": float(np.max(np.abs(bipolar + bipolar[::-1]))),
        "unipolar_restore_max_error": float(np.max(np.abs(restored - original))),
    }


def main() -> int:
    result = {
        "lut": lut_probe(),
        "antialias": antialias_probe(),
        "morph": spectral_morph_probe(),
        "bipolar_mirror": bipolar_mirror_probe(),
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    print(f"Wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
