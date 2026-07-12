#!/usr/bin/env python3
from __future__ import annotations
import argparse, atexit, csv, json, math, pathlib, shutil, subprocess, sys, time

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

SPECS = {
    "drive_scale": {"mode": "mul", "steps": (1.05, 0.95, 1.10, 0.90), "lo": 0.60, "hi": 1.60},
    "input_gain_db": {"mode": "add", "steps": (0.25, -0.25, 0.50, -0.50), "lo": -6.0, "hi": 6.0},
    "drive_gain_scale": {"mode": "mul", "steps": (1.05, 0.95, 1.10, 0.90), "lo": 0.55, "hi": 1.80},
    "drive_gain_max_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.45, "hi": 2.20},
    "diode_headroom_scale": {"mode": "mul", "steps": (1.04, 0.96, 1.08, 0.92), "lo": 0.70, "hi": 1.35},
    "soft_blend_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.40, "hi": 1.70},
    "clean_amount_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.35, "hi": 1.80},
    "dirty_low_mix_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.35, "hi": 1.80},
    "dirty_tone_offset": {"mode": "add", "steps": (0.015, -0.015, 0.030, -0.030), "lo": -0.18, "hi": 0.18},
    "post_asym_scale": {"mode": "mul", "steps": (1.10, 0.90), "lo": 0.35, "hi": 2.20},
    "clean_freq_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.45, "hi": 2.20},
    "dirty_low_freq_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.45, "hi": 2.20},
    "dirty_freq_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.45, "hi": 2.20},
    "dirty_amount_scale": {"mode": "mul", "steps": (1.08, 0.92, 1.16, 0.84), "lo": 0.45, "hi": 2.20},
}
PROFILES = {
    "fast": ("drive_scale", "input_gain_db", "drive_gain_scale", "diode_headroom_scale", "soft_blend_scale", "clean_amount_scale", "dirty_low_mix_scale", "clean_freq_scale", "dirty_low_freq_scale", "dirty_freq_scale", "dirty_amount_scale"),
    "balanced": tuple(SPECS.keys()),
}

# Control-fit often finds that Klon wants a larger excitation change than
# one-axis +/-0.25 dB nudges can prove. These candidates are still guarded by
# static+Hammer scoring before they can be applied.
OPERATING_POINT_CANDIDATES = (
    ("op_input_p1", {"input_gain_db": ("add", 1.0)}),
    ("op_input_p2", {"input_gain_db": ("add", 2.0)}),
    ("op_input_p3", {"input_gain_db": ("add", 3.0)}),
    ("op_input_p3_drive_m5", {"input_gain_db": ("add", 3.0), "drive_scale": ("mul", 0.95)}),
    ("op_input_p3_drive_m10", {"input_gain_db": ("add", 3.0), "drive_scale": ("mul", 0.90)}),
)
LAYERS = ("pre_a", "pre_ndsp", "pre_b", "post_a", "post_ndsp", "post_b")
PRE_LAYERS = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYERS = ("post_a", "post_ndsp", "post_b")
MAX_KLON_POST_EQ_BANDS = 64
MAX_KLON_PRE_B_BANDS = 2
MAX_KLON_POST_B_BANDS = 24

# Compact Klon PRE search grid. This is intentionally small: it gives the
# joint core+PRE/POST pass real frequency freedom without returning to the old
# huge/residual-only filterbank that caused slow, biased plateaus.
KLON_PRE_STRUCTURAL_SEEDS = (
    {"kind": "LowShelf", "freq_hz": 65.0, "q": 0.70, "gain_db": 1.5, "priority": 2.8, "source_layer": "seed"},
    {"kind": "LowShelf", "freq_hz": 95.0, "q": 0.70, "gain_db": 1.5, "priority": 3.0, "source_layer": "seed"},
    {"kind": "LowShelf", "freq_hz": 129.0, "q": 0.70, "gain_db": 1.5, "priority": 3.2, "source_layer": "seed"},
    {"kind": "LowShelf", "freq_hz": 180.0, "q": 0.70, "gain_db": 1.25, "priority": 2.7, "source_layer": "seed"},
    {"kind": "Peak", "freq_hz": 400.0, "q": 0.80, "gain_db": 1.5, "priority": 2.4, "source_layer": "seed"},
    {"kind": "Peak", "freq_hz": 570.0, "q": 0.85, "gain_db": 1.5, "priority": 2.8, "source_layer": "seed"},
    {"kind": "Peak", "freq_hz": 800.0, "q": 0.85, "gain_db": 1.25, "priority": 2.6, "source_layer": "seed"},
    {"kind": "Peak", "freq_hz": 1250.0, "q": 0.85, "gain_db": 1.25, "priority": 2.9, "source_layer": "seed"},
    {"kind": "Peak", "freq_hz": 1600.0, "q": 0.90, "gain_db": 1.25, "priority": 2.7, "source_layer": "seed"},
    {"kind": "Peak", "freq_hz": 2500.0, "q": 0.90, "gain_db": 1.0, "priority": 2.2, "source_layer": "seed"},
    {"kind": "HighShelf", "freq_hz": 3200.0, "q": 0.70, "gain_db": 1.25, "priority": 2.4, "source_layer": "seed"},
    {"kind": "HighShelf", "freq_hz": 5000.0, "q": 0.70, "gain_db": 1.25, "priority": 2.5, "source_layer": "seed"},
    {"kind": "TiltShelf", "freq_hz": 250.0, "q": 0.70, "gain_db": 1.0, "priority": 2.1, "source_layer": "seed"},
    {"kind": "TiltShelf", "freq_hz": 800.0, "q": 0.70, "gain_db": 1.0, "priority": 2.1, "source_layer": "seed"},
)

def read_json(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))

