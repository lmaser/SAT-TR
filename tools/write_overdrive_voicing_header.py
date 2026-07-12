#!/usr/bin/env python3
"""Generate SAT-TR Overdrive voicing constants from JSON.

Currently this owns only the TS808/type-0 Overdrive data. Analysis/CMA tooling
can update the JSON; the plugin consumes a small compile-time header with no
runtime parser cost.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

from overdrive_voicing_contract import DEFAULT_CONTRACT

ROOT = Path(__file__).resolve().parents[1]
STATE_PATH = ROOT / "tools" / "overdrive_voicing_state.json"
HEADER_PATH = ROOT / "Source" / "OverdriveVoicingData.h"

CORE_FIELDS = [
    "drive_scale",
    "drive_headroom",
    "input_gain_db",
    "solver_knee_start",
    "solver_pre_conduct",
    "input_high_pass_hz",
    "feedback_pole_min_hz",
    "upper_mid_split_hz",
    "low_trim_at_max_drive",
    "loop_drive_max",
    "loop_capped_gain_at_max_drive",
    "air_gain_at_max_drive",
    "loop_coeff_hz_lo",
    "loop_coeff_hz_hi",
    "body_limit_lo",
    "body_limit_hi",
    "body_feedback_lo",
    "body_feedback_hi",
    "body_hardness_lo",
    "body_hardness_hi",
    "body_asymmetry_lo",
    "body_asymmetry_hi",
    "upper_limit_lo",
    "upper_limit_hi",
    "upper_feedback_lo",
    "upper_feedback_hi",
    "upper_hardness_lo",
    "upper_hardness_hi",
    "upper_asymmetry_scale",
    "upper_dynamic_feedback",
    "upper_mid_dynamic_feedback",
    "upper_air_dynamic_feedback",
    "upper_blend_lo",
    "upper_blend_hi",
    "upper_air_trim_lo",
    "upper_air_trim_hi",
    "legacy_knee_range_lo",
    "legacy_knee_range_hi",
]

CPP_FIELD_NAMES = {
    "drive_scale": "driveScale",
    "drive_headroom": "driveHeadroom",
    "input_gain_db": "inputGainDb",
    "solver_knee_start": "solverKneeStart",
    "solver_pre_conduct": "solverPreConduct",
    "input_high_pass_hz": "inputHighPassHz",
    "feedback_pole_min_hz": "feedbackPoleMinHz",
    "upper_mid_split_hz": "upperMidSplitHz",
    "low_trim_at_max_drive": "lowTrimAtMaxDrive",
    "loop_drive_max": "loopDriveMax",
    "loop_capped_gain_at_max_drive": "loopCappedGainAtMaxDrive",
    "air_gain_at_max_drive": "airGainAtMaxDrive",
    "loop_coeff_hz_lo": "loopCoeffHzLo",
    "loop_coeff_hz_hi": "loopCoeffHzHi",
    "body_limit_lo": "bodyLimitLo",
    "body_limit_hi": "bodyLimitHi",
    "body_feedback_lo": "bodyFeedbackLo",
    "body_feedback_hi": "bodyFeedbackHi",
    "body_hardness_lo": "bodyHardnessLo",
    "body_hardness_hi": "bodyHardnessHi",
    "body_asymmetry_lo": "bodyAsymmetryLo",
    "body_asymmetry_hi": "bodyAsymmetryHi",
    "upper_limit_lo": "upperLimitLo",
    "upper_limit_hi": "upperLimitHi",
    "upper_feedback_lo": "upperFeedbackLo",
    "upper_feedback_hi": "upperFeedbackHi",
    "upper_hardness_lo": "upperHardnessLo",
    "upper_hardness_hi": "upperHardnessHi",
    "upper_asymmetry_scale": "upperAsymmetryScale",
    "upper_dynamic_feedback": "upperDynamicFeedback",
    "upper_mid_dynamic_feedback": "upperMidDynamicFeedback",
    "upper_air_dynamic_feedback": "upperAirDynamicFeedback",
    "upper_blend_lo": "upperBlendLo",
    "upper_blend_hi": "upperBlendHi",
    "upper_air_trim_lo": "upperAirTrimLo",
    "upper_air_trim_hi": "upperAirTrimHi",
    "legacy_knee_range_lo": "legacyKneeRangeLo",
    "legacy_knee_range_hi": "legacyKneeRangeHi",
}

KLON_CORE_DEFAULTS = {
    "drive_scale": 1.0,
    "input_gain_db": 0.0,
    "drive_gain_scale": 1.0,
    "drive_gain_max_scale": 1.0,
    "diode_headroom_scale": 1.0,
    "soft_blend_scale": 1.0,
    "clean_amount_scale": 1.0,
    "dirty_low_mix_scale": 1.0,
    "dirty_tone_offset": 0.0,
    "post_asym_scale": 1.0,
    "clean_freq_scale": 1.0,
    "dirty_low_freq_scale": 1.0,
    "dirty_freq_scale": 1.0,
    "dirty_amount_scale": 1.0,
}

KLON_CORE_FIELDS = list(KLON_CORE_DEFAULTS.keys())
KLON_CPP_FIELD_NAMES = {
    "drive_scale": "driveScale",
    "input_gain_db": "inputGainDb",
    "drive_gain_scale": "driveGainScale",
    "drive_gain_max_scale": "driveGainMaxScale",
    "diode_headroom_scale": "diodeHeadroomScale",
    "soft_blend_scale": "softBlendScale",
    "clean_amount_scale": "cleanAmountScale",
    "dirty_low_mix_scale": "dirtyLowMixScale",
    "dirty_tone_offset": "dirtyToneOffset",
    "post_asym_scale": "postAsymScale",
    "clean_freq_scale": "cleanFreqScale",
    "dirty_low_freq_scale": "dirtyLowFreqScale",
    "dirty_freq_scale": "dirtyFreqScale",
    "dirty_amount_scale": "dirtyAmountScale",
}

VALID_KINDS = {"Peak", "LowShelf", "HighShelf", "LowPass", "HighPass", "TiltShelf"}
VALID_AMOUNTS = {"Fixed", "Reference", "Classic", "ClassicDrive"}
MAX_KLON_EQ_STAGES = 4
MAX_CLASSIC_PRE_EQ_BANDS = 32
TS808_RESERVED_PRE_BANDS = 2
MAX_TS808_RESIDUAL_PRE_BANDS = MAX_CLASSIC_PRE_EQ_BANDS - TS808_RESERVED_PRE_BANDS
MAX_CLASSIC_POST_EQ_BANDS = 64
MAX_KLON_PRE_EQ_BANDS = 64
MAX_KLON_POST_EQ_BANDS = 64
PRE_LAYER_NAMES = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYER_NAMES = ("post_a", "post_ndsp", "post_b")
KLON_LAYER_NAMES = (*PRE_LAYER_NAMES, *POST_LAYER_NAMES)


def ensure_overdrive_b_layers(klon: dict) -> None:
    """Klon/Overdrive B uses the same cascade contract as TS808, but owns its data."""
    for name in KLON_LAYER_NAMES:
        klon.setdefault(name, [])
    core = klon.setdefault("core", {})
    for key, value in KLON_CORE_DEFAULTS.items():
        core.setdefault(key, value)
    klon["residual_pre"] = [band for name in PRE_LAYER_NAMES for band in klon.get(name, [])]
    klon["post_residual"] = [band for name in POST_LAYER_NAMES for band in klon.get(name, [])]


def validate_klon_layout(klon: dict) -> None:
    ensure_overdrive_b_layers(klon)
    contract = DEFAULT_CONTRACT["klon"]
    if len(klon.get("pre_b", [])) > contract["pre_b"]:
        raise ValueError(f"Klon pre_b has {len(klon.get('pre_b', []))} bands; compact contract allows {contract['pre_b']}")
    if len(klon.get("post_b", [])) > contract["post_b"]:
        raise ValueError(f"Klon post_b has {len(klon.get('post_b', []))} bands; compact contract allows {contract['post_b']}")
    pre_total = len(klon["residual_pre"])
    post_total = len(klon["post_residual"])
    if pre_total > MAX_KLON_PRE_EQ_BANDS:
        raise ValueError(f"Klon pre EQ uses {pre_total} bands; SAT-TR supports {MAX_KLON_PRE_EQ_BANDS}")
    if post_total > MAX_KLON_POST_EQ_BANDS:
        raise ValueError(f"Klon post EQ uses {post_total} bands; SAT-TR supports {MAX_KLON_POST_EQ_BANDS}")
    for name in KLON_LAYER_NAMES:
        for i, band in enumerate(klon[name]):
            validate_band(band, label=f"klon.{name}[{i}]")


def f32(value: float) -> str:
    text = f"{float(value):.8g}"
    if "e" not in text and "." not in text:
        text += ".0"
    return text + "f"


def validate_band(band: dict, *, label: str) -> None:
    kind = band["kind"]
    amount = band["amount"]
    if kind not in VALID_KINDS:
        raise ValueError(f"Unsupported EQ kind in {label}: {kind}")
    if amount not in VALID_AMOUNTS:
        raise ValueError(f"Unsupported EQ amount in {label}: {amount}")

    stages = int(band["stages"])
    if stages < 1 or stages > MAX_KLON_EQ_STAGES:
        raise ValueError(f"{label} uses {stages} stages; SAT-TR supports 1..{MAX_KLON_EQ_STAGES}")
    if kind == "TiltShelf" and stages > MAX_KLON_EQ_STAGES // 2:
        raise ValueError(f"{label} TiltShelf uses {stages} logical stages; SAT-TR supports 1..{MAX_KLON_EQ_STAGES // 2}")

    float(band["freq_hz"])
    float(band["q"])
    float(band["gain_db"])


def ensure_cascade_layers(ts: dict) -> None:
    """Keep old state files readable while making the actual contract explicit."""
    if not all(name in ts for name in PRE_LAYER_NAMES):
        legacy_pre = list(ts.get("residual_pre", []))
        ts.setdefault("pre_a", legacy_pre[:3])
        ts.setdefault("pre_ndsp", [])
        ts.setdefault("pre_b", [])

    if not all(name in ts for name in POST_LAYER_NAMES):
        legacy_post = list(ts.get("post_residual", []))
        ts.setdefault("post_a", legacy_post[:3])
        ts.setdefault("post_ndsp", legacy_post[3:])
        ts.setdefault("post_b", [])

    # Current TS808 tuning contract:
    # pre_a -> pre_ndsp -> pre_b -> core -> post_a -> post_ndsp -> post_b.
    # Do not silently truncate here; analysis and runtime must share the same
    # compact state. Use compact_overdrive_voicing.py before generating.
    ts["residual_pre"] = [band for name in PRE_LAYER_NAMES for band in ts.get(name, [])]
    ts["post_residual"] = [band for name in POST_LAYER_NAMES for band in ts.get(name, [])]


def validate_ts808_layout(ts: dict) -> None:
    ensure_cascade_layers(ts)
    contract = DEFAULT_CONTRACT["ts808"]
    if len(ts.get("pre_b", [])) > contract["pre_b"]:
        raise ValueError(f"TS808 pre_b has {len(ts.get('pre_b', []))} bands; compact contract allows {contract['pre_b']}")
    if len(ts.get("post_b", [])) > contract["post_b"]:
        raise ValueError(f"TS808 post_b has {len(ts.get('post_b', []))} bands; compact contract allows {contract['post_b']}")
    residual_pre = ts["residual_pre"]
    if len(residual_pre) > MAX_TS808_RESIDUAL_PRE_BANDS:
        raise ValueError(
            f"TS808 residual_pre has {len(residual_pre)} bands; SAT-TR has {MAX_TS808_RESIDUAL_PRE_BANDS} free pre bands"
        )

    post_total = len(ts["post_core"]) + len(ts["post_residual"])
    if bool(ts.get("post_hicut_enabled", True)):
        post_total += 1
    if post_total > MAX_CLASSIC_POST_EQ_BANDS:
        raise ValueError(f"TS808 post EQ uses {post_total} bands; SAT-TR supports {MAX_CLASSIC_POST_EQ_BANDS}")

    for name in (*PRE_LAYER_NAMES, "post_core", *POST_LAYER_NAMES):
        for i, band in enumerate(ts[name]):
            validate_band(band, label=f"{name}[{i}]")
    validate_band(ts["post_hicut"], label="post_hicut")


def band_line(band: dict) -> str:
    validate_band(band, label="band")
    kind = band["kind"]
    amount = band["amount"]
    return (
        "    { KlonEqKind::" + kind + ", "
        + f32(band["freq_hz"]) + ", "
        + f32(band["q"]) + ", "
        + f32(band["gain_db"]) + ", "
        + str(int(band["stages"])) + ", KlonEqAmount::" + amount + " },"
    )


def array_block(name: str, bands: list[dict]) -> str:
    # MSVC is happier with at least one element; neutral 0 dB bands are DSP identity.
    emitted = bands or [{
        "kind": "Peak",
        "freq_hz": 1000.0,
        "q": 1.0,
        "gain_db": 0.0,
        "stages": 1,
        "amount": "Fixed",
    }]
    lines = [f"static constexpr KlonEqBandSpec {name}[] =", "{"]
    lines.extend(band_line(band) for band in emitted)
    lines.append("};")
    return "\n".join(lines)


def generate(state: dict) -> str:
    ts = state["ts808"]
    ensure_cascade_layers(ts)
    validate_ts808_layout(ts)
    klon = state.get("klon", {})
    validate_klon_layout(klon)
    core = ts["core"]
    core.setdefault("drive_headroom", 1.0)
    missing = [field for field in CORE_FIELDS if field not in core]
    if missing:
        raise ValueError("Missing TS808 core fields: " + ", ".join(missing))

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/write_overdrive_voicing_header.py.",
        "// Edit tools/overdrive_voicing_state.json, then regenerate this file.",
        "",
        "namespace OverdriveVoicing",
        "{",
        "struct Ts808CoreTuning",
        "{",
    ]
    lines.extend(f"    float {CPP_FIELD_NAMES[field]};" for field in CORE_FIELDS)
    residual_enabled = bool(ts.get("residual_matching_enabled", False))
    base_voicing_enabled = bool(ts.get("base_voicing_enabled", True))
    post_hicut_enabled = bool(ts.get("post_hicut_enabled", True))

    lines.extend([
        "};",
        "",
        "static constexpr bool kTs808ResidualMatchingEnabled = "
        + ("true" if residual_enabled else "false") + ";",
        "static constexpr bool kTs808BaseVoicingEnabled = "
        + ("true" if base_voicing_enabled else "false") + ";",
        "static constexpr bool kTs808PostHiCutEnabled = "
        + ("true" if post_hicut_enabled else "false") + ";",
        "",
        "static constexpr Ts808CoreTuning kTs808Core =",
        "{",
    ])
    for i, field in enumerate(CORE_FIELDS):
        comma = "," if i < len(CORE_FIELDS) - 1 else ""
        lines.append(f"    {f32(core[field])}{comma}")
    lines.extend(["};", ""])

    klon_core = klon.setdefault("core", {})
    for field, value in KLON_CORE_DEFAULTS.items():
        klon_core.setdefault(field, value)
    lines.extend([
        "struct KlonCoreTuning",
        "{",
    ])
    lines.extend(f"    float {KLON_CPP_FIELD_NAMES[field]};" for field in KLON_CORE_FIELDS)
    lines.extend([
        "};",
        "",
        "static constexpr KlonCoreTuning kKlonCore =",
        "{",
    ])
    for i, field in enumerate(KLON_CORE_FIELDS):
        comma = "," if i < len(KLON_CORE_FIELDS) - 1 else ""
        lines.append(f"    {f32(klon_core[field])}{comma}")
    lines.extend(["};", ""])

    lines.append(array_block("kTs808PreA", ts["pre_a"]))
    lines.append("")
    lines.append(array_block("kTs808PreNdsp", ts["pre_ndsp"]))
    lines.append("")
    lines.append(array_block("kTs808PreB", ts["pre_b"]))
    lines.append("")
    lines.append(array_block("kTs808PostCore", ts["post_core"]))
    lines.append("")
    lines.append(array_block("kTs808PostA", ts["post_a"]))
    lines.append("")
    lines.append(array_block("kTs808PostNdsp", ts["post_ndsp"]))
    lines.append("")
    lines.append(array_block("kTs808PostB", ts["post_b"]))
    lines.append("")
    lines.extend([
        "static constexpr KlonEqBandSpec kTs808PostHiCut =",
        "    " + band_line(ts["post_hicut"]).strip().rstrip(",") + ";",
        "",
        "static constexpr bool kKlonResidualMatchingEnabled = "
        + ("true" if bool(klon.get("residual_matching_enabled", False)) else "false") + ";",
        "",
    ])
    lines.append(array_block("kKlonPreA", klon["pre_a"]))
    lines.append("")
    lines.append(array_block("kKlonPreNdsp", klon["pre_ndsp"]))
    lines.append("")
    lines.append(array_block("kKlonPreB", klon["pre_b"]))
    lines.append("")
    lines.append(array_block("kKlonPostA", klon["post_a"]))
    lines.append("")
    lines.append(array_block("kKlonPostNdsp", klon["post_ndsp"]))
    lines.append("")
    lines.append(array_block("kKlonPostB", klon["post_b"]))
    lines.append("")
    lines.extend([
        "} // namespace OverdriveVoicing",
        "",
    ])
    return "\n".join(lines)


def write_text_atomic(path: Path, text: str) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8", newline="\n")
    os.replace(tmp, path)


def main() -> None:
    state = json.loads(STATE_PATH.read_text(encoding="utf-8"))
    write_text_atomic(HEADER_PATH, generate(state))


if __name__ == "__main__":
    main()
