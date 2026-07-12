#!/usr/bin/env python3
"""Run the fixed SAT-TR Overdrive analysis suite.

Use after rendering the current batch files. This wrapper enforces a stable
iteration order so results stay comparable between code changes.

Expected render files:
  overdrive_id_batch__sat_raw.wav
  overdrive_id_batch__sat_voiced.wav
  overdrive_id_batch__target.wav
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import soundfile as sf


def require_file(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"missing required file: {path}")


def audio_frames(path: Path) -> int:
    try:
        return int(sf.info(str(path)).frames)
    except Exception as exc:
        raise SystemExit(f"cannot read audio info for {path}: {exc}") from exc


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    try:
        with path.open("rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except FileNotFoundError:
        return "missing"


def stable_hash(payload: dict) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def normalize_external_target_length(path: Path, expected_frames: int, tolerance_frames: int) -> bool:
    """Trim/pad tiny host-export length drift on external target renders.

    FL Studio can append/remove a handful of samples depending on PDC/tail export
    settings. SAT renders stay strict; only the external reference target is
    normalised, and only inside a small tolerance so a wrong batch still fails.
    """
    frames = audio_frames(path)
    delta = frames - expected_frames
    if delta == 0:
        return False
    if abs(delta) > max(0, int(tolerance_frames)):
        raise SystemExit(
            f"invalid render length for target {path}: got {frames} frames, "
            f"expected {expected_frames}. Difference {delta:+d} exceeds auto-normalise "
            f"tolerance of {tolerance_frames} frames. Re-render this file from the same stimulus batch before fitting."
        )

    data, sr = sf.read(str(path), dtype="float32", always_2d=True)
    if data.shape[0] != frames:
        raise SystemExit(f"cannot normalise target length for {path}: frame count changed while reading")

    backup = path.with_suffix(path.suffix + ".prelength.bak")
    if not backup.exists():
        shutil.copy2(path, backup)

    if frames > expected_frames:
        data = data[:expected_frames, :]
        action = f"trimmed {frames - expected_frames} trailing frames"
    else:
        pad = np.zeros((expected_frames - frames, data.shape[1]), dtype=data.dtype)
        data = np.vstack([data, pad])
        action = f"padded {expected_frames - frames} trailing frames"

    sf.write(str(path), data, sr, subtype="FLOAT")
    print(f"Normalised target length: {path} ({action}; backup: {backup})")
    return True


def run(cmd: list[str]) -> None:
    print("\n==>", " ".join(cmd))
    start = time.perf_counter()
    subprocess.run(cmd, check=True)
    elapsed = time.perf_counter() - start
    print(f"<== done in {elapsed:.2f}s")


def remove_if_exists(path: Path) -> None:
    try:
        if path.exists():
            path.unlink()
    except Exception as exc:
        print(f"WARNING: cannot remove stale render file {path}: {exc}")


def remove_tree_if_exists(path: Path) -> None:
    try:
        if path.exists():
            shutil.rmtree(path)
    except Exception as exc:
        raise SystemExit(f"cannot remove stale analysis directory {path}: {exc}") from exc


def run_renderer(cmd: list[str]) -> None:
    print("\n==>", " ".join(cmd))
    start = time.perf_counter()
    completed = subprocess.run(cmd, text=True, capture_output=True)
    elapsed = time.perf_counter() - start
    if completed.stdout:
        print(completed.stdout, end="" if completed.stdout.endswith("\n") else "\n")
    if completed.stderr:
        print(completed.stderr, end="" if completed.stderr.endswith("\n") else "\n", file=sys.stderr)
    print(f"<== done in {elapsed:.2f}s")
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, cmd, output=completed.stdout, stderr=completed.stderr)


def active_voicing_metadata(state_path: Path | None = None, pedal: str = "ts808") -> dict:
    state_path = state_path or Path("SAT-TR/tools/overdrive_voicing_state.json")
    if not state_path.exists():
        return {}
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
        voicing = state.get("klon" if pedal == "klon" else "ts808", {})
    except Exception:
        return {}
    default_core = "klon_split_clean_dirty_core" if pedal == "klon" else "ts808_feedback_diode_core"
    return {
        "sat_voicing_pedal": pedal,
        "sat_voicing_variant": voicing.get("active_variant", "unknown"),
        "sat_base_voicing_enabled": bool(voicing.get("base_voicing_enabled", True)),
        "sat_post_hicut_enabled": bool(voicing.get("post_hicut_enabled", True)),
        "sat_residual_matching_enabled": bool(voicing.get("residual_matching_enabled", False)),
        "sat_voicing_cascade": voicing.get("voicing_cascade_contract", {
            "pre_order": ["residual_pre"],
            "core": default_core,
            "post_order": ["post_residual"],
        }),
        "sat_voicing_layer_counts": {
            "pre_a": len(voicing.get("pre_a", [])),
            "pre_ndsp": len(voicing.get("pre_ndsp", [])),
            "pre_b": len(voicing.get("pre_b", [])),
            "post_a": len(voicing.get("post_a", [])),
            "post_ndsp": len(voicing.get("post_ndsp", [])),
            "post_b": len(voicing.get("post_b", [])),
        },
    }


def active_voicing_core_args(state_path: Path | None = None, pedal: str = "ts808") -> list[str]:
    state_path = state_path or Path("SAT-TR/tools/overdrive_voicing_state.json")
    if not state_path.exists():
        return []
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
        node = state.get("klon" if pedal == "klon" else "ts808", {})
        core = node.get("core", {})
    except Exception:
        return []

    if pedal == "klon":
        klon_defaults = {
            "drive_scale": 1.0,
            "input_gain_db": 0.0,
            "drive_gain_scale": 1.0,
            "drive_gain_max_scale": 1.0,
            "diode_headroom_scale": 1.0,
            "soft_blend_scale": 1.0,
            "clean_amount_scale": 1.0,
            "dirty_low_mix_scale": 1.0,
            "clean_freq_scale": 1.0,
            "dirty_low_freq_scale": 1.0,
            "dirty_freq_scale": 1.0,
            "dirty_amount_scale": 1.0,
            "dirty_tone_offset": 0.0,
            "post_asym_scale": 1.0,
        }
        core = {**klon_defaults, **core}
        mapping = {
            "drive_scale": "--klon-drive-scale",
            "input_gain_db": "--klon-input-gain-db",
            "drive_gain_scale": "--klon-drive-gain-scale",
            "drive_gain_max_scale": "--klon-drive-gain-max-scale",
            "diode_headroom_scale": "--klon-diode-headroom-scale",
            "soft_blend_scale": "--klon-soft-blend-scale",
            "clean_amount_scale": "--klon-clean-amount-scale",
            "dirty_low_mix_scale": "--klon-dirty-low-mix-scale",
            "clean_freq_scale": "--klon-clean-freq-scale",
            "dirty_low_freq_scale": "--klon-dirty-low-freq-scale",
            "dirty_freq_scale": "--klon-dirty-freq-scale",
            "dirty_amount_scale": "--klon-dirty-amount-scale",
            "dirty_tone_offset": "--klon-dirty-tone-offset",
            "post_asym_scale": "--klon-post-asym-scale",
        }
    else:
        mapping = {
            "drive_scale": "--ts-drive-scale",
            "drive_headroom": "--ts-drive-headroom",
            "input_gain_db": "--ts-input-gain-db",
            "loop_drive_max": "--ts-loop-drive-max",
            "loop_capped_gain_at_max_drive": "--ts-loop-capped-gain",
            "air_gain_at_max_drive": "--ts-air-gain",
            "solver_knee_start": "--ts-solver-knee-start",
            "solver_pre_conduct": "--ts-solver-pre-conduct",
            "upper_mid_split_hz": "--ts-upper-mid-split",
            "upper_blend_lo": "--ts-upper-blend-lo",
            "upper_blend_hi": "--ts-upper-blend-hi",
            "upper_air_trim_lo": "--ts-upper-air-trim-lo",
            "upper_air_trim_hi": "--ts-upper-air-trim-hi",
            "body_feedback_hi": "--ts-body-feedback-hi",
            "body_hardness_hi": "--ts-body-hardness-hi",
            "body_asymmetry_lo": "--ts-body-asymmetry-lo",
            "body_asymmetry_hi": "--ts-body-asymmetry-hi",
            "upper_feedback_hi": "--ts-upper-feedback-hi",
            "upper_hardness_hi": "--ts-upper-hardness-hi",
            "upper_asymmetry_scale": "--ts-upper-asymmetry-scale",
            "upper_dynamic_feedback": "--ts-upper-dynamic-feedback",
            "upper_mid_dynamic_feedback": "--ts-upper-mid-dynamic-feedback",
            "upper_air_dynamic_feedback": "--ts-upper-air-dynamic-feedback",
        }
    out: list[str] = []
    for key, arg in mapping.items():
        if key in core:
            out.extend([arg, str(float(core[key]))])
    return out


def voicing_state_has_cascade_layers(state_path: Path, pedal: str) -> bool:
    """Return true when the state carries explicit pre/post voicing layers.

    SatOverdriveRender can receive core parameters from --voicing-state via
    active_voicing_core_args(), but pre/post layers only reach the renderer via
    --ts-cascade-csv unless the executable was rebuilt from the exact same
    generated header. Candidate runs must therefore pass the CSV explicitly.
    """
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
        node = state.get("klon" if pedal == "klon" else "ts808", {})
    except Exception:
        return False

    for layer in ("pre_a", "pre_ndsp", "pre_b", "post_a", "post_ndsp", "post_b"):
        if node.get(layer):
            return True
    return False


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stim-dir", default="SAT-TR/tools/overdrive_id_stimuli")
    ap.add_argument("--render-dir", default="SAT-TR/tools/overdrive_id_renders")
    ap.add_argument("--out-root", default="SAT-TR/tools")
    ap.add_argument("--cma-iters", type=int, default=260)
    ap.add_argument("--cma-popsize", type=int, default=72)
    ap.add_argument("--skip-cma", action="store_true",
                    help="Compatibility alias: skip static residual fitting.")
    ap.add_argument("--use-cma-static-eq", action="store_true",
                    help="Use slow CMA for the static residual EQ fit. Default is fast least-squares.")
    ap.add_argument("--render-plan", default=None,
                    help="Optional JSON plan generated by write_overdrive_render_plan.py. Runs the suite once per case.")
    ap.add_argument("--case-id", action="append", default=[],
                    help="Run only this render-plan case id. Can be passed more than once.")
    ap.add_argument("--skip-missing", action="store_true",
                    help="With --render-plan, skip cases whose WAV files are not present instead of aborting.")
    ap.add_argument("--sat-renderer-exe", default=None,
                    help="Optional SAT offline renderer executable. Generates missing sat_raw/sat_voiced files from the dry batch.")
    ap.add_argument("--force-render-sat", action="store_true",
                    help="With --sat-renderer-exe, regenerate sat_raw/sat_voiced even when files already exist.")
    ap.add_argument("--disable-sat-render-cache", action="store_true",
                    help="Disable exact-signature SAT render reuse. By default, --force-render-sat still reuses a render when stimulus, renderer, cascade and voicing state match exactly.")
    ap.add_argument("--sat-render-mode", choices=["both", "voiced", "raw"], default="both",
                    help="Which SAT renders are required. Fit-only candidate scoring normally needs only voiced.")
    ap.add_argument("--target-length-tolerance", type=int, default=2048,
                    help="Auto trim/pad external target renders when length drift is within this many frames. SAT renders remain strict.")
    ap.add_argument("--ts-cascade-csv", default=None,
                    help="Optional TS808 cascade CSV passed to SatOverdriveRender for pre/post candidate rendering.")
    ap.add_argument("--voicing-state", default=None,
                    help="Optional voicing JSON used for core args and metadata. Candidate pre/post layers still require --ts-cascade-csv.")
    ap.add_argument("--allow-compiled-cascade-with-voicing-state", action="store_true",
                    help="Allow --voicing-state without --ts-cascade-csv, relying on the cascade embedded in the renderer executable. Use only for current compiled-state checks, not candidate validation.")
    ap.add_argument("--fit-only", action="store_true",
                    help="Skip auxiliary batch/matched analyses and run only the residual fit used for scoring.")
    ap.add_argument("--fit-nfft", type=int, default=1024,
                    help="FFT size for residual fit. Default is fast for broad tone fitting; use 8192+ for audit.")
    ap.add_argument("--fit-bands", type=int, default=48,
                    help="Residual band count passed to the fit script when --fit-layout peak is used.")
    ap.add_argument("--fit-layout", choices=["tone", "ndsp-band-eq", "ndsp-foundation-eq", "peak"], default="ndsp-foundation-eq",
                    help="ndsp-foundation-eq is the current contract: one learned 3EQ foundation before the fixed NDSP grid. tone/peak are legacy diagnostics.")
    ap.add_argument("--fit-basis-q", type=float, default=0.85,
                    help="Residual EQ Q for peak layout. Wider defaults avoid high-Q sawtooth overfitting.")
    ap.add_argument("--fit-max-gain-db", type=float, default=12.0,
                    help="Max absolute gain per residual band.")
    ap.add_argument("--fit-grid-points", type=int, default=256,
                    help="Log-grid points used by residual fit/scoring. Iteration default is 256; use 512 for slow audit.")
    ap.add_argument("--foundation-prefilter-limit", type=int, default=48,
                    help="Cheap linear shortlist size for foundation-band fitting. Iteration default is 48; use 96 for slow audit.")
    ap.add_argument("--foundation-exact-limit", type=int, default=12,
                    help="Number of linearly ranked foundation candidates that receive nonlinear least-squares refit. 0 refits the full shortlist.")
    ap.add_argument("--skip-hammerstein", action="store_true",
                    help="Skip Hammerstein branch diagnostics in the residual fit.")
    ap.add_argument("--hammerstein-orders", default="1,2,3,5,7",
                    help="Comma-separated Hammerstein orders used when diagnostics are enabled.")
    ap.add_argument("--hammerstein-taps", type=int, default=128,
                    help="Hammerstein FIR taps used when diagnostics are enabled.")
    ap.add_argument("--hammerstein-chunk-samples", type=int, default=8192,
                    help="Rows per Hammerstein normal-equation chunk.")
    ap.add_argument("--feature-cache-dir", default=None,
                    help="Shared cache directory for target-side fit features. If omitted, uses <out-root>/_feature_cache.")
    ap.add_argument("--no-fit-plot", action="store_true",
                    help="Skip fit PNG plot generation.")
    args = ap.parse_args()

    stim_dir = Path(args.stim_dir)
    render_dir = Path(args.render_dir)
    out_root = Path(args.out_root)

    manifest_path = stim_dir / "manifest.json"
    require_file(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    names = manifest["batch_render_naming"]

    require_file(stim_dir / manifest["batch_file"])

    py = sys.executable

    def render_missing_sat_files(case_meta: dict, files: dict) -> None:
        if not args.sat_renderer_exe:
            return
        renderer = Path(args.sat_renderer_exe)
        require_file(renderer)
        dry_path = stim_dir / manifest["batch_file"]
        render_dir.mkdir(parents=True, exist_ok=True)
        sat_settings = case_meta.get("sat_settings", {}) if isinstance(case_meta, dict) else {}
        drive = float(sat_settings.get("drive", 1.0))
        type_value = float(sat_settings.get("type", sat_settings.get("type_value", 0.0)))
        knee = float(sat_settings.get("knee", 0.0))
        input_db = float(sat_settings.get("host_or_plugin_input_db", sat_settings.get("input_db", 0.0)))
        output_db = float(sat_settings.get("output_db", 0.0))
        model_name = str(sat_settings.get("engine_model", sat_settings.get("model_key", ""))).lower()
        if not model_name:
            model_name = "klon" if type_value > 0.5 else "ts808"
        common = [
            str(renderer),
            "--in", str(dry_path),
            "--model", model_name,
            "--drive", str(drive),
            "--type", str(type_value),
            "--knee", str(knee),
            "--input-db", str(input_db),
            "--output-db", str(output_db),
        ]
        core_args = active_voicing_core_args(Path(args.voicing_state) if args.voicing_state else None, pedal=model_name)
        common.extend(core_args)
        if args.ts_cascade_csv:
            common.extend(["--ts-cascade-csv", str(Path(args.ts_cascade_csv))])
        expected_frames = audio_frames(dry_path)
        render_signature_base = {
            "version": 1,
            "renderer": str(renderer),
            "renderer_sha256": file_sha256(renderer),
            "dry_path": str(dry_path),
            "dry_sha256": file_sha256(dry_path),
            "model": model_name,
            "drive": drive,
            "type": type_value,
            "knee": knee,
            "input_db": input_db,
            "output_db": output_db,
            "core_args": core_args,
            "cascade_csv": str(Path(args.ts_cascade_csv)) if args.ts_cascade_csv else "",
            "cascade_sha256": file_sha256(Path(args.ts_cascade_csv)) if args.ts_cascade_csv else "",
            "voicing_state": str(Path(args.voicing_state)) if args.voicing_state else "",
            "voicing_sha256": file_sha256(Path(args.voicing_state)) if args.voicing_state else "",
            "expected_frames": expected_frames,
        }
        render_roles = []
        if args.sat_render_mode in ("both", "raw"):
            render_roles.append(("sat_raw", "1"))
        if args.sat_render_mode in ("both", "voiced"):
            render_roles.append(("sat_voiced", "0"))

        for key, raw in render_roles:
            target = render_dir / files[key]
            meta_path = target.with_suffix(target.suffix + ".render.json")
            signature_payload = {**render_signature_base, "role": key, "raw": raw}
            signature = stable_hash(signature_payload)
            cache_hit = False
            if not args.disable_sat_render_cache and target.exists() and meta_path.exists():
                try:
                    meta = json.loads(meta_path.read_text(encoding="utf-8"))
                    cache_hit = meta.get("signature") == signature and audio_frames(target) == expected_frames
                except Exception:
                    cache_hit = False
            if cache_hit:
                print(json.dumps({
                    "sat_render_cache_hit": True,
                    "role": key,
                    "path": str(target),
                    "signature": signature,
                }, sort_keys=True))
                continue

            needs_render = args.force_render_sat or not target.exists()
            if target.exists() and not needs_render:
                existing_frames = audio_frames(target)
                needs_render = existing_frames != expected_frames
                if needs_render:
                    print(f"Existing {target} has {existing_frames} frames; expected {expected_frames}. Regenerating.")
            if not needs_render:
                continue

            last_frames = -1
            last_error = ""
            max_attempts = 3
            tmp_path = target.with_suffix(target.suffix + ".tmp")
            for attempt in range(1, max_attempts + 1):
                remove_if_exists(target)
                remove_if_exists(tmp_path)
                remove_if_exists(meta_path)
                try:
                    run_renderer(common + ["--raw", raw, "--out", str(target)])
                except subprocess.CalledProcessError as exc:
                    size = target.stat().st_size if target.exists() else 0
                    stderr_tail = (exc.stderr or "").strip().splitlines()[-3:]
                    stdout_tail = (exc.output or "").strip().splitlines()[-3:]
                    diagnostic = " | ".join(stderr_tail or stdout_tail)
                    last_error = f"renderer exited with {exc.returncode}; partial_size={size}"
                    if diagnostic:
                        last_error += f"; diagnostic={diagnostic}"
                    print(f"Render attempt {attempt}/{max_attempts} failed for {target}: {last_error}")
                    remove_if_exists(target)
                    remove_if_exists(tmp_path)
                    continue
                if not target.exists():
                    last_error = "renderer did not create output file"
                    print(f"Render attempt {attempt}/{max_attempts} failed for {target}: {last_error}")
                    remove_if_exists(tmp_path)
                    continue
                size = target.stat().st_size
                try:
                    last_frames = audio_frames(target)
                except SystemExit as exc:
                    last_error = f"cannot read rendered WAV info; bytes={size}; {exc}"
                    print(f"Render attempt {attempt}/{max_attempts} failed for {target}: {last_error}")
                    remove_if_exists(target)
                    remove_if_exists(tmp_path)
                    continue
                if last_frames == expected_frames:
                    break
                last_error = f"got {last_frames} frames, expected {expected_frames}; bytes={size}"
                print(f"Invalid render length for {target} on attempt {attempt}/{max_attempts}: {last_error}")
            if last_frames != expected_frames:
                remove_if_exists(target)
                remove_if_exists(tmp_path)
                remove_if_exists(meta_path)
                raise SystemExit(
                    f"render failed for {target} after {max_attempts} attempts: {last_error or 'unknown renderer failure'}. "
                    "No partial WAV was kept. This usually means the offline renderer or filesystem produced a partial write."
                )
            meta_path.write_text(json.dumps({
                "signature": signature,
                "role": key,
                "raw": raw,
                "path": str(target),
                "frames": expected_frames,
                "written_at_unix": time.time(),
                "signature_payload": signature_payload,
            }, indent=2, sort_keys=True), encoding="utf-8")

    def run_case(case: dict | None) -> Path:
        if case is None:
            case_id = "default"
            files = {
                "sat_raw": names["sat_raw"],
                "sat_voiced": names.get("sat_voiced_optional", manifest["batch_file"].replace(".wav", "__sat_voiced.wav")),
                "target": names["target"],
            }
            case_meta = {"id": case_id, "mode": "default three-file workflow"}
            pedal = str(case_meta.get("sat_settings", {}).get("engine_model", plan.get("pedal", "ts808"))).lower()
            if args.voicing_state and not args.ts_cascade_csv and not args.allow_compiled_cascade_with_voicing_state:
                state_path = Path(args.voicing_state)
                if voicing_state_has_cascade_layers(state_path, pedal):
                    raise SystemExit(
                        "Refusing candidate render: --voicing-state contains pre/post cascade layers, "
                        "but --ts-cascade-csv was not provided. Pass the matching cascade CSV, or use "
                        "--allow-compiled-cascade-with-voicing-state only when intentionally relying on the renderer's compiled header."
                    )
            case_meta.update(active_voicing_metadata(Path(args.voicing_state) if args.voicing_state else None, pedal=pedal))
            if args.ts_cascade_csv:
                case_meta["sat_ts_cascade_csv"] = str(args.ts_cascade_csv)
            case_root = out_root
        else:
            case_id = case["id"]
            files = case["files"]
            case_meta = dict(case)
            pedal = str(case_meta.get("sat_settings", {}).get("engine_model", plan.get("pedal", "ts808"))).lower()
            if args.voicing_state and not args.ts_cascade_csv and not args.allow_compiled_cascade_with_voicing_state:
                state_path = Path(args.voicing_state)
                if voicing_state_has_cascade_layers(state_path, pedal):
                    raise SystemExit(
                        "Refusing candidate render: --voicing-state contains pre/post cascade layers, "
                        "but --ts-cascade-csv was not provided. Pass the matching cascade CSV, or use "
                        "--allow-compiled-cascade-with-voicing-state only when intentionally relying on the renderer's compiled header."
                    )
            case_meta.update(active_voicing_metadata(Path(args.voicing_state) if args.voicing_state else None, pedal=pedal))
            if args.ts_cascade_csv:
                case_meta["sat_ts_cascade_csv"] = str(args.ts_cascade_csv)
            case_root = out_root / "overdrive_cases" / case_id

        render_missing_sat_files(case_meta, files)
        expected_frames = audio_frames(stim_dir / manifest["batch_file"])
        required_roles = ["target"]
        if args.sat_render_mode in ("both", "raw"):
            required_roles.append("sat_raw")
        if args.sat_render_mode in ("both", "voiced"):
            required_roles.append("sat_voiced")
        for role in required_roles:
            path = render_dir / files[role]
            require_file(path)
            frames = audio_frames(path)
            if frames != expected_frames:
                if role == "target":
                    normalize_external_target_length(path, expected_frames, args.target_length_tolerance)
                    frames = audio_frames(path)
                if frames != expected_frames:
                    raise SystemExit(
                        f"invalid render length for {role} {path}: got {frames} frames, "
                        f"expected {expected_frames}. Re-render this file from the same stimulus batch before fitting."
                    )
        meta_json = json.dumps(case_meta, separators=(",", ":"))

        if not args.fit_only:
            run([
                py, "SAT-TR/tools/analyze_overdrive_batch.py",
                "--stim-dir", str(stim_dir),
                "--render-dir", str(render_dir),
                "--sat-raw-file", files["sat_raw"],
                "--sat-voiced-file", files["sat_voiced"],
                "--target-file", files["target"],
                "--case-meta", meta_json,
                "--out", str(case_root / "overdrive_id_analysis"),
            ])

            run([
                py, "SAT-TR/tools/analyze_overdrive_matched.py",
                "--stim-dir", str(stim_dir),
                "--render-dir", str(render_dir),
                "--sat-file", files["sat_voiced"],
                "--target-file", files["target"],
                "--case-meta", meta_json,
                "--out", str(case_root / "overdrive_id_analysis_matched_global_rms"),
                "--sat-render", "sat_voiced",
                "--level-match", "global-rms",
            ])

            run([
                py, "SAT-TR/tools/analyze_overdrive_matched.py",
                "--stim-dir", str(stim_dir),
                "--render-dir", str(render_dir),
                "--sat-file", files["sat_voiced"],
                "--target-file", files["target"],
                "--case-meta", meta_json,
                "--out", str(case_root / "overdrive_id_analysis_matched_segment_rms"),
                "--sat-render", "sat_voiced",
                "--level-match", "segment-rms",
            ])



        if not args.skip_cma:
            # Fit outputs are decision artifacts, not reusable cache. Always
            # materialise them from the current scorer/render/cascade so stale
            # fit_summary.json files cannot promote obsolete candidates. Target
            # feature caches remain valid and are handled by the fit script.
            remove_tree_if_exists(case_root / "overdrive_id_fit_voiced")
            run([
                py, "SAT-TR/tools/fit_overdrive_hammerstein_cma.py",
                "--stim-dir", str(stim_dir),
                "--render-dir", str(render_dir),
                "--sat-file", files["sat_voiced"],
                "--target-file", files["target"],
                "--case-meta", meta_json,
                "--out", str(case_root / "overdrive_id_fit_voiced"),
                "--sat-render", "sat_voiced",
                "--bands", str(args.fit_bands),
                "--fit-layout", args.fit_layout,
                "--orders", str(args.hammerstein_orders),
                "--hammerstein-taps", str(args.hammerstein_taps),
                "--hammerstein-chunk-samples", str(args.hammerstein_chunk_samples),
                "--basis-q", str(args.fit_basis_q),
                "--max-gain-db", str(args.fit_max_gain_db),
                "--nfft", str(args.fit_nfft),
                "--fit-grid-points", str(args.fit_grid_points),
                "--foundation-prefilter-limit", str(args.foundation_prefilter_limit),
                "--foundation-exact-limit", str(args.foundation_exact_limit),
                "--feature-cache-dir", str(Path(args.feature_cache_dir) if args.feature_cache_dir else Path(args.out_root) / "_feature_cache"),
                "--kind-filter", "pink,brown,white,multitone,sweep,stepsine,twotone_lowmid,twotone_himid,tritone_mid,tritone_himid,white_sweep_level,pink_sweep_level,brown_sweep_level",
                "--optimizer", "cma" if args.use_cma_static_eq else "least-squares",
                "--cma-iters", str(args.cma_iters),
                "--popsize", str(args.cma_popsize),
            ] + (["--skip-hammerstein"] if args.skip_hammerstein else [])
              + (["--no-plot"] if args.no_fit_plot else []))

        return case_root

    completed_roots = []
    if args.render_plan:
        plan_path = Path(args.render_plan)
        require_file(plan_path)
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        selected_case_ids = set(args.case_id)
        selected_cases = [
            case for case in plan["cases"]
            if not selected_case_ids or case["id"] in selected_case_ids
        ]
        if selected_case_ids:
            found_case_ids = {case["id"] for case in selected_cases}
            missing_case_ids = sorted(selected_case_ids - found_case_ids)
            if missing_case_ids:
                raise SystemExit("unknown case id(s): " + ", ".join(missing_case_ids))
        for case in selected_cases:
            try:
                completed_roots.append(run_case(case))
            except SystemExit as exc:
                if not args.skip_missing:
                    raise
                print(f"Skipping {case['id']}: {exc}")
    else:
        completed_roots.append(run_case(None))

    print("\nAnalysis suite complete.")
    print("Primary roots:")
    for root in completed_roots:
        print(f"  {root}")


if __name__ == "__main__":
    main()