def write_json(path: pathlib.Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def cleanup_heavy_artifacts(root: pathlib.Path) -> dict:
    deleted_files = 0
    deleted_bytes = 0
    for pattern in ("*.wav", "*.npz"):
        for path in root.rglob(pattern):
            try:
                size = path.stat().st_size
                path.unlink()
            except OSError:
                continue
            deleted_files += 1
            deleted_bytes += size
    return {"deleted_files": deleted_files, "deleted_bytes": deleted_bytes}


def case_files(render_plan: pathlib.Path, case_id: str) -> dict:
    plan = read_json(render_plan)
    for case in plan.get("cases", []):
        if case.get("id") == case_id:
            return dict(case.get("files", {}))
    raise SystemExit(f"case-id {case_id!r} not found in render plan {render_plan}")

def run(cmd: list[str], cwd: pathlib.Path, *, quiet: bool = False) -> None:
    if not quiet:
        print("==>", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=str(cwd), check=True)

def ensure_klon_core(state: dict) -> dict:
    klon = state.setdefault("klon", {})
    core = klon.setdefault("core", {})
    for key, value in KLON_CORE_DEFAULTS.items():
        core.setdefault(key, value)
    return core

def write_cascade_csv(state: dict, out: pathlib.Path) -> None:
    klon = state.get("klon", {})
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["layer", "kind", "freq_hz", "gain_db", "q", "stages", "amount"])
        w.writeheader()
        for layer in LAYERS:
            for band in klon.get(layer, []):
                w.writerow({
                    "layer": layer,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 1000.0)),
                    "gain_db": float(band.get("gain_db", 0.0)),
                    "q": float(band.get("q", 1.0)),
                    "stages": int(float(band.get("stages", 1))),
                    "amount": band.get("amount", "Classic"),
                })

def fit_root(root: pathlib.Path, case_id: str) -> pathlib.Path:
    return root / "overdrive_cases" / case_id / "overdrive_id_fit_voiced"

def static_score(root: pathlib.Path, case_id: str) -> float:
    return float(read_json(fit_root(root, case_id) / "fit_summary.json")["score"])

def hammer_score(root: pathlib.Path, case_id: str) -> float:
    path = fit_root(root, case_id) / "hammerstein_branch_summary.csv"
    rows = []
    if not path.exists():
        return math.inf
    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        if "skipped" in (reader.fieldnames or []):
            return math.inf
        for row in reader:
            vals = []
            for key, value in row.items():
                if key.endswith("_branch_median_delta_db"):
                    try:
                        vals.append(abs(float(value)))
                    except Exception:
                        pass
            if vals:
                rows.append(sum(vals) / len(vals))
    return float(sum(rows) / len(rows)) if rows else math.inf

def copy_target(render_dir: pathlib.Path, source_render_dir: pathlib.Path, target_file: str) -> None:
    render_dir.mkdir(parents=True, exist_ok=True)
    src = source_render_dir / target_file
    dst = render_dir / target_file
    if not src.exists():
        raise SystemExit(f"missing Klon target render: {src}")
    if not dst.exists() or dst.stat().st_size != src.stat().st_size:
        shutil.copy2(src, dst)

def run_suite(args, root: pathlib.Path, state: dict, *, source_render_dir: pathlib.Path, target_file: str, include_hammer: bool = True) -> tuple[float, float]:
    state_path = root / "candidate_voicing_state.json"
    csv_path = root / "candidate_cascade.csv"
    render_dir = root / "renders"
    write_json(state_path, state)
    write_cascade_csv(state, csv_path)
    copy_target(render_dir, source_render_dir, target_file)
    cmd = [sys.executable, "SAT-TR/tools/run_overdrive_analysis_suite.py",
           "--render-plan", args.render_plan,
           "--stim-dir", args.stim_dir,
           "--render-dir", str(render_dir),
           "--out-root", str(root),
           "--sat-renderer-exe", args.renderer,
           "--force-render-sat",
           "--ts-cascade-csv", str(csv_path),
           "--voicing-state", str(state_path),
           "--sat-render-mode", "voiced",
           "--fit-nfft", str(args.fit_nfft),
           "--fit-bands", "48",
           "--fit-layout", "ndsp-foundation-eq",
           "--fit-basis-q", "0.85",
           "--fit-max-gain-db", "12.0",
           "--fit-grid-points", str(args.fit_grid_points),
           "--foundation-prefilter-limit", "48",
           "--foundation-exact-limit", str(args.foundation_exact_limit),
           "--fit-only",
           "--no-fit-plot"]
    if include_hammer:
        cmd.extend([
            "--hammerstein-orders", args.hammerstein_orders,
            "--hammerstein-taps", str(args.hammerstein_taps),
            "--hammerstein-chunk-samples", str(args.hammerstein_chunk_samples),
        ])
    else:
        cmd.append("--skip-hammerstein")
    run(cmd, pathlib.Path.cwd(), quiet=bool(getattr(args, "quiet", False)))
    return static_score(root, args.case_id), hammer_score(root, args.case_id)

def candidate_value(current: float, spec: dict, step: float) -> float:
    if spec["mode"] == "mul":
        return max(spec["lo"], min(spec["hi"], current * step))
    return max(spec["lo"], min(spec["hi"], current + step))


def combined_score(static_score_value: float, hammer_score_value: float, source_static: float, source_hammer: float, hammer_weight: float) -> float:
    if source_static <= 0.0 or source_hammer <= 0.0:
        return math.inf
    if not math.isfinite(static_score_value) or not math.isfinite(hammer_score_value):
        return math.inf
    return (float(static_score_value) / float(source_static)) + float(hammer_weight) * (float(hammer_score_value) / float(source_hammer))


def _band_priority(band: dict) -> tuple[float, float, float]:
    gain = abs(float(band.get("gain_db", 0.0)))
    stages = max(1.0, float(band.get("stages", 1.0)))
    q = max(0.25, float(band.get("q", 1.0)))
    return (gain * stages * (1.0 + 0.08 * min(q, 4.0)), gain, float(band.get("freq_hz", 0.0)))


def _compact_bands(bands: list[dict], max_bands: int) -> tuple[list[dict], int]:
    if max_bands < 0 or len(bands) <= max_bands:
        return list(bands), 0
    if max_bands <= 0:
        return [], len(bands)
    ranked = sorted(enumerate(bands), key=lambda item: _band_priority(item[1]), reverse=True)[:max_bands]
    keep = {idx for idx, _ in ranked}
    return [band for idx, band in enumerate(bands) if idx in keep], len(bands) - max_bands


def compact_state_for_contract(args: argparse.Namespace, state: dict) -> tuple[dict, dict]:
    cand = json.loads(json.dumps(state))
    klon = cand.setdefault("klon", {})
    before = {layer: len(klon.get(layer, [])) for layer in ("pre_b", "post_b")}
    pre_b, removed_pre = _compact_bands(list(klon.get("pre_b", [])), int(args.compact_pre_b_bands))
    post_b, removed_post = _compact_bands(list(klon.get("post_b", [])), int(args.compact_post_b_bands))
    klon["pre_b"] = pre_b
    klon["post_b"] = post_b
    klon["residual_pre"] = [band for layer in PRE_LAYERS for band in klon.get(layer, [])]
    klon["post_residual"] = [band for layer in POST_LAYERS for band in klon.get(layer, [])]
    return cand, {
        "pre_b_before": before["pre_b"],
        "pre_b_after": len(pre_b),
        "post_b_before": before["post_b"],
        "post_b_after": len(post_b),
        "removed_pre_b": removed_pre,
        "removed_post_b": removed_post,
    }


def complexity_penalty(args: argparse.Namespace, state: dict) -> tuple[float, dict]:
    klon = state.get("klon", {})
    layers = (*PRE_LAYERS, *POST_LAYERS)
    bands = [band for layer in layers for band in klon.get(layer, [])]
    band_count = len(bands)
    post_b_count = len(klon.get("post_b", []))
    pre_b_count = len(klon.get("pre_b", []))
    abs_gain = sum(abs(float(b.get("gain_db", 0.0))) * max(1.0, float(b.get("stages", 1.0))) for b in bands)
    excess_bands = max(0, pre_b_count - int(args.compact_pre_b_bands)) + max(0, post_b_count - int(args.compact_post_b_bands))
    penalty = float(args.complexity_band_penalty) * excess_bands + float(args.complexity_gain_penalty) * abs_gain
    return penalty, {"band_count": band_count, "pre_b_count": pre_b_count, "post_b_count": post_b_count, "abs_gain_stage_sum": abs_gain, "excess_bands": excess_bands}

def _parse_deltas(value: str) -> list[float]:
    return [float(x) for x in str(value).split(",") if x.strip()]


def _read_explicit_fit_layers(path: pathlib.Path) -> dict[str, list[dict]]:
    layers = {name: [] for name in POST_LAYERS}
    if not path.exists():
        return layers
    with path.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            layer = str(row.get("layer", "")).strip()
            if layer not in layers:
                continue
            layers[layer].append({
                "kind": row.get("kind") or "Peak",
                "freq_hz": float(row["freq_hz"]),
                "gain_db": float(row["gain_db"]),
                "q": float(row.get("q") or row.get("q_basis") or 1.0),
                "stages": int(float(row.get("stages") or 1)),
                "amount": row.get("amount") or "Classic",
            })
    return layers


def _apply_post_fit_layers(state: dict, root: pathlib.Path, case_id: str) -> tuple[dict | None, dict]:
    fit_csv = fit_root(root, case_id) / "cma_filterbank_fit.csv"
    layers = _read_explicit_fit_layers(fit_csv)
    if not any(layers.values()):
        return None, {"label": "fit_post_replace_missing", "fit_csv": str(fit_csv), "joint_refit": False}
    cand = json.loads(json.dumps(state))
    klon = cand.setdefault("klon", {})
    # Upper-bound mode must never reduce available POST capacity. The fitter can
    # emit a compact replacement with only a couple of residual rows; that is a
    # useful hint, not proof that the existing residual bank should be deleted.
    # Overlay the fitted fixed layers and keep the larger residual bank.
    for layer in ("post_a", "post_ndsp"):
        if layers[layer]:
            klon[layer] = list(layers[layer])
    if len(layers.get("post_b", [])) >= len(klon.get("post_b", [])):
        klon["post_b"] = list(layers["post_b"])
    fixed_post = len(klon.get("post_a", [])) + len(klon.get("post_ndsp", []))
    max_post_b = max(0, MAX_KLON_POST_EQ_BANDS - fixed_post)
    klon["post_b"] = klon.get("post_b", [])[:max_post_b]
    return cand, {
        "label": "fit_post_overlay",
        "fit_csv": str(fit_csv),
        "joint_refit": True,
        "fit_layers": {layer: len(layers[layer]) for layer in POST_LAYERS},
        "materialized_layers": {layer: len(klon.get(layer, [])) for layer in POST_LAYERS},
    }


_PRE_STRUCTURAL_KINDS = {"Peak", "LowShelf", "HighShelf", "TiltShelf"}


def _safe_label(value: object) -> str:
    return str(value).replace("+", "p").replace("-", "m").replace(".", "p").replace(" ", "_")


def _clamp_float(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def _read_fit_rows_for_pre(root: pathlib.Path, case_id: str, max_rows: int) -> list[dict]:
    fit_csv = fit_root(root, case_id) / "cma_filterbank_fit.csv"
    rows: list[dict] = []
    if not fit_csv.exists():
        return rows
    with fit_csv.open("r", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            kind = row.get("kind") or "Peak"
            if kind not in _PRE_STRUCTURAL_KINDS:
                continue
            try:
                gain = float(row["gain_db"])
                freq = float(row["freq_hz"])
                q = float(row.get("q") or row.get("q_basis") or 1.0)
            except Exception:
                continue
            if freq < 40.0 or freq > 18000.0:
                continue
            rows.append({
                "kind": kind,
                "freq_hz": freq,
                "q": _clamp_float(q, 0.5, 3.0),
                "gain_db": gain,
                "stages": int(float(row.get("stages") or 1)),
                "amount": row.get("amount") or "Classic",
                "source_layer": row.get("layer", ""),
                "priority": abs(gain),
            })
    rows.sort(key=lambda r: r["priority"], reverse=True)
    return rows[:int(max_rows)]


def _refit_pre_structural_candidates(args, base_state: dict, root: pathlib.Path, *, source_static: float,
                                     source_render_dir: pathlib.Path, target_file: str,
                                     pass_index: int) -> tuple[float, dict, dict]:
    max_candidates = int(args.coupled_pre_structural_max_candidates)
    if max_candidates == 0:
        return float(source_static), base_state, {"label": "pre_structural_none", "score": float(source_static), "joint_refit": False}

    row_count = int(args.coupled_pre_structural_source_rows)
    source_rows = _read_fit_rows_for_pre(root, args.case_id, row_count)
    seeded_rows = list(KLON_PRE_STRUCTURAL_SEEDS)
    candidate_rows = []
    for row in source_rows:
        r = dict(row)
        # Residual rows are useful hints, but they must not monopolise the
        # structural PRE budget. Cap their priority so Klon-domain seeds always
        # get tested in the same pass.
        residual_priority = abs(float(r.get("gain_db", 0.0)))
        r["priority"] = min(residual_priority, 2.35) + 0.15
        candidate_rows.append(r)
    candidate_rows.extend(seeded_rows)
    if not candidate_rows:
        return float(source_static), base_state, {"label": "pre_structural_no_rows", "score": float(source_static), "joint_refit": False}

    layers = [x.strip() for x in str(args.coupled_pre_structural_layers).split(",") if x.strip()]
    layers = [layer for layer in layers if layer in PRE_LAYERS]
    scales = [float(x) for x in str(args.coupled_pre_structural_gain_scales).split(",") if x.strip()]
    if not layers or not scales:
        return float(source_static), base_state, {"label": "pre_structural_empty_grid", "score": float(source_static), "joint_refit": False}

    jobs = []
    seen: set[tuple] = set()
    layer_priority = {"pre_a": 0, "pre_b": 1, "pre_ndsp": 2}
    band_priority = {0: 0, 1: 1, 2: 2}

    for fit_row in candidate_rows:
        for layer in layers:
            layer_bands = base_state.get("klon", {}).get(layer, [])
            for band_index in range(len(layer_bands)):
                for scale in scales:
                    gain = _clamp_float(float(fit_row["gain_db"]) * scale, -4.0, 4.0)
                    if abs(gain) < 0.05:
                        continue
                    candidate_band = {
                        "kind": fit_row["kind"],
                        "freq_hz": float(fit_row["freq_hz"]),
                        "gain_db": gain,
                        "q": _clamp_float(float(fit_row.get("q", 0.85)), 0.5, 3.0),
                        "stages": max(1, min(2, int(fit_row.get("stages", 1)))),
                        "amount": "Classic",
                    }
                    signature = (layer, band_index, candidate_band["kind"], round(candidate_band["freq_hz"], 3),
                                 round(candidate_band["q"], 4), round(candidate_band["gain_db"], 4), candidate_band["stages"])
                    if signature in seen:
                        continue
                    seen.add(signature)
                    # Score ordering matters because this pass is deliberately budgeted. Prefer
                    # explicit residual hints, then musically plausible Klon shelves/low-mid
                    # shaping, and only then later PRE slots.
                    priority = float(fit_row.get("priority", abs(float(fit_row.get("gain_db", 0.0)))))
                    priority += 0.05 if fit_row.get("source_layer") != "seed" else 0.35
                    priority -= 0.10 * layer_priority.get(layer, 3)
                    priority -= 0.03 * band_priority.get(band_index, 3)
                    jobs.append((priority, layer, band_index, candidate_band, fit_row, scale))

    jobs.sort(key=lambda item: item[0], reverse=True)

    best_score = float(source_static)
    best_state = base_state
    best_row = {"label": "pre_structural_none", "score": float(source_static), "joint_refit": False}

    for tested, (_, layer, band_index, candidate_band, fit_row, scale) in enumerate(jobs[:max_candidates], start=1):
        cand = json.loads(json.dumps(base_state))
        cand["klon"][layer][band_index] = candidate_band
        kind_abbrev = {"LowShelf": "LS", "HighShelf": "HS", "TiltShelf": "TS", "Peak": "PK"}.get(candidate_band["kind"], "EQ")
        layer_abbrev = {"pre_a": "a", "pre_ndsp": "n", "pre_b": "b"}.get(layer, layer[:1])
        gain_tag = f"{candidate_band['gain_db']:+.2f}".replace("+", "p").replace("-", "m").replace(".", "p")
        label = _safe_label(
            f"ps{pass_index}{layer_abbrev}{band_index}{kind_abbrev}{int(round(candidate_band['freq_hz']))}_{gain_tag}"
        )
        cand_root = root / "pre_s" / label
        try:
            score, _ = run_suite(args, cand_root, cand, source_render_dir=source_render_dir,
                                 target_file=target_file, include_hammer=False)
        except subprocess.CalledProcessError as exc:
            write_json(cand_root / "joint_refit_error.json", {"label": label, "returncode": exc.returncode})
            continue
        row = {
            "label": label,
            "score": float(score),
            "joint_refit": True,
            "structural": True,
            "layer": layer,
            "band_index": band_index,
            "band": candidate_band,
            "source_fit_row": fit_row,
            "gain_scale": scale,
            "pass_index": pass_index,
            "tested_rank": tested,
        }
        write_json(cand_root / "joint_refit_result.json", row)
        if score < best_score:
            best_score = float(score)
            best_state = cand
            best_row = row
    return best_score, best_state, best_row


def _refit_layer_candidates(args, base_state: dict, root: pathlib.Path, *, source_static: float,
                            source_render_dir: pathlib.Path, target_file: str, layers: tuple[str, ...],
                            deltas: list[float], max_candidates: int, pass_index: int, label_prefix: str) -> tuple[float, dict, dict]:
    best_score = float(source_static)
    best_state = base_state
    best_row = {"label": f"{label_prefix}_none", "score": float(source_static), "joint_refit": False}
    if not deltas or max_candidates == 0:
        return best_score, best_state, best_row

    tested = 0
    for layer in layers:
        for band_index, band in enumerate(base_state.get("klon", {}).get(layer, [])):
            for delta in deltas:
                if max_candidates > 0 and tested >= max_candidates:
                    return best_score, best_state, best_row
                tested += 1
                cand = json.loads(json.dumps(base_state))
                cand["klon"][layer][band_index]["gain_db"] = float(cand["klon"][layer][band_index].get("gain_db", 0.0)) + delta
                label = (
                    f"{label_prefix}_p{pass_index}_{layer}_{band_index}_{band.get('kind', 'Peak')}_"
                    f"{float(band.get('freq_hz', 0.0)):g}_{delta:+.4g}dB"
                    .replace("+", "p").replace("-", "m").replace(".", "p")
                )
                cand_root = root / ("pre_d" if label_prefix == "pre_refit" else "post_d") / label
                try:
                    score, _ = run_suite(args, cand_root, cand, source_render_dir=source_render_dir, target_file=target_file, include_hammer=False)
                except subprocess.CalledProcessError as exc:
                    write_json(cand_root / "joint_refit_error.json", {"label": label, "returncode": exc.returncode})
                    continue
                row = {
                    "label": label,
                    "score": float(score),
                    "joint_refit": True,
                    "layer": layer,
                    "band_index": band_index,
                    "delta_db": delta,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 0.0)),
                    "pass_index": pass_index,
                }
                write_json(cand_root / "joint_refit_result.json", row)
                if score < best_score:
                    best_score = float(score)
                    best_state = cand
                    best_row = row
    return best_score, best_state, best_row


def joint_refit_candidates(args, base_state: dict, root: pathlib.Path, *, source_static: float,
                           source_render_dir: pathlib.Path, target_file: str) -> tuple[float, dict, dict]:
    """Coordinate refit around a core/Hammer candidate.

    This is deliberately not a cosmetic post-only repair: each pass can move PRE
    first, then POST, so the nonlinear core is judged after the filters that feed
    it have had a chance to adapt. That is the missing step that made good Hammer
    candidates look bad under the old flow.
    """
    pre_deltas = _parse_deltas(args.coupled_pre_deltas)
    post_deltas = _parse_deltas(args.coupled_post_deltas)
    pre_max = int(args.coupled_pre_max_candidates)
    post_max = int(args.coupled_post_max_candidates)
    passes = max(1, int(args.coupled_refit_passes))

    best_score = float(source_static)
    best_state = base_state
    moves: list[dict] = []

    if args.coupled_fit_post_replace:
        structural_state, structural_row = _apply_post_fit_layers(base_state, root, args.case_id)
        if structural_state is not None:
            structural_root = root / "fit_post_replace"
            try:
                structural_score, _ = run_suite(args, structural_root, structural_state,
                                                source_render_dir=source_render_dir,
                                                target_file=target_file, include_hammer=False)
            except subprocess.CalledProcessError as exc:
                structural_row = {**structural_row, "returncode": exc.returncode, "score": float("inf")}
                write_json(structural_root / "joint_refit_error.json", structural_row)
            else:
                structural_row = {**structural_row, "score": float(structural_score)}
                write_json(structural_root / "joint_refit_result.json", structural_row)
                if structural_score < best_score:
                    best_score = float(structural_score)
                    best_state = structural_state
                    moves.append(structural_row)

    for pass_index in range(1, passes + 1):
        improved = False

        structural_score, structural_state, structural_row = _refit_pre_structural_candidates(
            args, best_state, root, source_static=best_score, source_render_dir=source_render_dir,
            target_file=target_file, pass_index=pass_index)
        if structural_score < best_score:
            best_score, best_state = structural_score, structural_state
            moves.append(structural_row)
            improved = True

        pre_score, pre_state, pre_row = _refit_layer_candidates(
            args, best_state, root, source_static=best_score, source_render_dir=source_render_dir,
            target_file=target_file, layers=PRE_LAYERS, deltas=pre_deltas, max_candidates=pre_max,
            pass_index=pass_index, label_prefix="pre_refit")
        if pre_score < best_score:
            best_score, best_state = pre_score, pre_state
            moves.append(pre_row)
            improved = True

        post_score, post_state, post_row = _refit_layer_candidates(
            args, best_state, root, source_static=best_score, source_render_dir=source_render_dir,
            target_file=target_file, layers=POST_LAYERS, deltas=post_deltas, max_candidates=post_max,
            pass_index=pass_index, label_prefix="post_refit")
        if post_score < best_score:
            best_score, best_state = post_score, post_state
            moves.append(post_row)
            improved = True

        if not improved:
            break

    if not moves:
        return best_score, best_state, {"label": "core_only", "score": best_score, "joint_refit": False, "moves": []}
    return best_score, best_state, {"label": "+".join(m["label"] for m in moves), "score": best_score, "joint_refit": True, "moves": moves}




def final_guard_score(args, root: pathlib.Path, state: dict, *, source_render_dir: pathlib.Path, target_file: str) -> tuple[float, float]:
    """Re-score the fully materialized Klon candidate after coupled PRE/POST refit.

    Klon previously exposed a false-positive path: a core candidate could improve
    Hammerstein before refit, then the final materialized state no longer had the
    same Hammer result. This guard makes final static and final Hammer the only
    acceptance scores.

    Long guard runs can occasionally return a non-zero process code after writing
    complete score artifacts. Recover those metrics only when both required files
    exist; otherwise keep the failure hard.
    """
    guard_root = root / "final_guard"
    try:
        return run_suite(args, guard_root, state, source_render_dir=source_render_dir,
                         target_file=target_file, include_hammer=True)
    except subprocess.CalledProcessError:
        fit = fit_root(guard_root, args.case_id)
        if (fit / "fit_summary.json").exists() and (fit / "hammerstein_branch_summary.csv").exists():
            return static_score(guard_root, args.case_id), hammer_score(guard_root, args.case_id)
        raise

def make_core_candidates(state: dict, args) -> list[tuple[str, str, object, object, object, dict]]:
    if int(args.max_candidates) <= 0:
        return []
    core = ensure_klon_core(state)
    only_params = [x.strip() for x in str(args.only_params).split(',') if x.strip()]
    params = only_params or list(PROFILES[args.profile])
    params = [p for p in params if p in SPECS]
    candidates: list[tuple[str, str, object, object, object, dict]] = []

    def can_add() -> bool:
        return len(candidates) < int(args.max_candidates)

    max_steps = max((len(SPECS[p]['steps']) for p in params), default=0)
    for step_index in range(max_steps):
        for key in params:
            spec = SPECS[key]
            if step_index >= len(spec['steps']):
                continue
            current = float(core.get(key, KLON_CORE_DEFAULTS[key]))
            step = float(spec['steps'][step_index])
            value = candidate_value(current, spec, step)
            if abs(value - current) <= 1.0e-9:
                continue
            cand = json.loads(json.dumps(state))
            ensure_klon_core(cand)[key] = value
            label = f"core_hammer_{key}_{str(round(value,8)).replace('-', 'm').replace('.', 'p')}"
            candidates.append((label, key, current, value, step, cand))
            if not can_add():
                return candidates

    pair_budget = int(getattr(args, 'core_pair_candidates', 0))
    pair_count = 0
    if pair_budget > 0:
        first_steps = {key: tuple(float(x) for x in SPECS[key]['steps'][:2]) for key in params}
        for i, left in enumerate(params):
            for right in params[i + 1:]:
                for left_step in first_steps[left]:
                    for right_step in first_steps[right]:
                        cand = json.loads(json.dumps(state))
                        cand_core = ensure_klon_core(cand)
                        old_left = float(core.get(left, KLON_CORE_DEFAULTS[left]))
                        old_right = float(core.get(right, KLON_CORE_DEFAULTS[right]))
                        new_left = candidate_value(old_left, SPECS[left], left_step)
                        new_right = candidate_value(old_right, SPECS[right], right_step)
                        if abs(new_left - old_left) <= 1.0e-9 and abs(new_right - old_right) <= 1.0e-9:
                            continue
                        cand_core[left] = new_left
                        cand_core[right] = new_right
                        label = (
                            f"corepair_{left}_{str(round(new_left,8)).replace('-', 'm').replace('.', 'p')}_"
                            f"{right}_{str(round(new_right,8)).replace('-', 'm').replace('.', 'p')}"
                        )
                        candidates.append((label, f"{left},{right}", {left: old_left, right: old_right},
                                           {left: new_left, right: new_right}, {left: left_step, right: right_step}, cand))
                        pair_count += 1
                        if pair_count >= pair_budget or not can_add():
                            return candidates
    return candidates

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--render-plan", default="SAT-TR/tools/overdrive_id_renders/render_plan_klon.json")
    ap.add_argument("--case-id", default="klon_drive_drv100_in_p0")
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--render-dir", default="SAT-TR/tools/overdrive_id_renders")
    ap.add_argument("--renderer", default="SAT-TR/tools/sat_overdrive_renderer/SatOverdriveRender.exe")
    ap.add_argument("--out-root", default="analysis_out/klon_core_hammer_refine")
    ap.add_argument("--iterations", type=int, default=1)
    ap.add_argument("--max-candidates", type=int, default=24)
    ap.add_argument("--core-pair-candidates", type=int, default=0, help="Add shallow two-parameter Klon core interaction candidates after one-axis screening.")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="fast")
    ap.add_argument("--static-worsening", type=float, default=0.020)
    ap.add_argument("--global-static-tolerance", type=float, default=1.0e-9,
                    help="Do not accept candidates whose final static score regresses beyond the best persisted Klon score.")
    ap.add_argument("--hammer-relative-improvement", type=float, default=0.002)
    ap.add_argument("--hammer-absolute-improvement", type=float, default=0.025)
    ap.add_argument("--hammer-min-improvement", type=float, default=0.002, help="Minimum final Hammer improvement accepted when static score also improves.")
    ap.add_argument("--combined-hammer-weight", type=float, default=0.35, help="Weight of normalized Hammerstein error in the combined promotion score.")
    ap.add_argument("--require-combined-improvement", type=float, default=0.0005, help="Minimum normalized combined-score improvement required for promotion.")
    ap.add_argument("--complexity-band-penalty", type=float, default=0.0025, help="Normalized penalty per residual band beyond compact caps.")
    ap.add_argument("--complexity-gain-penalty", type=float, default=0.00002, help="Normalized penalty per dB-stage of total cascade EQ gain.")
    ap.add_argument("--compact-pre-b-bands", type=int, default=MAX_KLON_PRE_B_BANDS, help="Maximum Klon pre_b bands materialized during scoring.")
    ap.add_argument("--compact-post-b-bands", type=int, default=MAX_KLON_POST_B_BANDS, help="Maximum Klon post_b bands materialized during scoring.")
    ap.add_argument("--compact-static-worsening", type=float, default=0.00002, help="Maximum raw static worsening allowed when regularized score improves via compactness.")
    ap.add_argument("--fit-nfft", type=int, default=1024)
    ap.add_argument("--fit-grid-points", type=int, default=256)
    ap.add_argument("--foundation-exact-limit", type=int, default=8)
    ap.add_argument("--coupled-pre-deltas", default="-0.25,-0.125,0.125,0.25")
    ap.add_argument("--coupled-pre-max-candidates", type=int, default=0)
    ap.add_argument("--coupled-pre-structural-max-candidates", type=int, default=0)
    ap.add_argument("--coupled-pre-structural-source-rows", type=int, default=6)
    ap.add_argument("--coupled-pre-structural-layers", default="pre_a,pre_b")
    ap.add_argument("--coupled-pre-structural-gain-scales", default="0.35,0.7,-0.35,-0.7")
    ap.add_argument("--coupled-post-deltas", default="-0.25,-0.125,0.125,0.25")
    ap.add_argument("--coupled-post-max-candidates", type=int, default=0)
    ap.add_argument("--coupled-refit-passes", type=int, default=1)
    ap.add_argument("--coupled-fit-post-replace", action="store_true", help="Before micro-deltas, test replacing Klon POST layers with the structural cma_filterbank_fit from the core candidate.")
    ap.add_argument("--only-params", default="", help="Comma-separated Klon core params to test; empty tests the selected profile.")
    ap.add_argument("--include-operating-point-candidates", action="store_true", help="Also spend remaining candidate budget on larger input/drive operating-point jumps.")
    ap.add_argument("--hammerstein-orders", default="1,2,3,5")
    ap.add_argument("--hammerstein-taps", type=int, default=64)
    ap.add_argument("--hammerstein-chunk-samples", type=int, default=16384)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--keep-out-root", action="store_true", help="Do not remove stale Klon core-Hammer candidate/source folders before this run.")
    ap.add_argument("--keep-heavy-artifacts", action="store_true", help="Keep regenerable WAV/NPZ artifacts under out-root after the run.")
    ap.add_argument("--quiet", action="store_true", help="Suppress nested command echo to keep long probes readable and avoid console backpressure.")
    args = ap.parse_args()

    cwd = pathlib.Path.cwd()
    state_path = pathlib.Path(args.state)
    source_render_dir = pathlib.Path(args.render_dir)
    state = read_json(state_path)
    files = case_files(pathlib.Path(args.render_plan), args.case_id)
    target_file = files.get("target")
    if not target_file:
        raise SystemExit(f"case-id {args.case_id!r} has no target file in {args.render_plan}")
    ensure_klon_core(state)
    state, source_contract_compaction = compact_state_for_contract(args, state)
    out_root = pathlib.Path(args.out_root)
    if not args.keep_heavy_artifacts:
        atexit.register(lambda: cleanup_heavy_artifacts(out_root))
    out_root.mkdir(parents=True, exist_ok=True)
    if not args.keep_out_root:
        stale_paths = [out_root / "candidates"]
        stale_paths.extend(out_root.glob("iteration_*_source"))
        for stale in stale_paths:
            if stale.exists():
                shutil.rmtree(stale)

    accepted = []
    iteration_reports = []
    for iteration in range(1, args.iterations + 1):
        source_root = out_root / f"iteration_{iteration}_source"
        source_static, source_hammer = run_suite(args, source_root, state, source_render_dir=source_render_dir, target_file=target_file)
        source_combined = combined_score(source_static, source_hammer, source_static, source_hammer, args.combined_hammer_weight)
        source_complexity_penalty, source_complexity = complexity_penalty(args, state)
        source_regularized = source_combined + source_complexity_penalty
        persisted_best_static = state.get("klon", {}).get("last_verified_best_score", None)
        if not isinstance(persisted_best_static, (int, float)) or not math.isfinite(float(persisted_best_static)) or float(persisted_best_static) <= 0.0:
            persisted_best_static = source_static
        persisted_best_static = float(persisted_best_static)
        global_static_allowed = persisted_best_static + float(args.global_static_tolerance)
        print(json.dumps({"iteration": iteration, "source_static": source_static, "source_hammer": source_hammer, "source_combined": source_combined, "source_complexity_penalty": source_complexity_penalty, "source_regularized_score": source_regularized, "source_contract_compaction": source_contract_compaction, "persisted_best_static": persisted_best_static, "global_static_allowed": global_static_allowed}, indent=2), flush=True)
        candidates = make_core_candidates(state, args)
        iteration_rows = []
        iteration_report = {
            "iteration": iteration,
            "source_static": source_static,
            "source_hammer": source_hammer,
            "candidates": iteration_rows,
        }

        best = None
        for label, key, old, new, step, cand in candidates:
            root = out_root / "candidates" / f"iteration_{iteration}_{label}"
            try:
                cand_static, cand_hammer = run_suite(args, root, cand, source_render_dir=source_render_dir, target_file=target_file)
            except subprocess.CalledProcessError as exc:
                error_row = {"label": label, "candidate": label, "error": "run_suite_failed", "returncode": exc.returncode, "param": key, "old": old, "new": new, "step": step}
                print(json.dumps(error_row), flush=True)
                iteration_rows.append(error_row)
                write_json(root / "candidate_error.json", error_row)
                continue
            static_allowed = source_static * (1.0 + args.static_worsening)
            hammer_required = min(source_hammer * (1.0 - args.hammer_relative_improvement),
                                  source_hammer - args.hammer_absolute_improvement)
            refit_score = cand_static
            refit_state = cand
            refit_row = {"label": "core_only", "score": cand_static, "post_refit": False}
            hammer_ok = cand_hammer <= hammer_required
            static_promising = cand_static <= static_allowed
            # Judge promising core candidates after coupled PRE/POST refit. A
            # candidate with excellent static score but slightly worse raw Hammer
            # can still become valid once the feeding/output filters are adapted.
            # The final_guard below remains the only acceptance authority.
            refit_gate = hammer_ok or static_promising
            if refit_gate:
                refit_score, refit_state, refit_row = joint_refit_candidates(
                    args, cand, root, source_static=cand_static,
                    source_render_dir=source_render_dir, target_file=target_file,
                )
            final_static, final_hammer = (cand_static, cand_hammer)
            if refit_gate:
                refit_state, candidate_contract_compaction = compact_state_for_contract(args, refit_state)
                if isinstance(refit_row, dict):
                    refit_row = {**refit_row, "contract_compaction": candidate_contract_compaction}
                try:
                    final_static, final_hammer = final_guard_score(
                        args, root, refit_state, source_render_dir=source_render_dir, target_file=target_file
                    )
                except subprocess.CalledProcessError as exc:
                    row = {
                        "label": label, "param": key, "old": old, "new": new, "step": step,
                        "static_score": cand_static, "joint_refit_score": refit_score,
                        "joint_refit": refit_row, "hammer_score": cand_hammer,
                        "static_allowed": static_allowed, "hammer_required": hammer_required,
                        "error": "final_guard_failed", "returncode": exc.returncode,
                        "eligible": False,
                    }
                    print(json.dumps(row, indent=2), flush=True)
                    write_json(root / "candidate_result.json", row)
                    iteration_rows.append(row)
                    continue
            final_hammer_ok = final_hammer <= hammer_required
            final_hammer_improved = final_hammer <= source_hammer - float(args.hammer_min_improvement)
            final_combined = combined_score(final_static, final_hammer, source_static, source_hammer, args.combined_hammer_weight)
            final_complexity_penalty, final_complexity = complexity_penalty(args, refit_state)
            final_regularized = final_combined + final_complexity_penalty
            regularized_improved = final_regularized <= source_regularized - float(args.require_combined_improvement)
            combined_improved = final_combined <= source_combined - float(args.require_combined_improvement)
            raw_static_ok = final_static <= static_allowed or (regularized_improved and final_static <= source_static + float(args.compact_static_worsening))
            global_static_ok = final_static <= global_static_allowed
            row = {"label": label, "param": key, "old": old, "new": new, "step": step,
                   "static_score": cand_static, "joint_refit_score": refit_score,
                   "joint_refit": refit_row, "hammer_score": cand_hammer,
                   "final_static_score": final_static, "final_hammer_score": final_hammer,
                   "source_combined_score": source_combined, "final_combined_score": final_combined,
                   "final_complexity_penalty": final_complexity_penalty, "final_regularized_score": final_regularized,
                   "final_complexity": final_complexity, "regularized_improved": regularized_improved,
                   "combined_improved": combined_improved, "raw_static_ok": raw_static_ok,
                   "global_static_ok": global_static_ok,
                   "final_hammer_ok": final_hammer_ok, "final_hammer_improved": final_hammer_improved,
                   "refit_gate": refit_gate, "static_promising": static_promising,
                   "static_allowed": static_allowed, "hammer_required": hammer_required,
                   "persisted_best_static": persisted_best_static, "global_static_allowed": global_static_allowed,
                   "eligible": regularized_improved and raw_static_ok and global_static_ok and (final_hammer_ok or final_hammer_improved)}
            print(json.dumps(row, indent=2), flush=True)
            write_json(root / "candidate_result.json", row)
            iteration_rows.append(row)
            if row["eligible"] and (best is None or row["final_regularized_score"] < best["final_regularized_score"]):
                best = {**row, "state": refit_state}
        if best is None:
            iteration_report["accepted"] = False
            iteration_report["reason"] = "no Klon core candidate improved Hammerstein within static guard"
            iteration_report["top_by_static"] = sorted([r for r in iteration_rows if "final_static_score" in r], key=lambda r: float(r["final_static_score"]))[:12]
            iteration_report["top_by_hammer"] = sorted([r for r in iteration_rows if "final_hammer_score" in r], key=lambda r: float(r["final_hammer_score"]))[:12]
            iteration_report["top_by_joint_refit"] = sorted([r for r in iteration_rows if "joint_refit_score" in r], key=lambda r: float(r["joint_refit_score"]))[:12]
            iteration_report["top_by_combined"] = sorted([r for r in iteration_rows if "final_combined_score" in r], key=lambda r: float(r["final_combined_score"]))[:12]
            iteration_report["top_by_regularized"] = sorted([r for r in iteration_rows if "final_regularized_score" in r], key=lambda r: float(r["final_regularized_score"]))[:12]
            iteration_reports.append(iteration_report)
            print(json.dumps({"accepted": False, "reason": iteration_report["reason"], "source_static": source_static, "source_hammer": source_hammer, "top_by_static": iteration_report["top_by_static"][:5], "top_by_hammer": iteration_report["top_by_hammer"][:5], "top_by_combined": iteration_report["top_by_combined"][:5]}, indent=2), flush=True)
            break
        state = best.pop("state")
        state["klon"]["last_core_hammer_candidate"] = best
        state["klon"]["last_verified_best_score"] = float(best.get("final_static_score", best.get("joint_refit_score", best.get("static_score", 0.0))))
        state["klon"]["last_verified_best_hammer_score"] = float(best.get("final_hammer_score", best.get("hammer_score", 0.0)))
        state["klon"]["last_verified_best_candidate"] = best.get("joint_refit", best.get("post_refit", {}))
        state["klon"]["last_core_hammer_updated_at_unix"] = time.time()
        state["klon"]["last_core_hammer_source_static"] = source_static
        state["klon"]["last_core_hammer_source_hammer"] = source_hammer
        accepted.append(best)
        iteration_report["accepted"] = True
        iteration_report["accepted_candidate"] = best
        iteration_report["top_by_static"] = sorted([r for r in iteration_rows if "final_static_score" in r], key=lambda r: float(r["final_static_score"]))[:12]
        iteration_report["top_by_hammer"] = sorted([r for r in iteration_rows if "final_hammer_score" in r], key=lambda r: float(r["final_hammer_score"]))[:12]
        iteration_report["top_by_joint_refit"] = sorted([r for r in iteration_rows if "joint_refit_score" in r], key=lambda r: float(r["joint_refit_score"]))[:12]
        iteration_report["top_by_combined"] = sorted([r for r in iteration_rows if "final_combined_score" in r], key=lambda r: float(r["final_combined_score"]))[:12]
        iteration_reports.append(iteration_report)
        print(json.dumps({"accepted": True, "candidate": best}, indent=2), flush=True)

    all_rows = [row for report in iteration_reports for row in report.get("candidates", [])]
    summary = {
        "accepted_count": len(accepted),
        "accepted": accepted,
        "iterations": iteration_reports,
        "top_by_static": sorted([r for r in all_rows if "final_static_score" in r], key=lambda r: float(r["final_static_score"]))[:20],
        "top_by_hammer": sorted([r for r in all_rows if "final_hammer_score" in r], key=lambda r: float(r["final_hammer_score"]))[:20],
        "top_by_joint_refit": sorted([r for r in all_rows if "joint_refit_score" in r], key=lambda r: float(r["joint_refit_score"]))[:20],
        "top_by_combined": sorted([r for r in all_rows if "final_combined_score" in r], key=lambda r: float(r["final_combined_score"]))[:20],
        "top_by_regularized": sorted([r for r in all_rows if "final_regularized_score" in r], key=lambda r: float(r["final_regularized_score"]))[:20],
    }
    write_json(out_root / "summary.json", summary)
    if args.apply and accepted:
        write_json(state_path, state)
        run([sys.executable, "SAT-TR/tools/write_overdrive_voicing_header.py"], cwd, quiet=bool(args.quiet))
        print("APPLIED Klon core-Hammer candidate(s)")

if __name__ == "__main__":
    main()
