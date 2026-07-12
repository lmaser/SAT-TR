#!/usr/bin/env python3
from __future__ import annotations
import argparse, csv, json, pathlib, shutil, subprocess, sys, time

LAYERS = ("pre_a", "pre_ndsp", "pre_b", "post_a", "post_ndsp", "post_b")
POST_LAYERS = ("post_a", "post_ndsp", "post_b")

def read_json(p: pathlib.Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))

def write_json(p: pathlib.Path, d: dict) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")

def case_files(render_plan: pathlib.Path, case_id: str) -> dict:
    plan = read_json(render_plan)
    for case in plan.get("cases", []):
        if case.get("id") == case_id:
            return dict(case.get("files", {}))
    raise SystemExit(f"case-id {case_id!r} not found in render plan {render_plan}")

def write_csv(state: dict, pedal: str, out: pathlib.Path) -> None:
    node = state[pedal]
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["layer", "kind", "freq_hz", "gain_db", "q", "stages", "amount"])
        w.writeheader()
        for layer in LAYERS:
            for band in node.get(layer, []):
                w.writerow({
                    "layer": layer,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 1000.0)),
                    "gain_db": float(band.get("gain_db", 0.0)),
                    "q": float(band.get("q", 1.0)),
                    "stages": int(float(band.get("stages", 1))),
                    "amount": band.get("amount", "Classic"),
                })

def fit_summary_path(root: pathlib.Path, case_id: str) -> pathlib.Path:
    return root / "overdrive_cases" / case_id / "overdrive_id_fit_voiced" / "fit_summary.json"

def fit_score(root: pathlib.Path, case_id: str) -> float:
    return float(read_json(fit_summary_path(root, case_id))["score"])

def write_running_summary(out_root: pathlib.Path, source_score: float, best: dict, results: list[dict]) -> None:
    summary = {
        "source_score": source_score,
        "best_score": best["score"],
        "best_label": best["label"],
        "complete": False,
        "results": sorted(results, key=lambda r: r.get("score", 1e9)),
    }
    write_json(out_root / "summary.json", summary)

def previous_verified_score(state: dict) -> float | None:
    try:
        value = state.get("klon", {}).get("last_verified_best_score")
        if value is None:
            return None
        return float(value)
    except (TypeError, ValueError):
        return None

def parse_band_filter(value: str) -> set[tuple[str, int]]:
    allowed: set[tuple[str, int]] = set()
    for item in str(value or "").split(","):
        item = item.strip()
        if not item:
            continue
        if ":" not in item:
            raise SystemExit(f"invalid --band-filter item {item!r}; expected layer:index")
        layer, index = item.split(":", 1)
        layer = layer.strip()
        if layer not in POST_LAYERS:
            raise SystemExit(f"invalid --band-filter layer {layer!r}; expected one of {', '.join(POST_LAYERS)}")
        allowed.add((layer, int(index.strip())))
    return allowed

def parse_layer_filter(value: str) -> set[str]:
    allowed = {item.strip() for item in str(value or "").split(",") if item.strip()}
    invalid = allowed.difference(POST_LAYERS)
    if invalid:
        raise SystemExit(f"invalid --layer-filter layer(s): {', '.join(sorted(invalid))}")
    return allowed

def ensure_target(render_dir: pathlib.Path, source_render_dir: pathlib.Path, target_file: str) -> None:
    render_dir.mkdir(parents=True, exist_ok=True)
    target = source_render_dir / target_file
    dest = render_dir / target_file
    if not target.exists():
        raise SystemExit(f"missing source target {target}")
    if not dest.exists() or dest.stat().st_size != target.stat().st_size:
        shutil.copy2(target, dest)


def run(cmd: list[str], cwd: pathlib.Path) -> None:
    print("==>", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=str(cwd), check=True)

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--render-plan", default="SAT-TR/tools/overdrive_id_renders/render_plan_klon.json")
    ap.add_argument("--case-id", default="klon_drive_drv100_in_p0")
    ap.add_argument("--render-dir", default="SAT-TR/tools/overdrive_id_renders")
    ap.add_argument("--out-root", default="analysis_out/klon_post_local_refine")
    ap.add_argument("--renderer", default="SAT-TR/tools/sat_overdrive_renderer/SatOverdriveRender.exe")
    ap.add_argument("--deltas", default="-1.0,-0.5,0.5,1.0")
    ap.add_argument("--layer-filter", default="", help="Optional comma list of post layers to test, e.g. post_ndsp,post_a.")
    ap.add_argument("--band-filter", default="", help="Optional comma list of exact layer:index bands to test, e.g. post_ndsp:1,post_ndsp:0.")
    ap.add_argument("--max-candidates", type=int, default=0, help="Maximum candidates to evaluate after filters. 0 means all.")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    cwd = pathlib.Path.cwd()
    state_path = pathlib.Path(args.state)
    base_state = read_json(state_path)
    if "klon" not in base_state:
        raise SystemExit("state has no klon section")
    deltas = [float(x) for x in args.deltas.split(",") if x.strip()]
    out_root = pathlib.Path(args.out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    source_root = out_root / "source"
    source_render_dir = pathlib.Path(args.render_dir)
    files = case_files(pathlib.Path(args.render_plan), args.case_id)
    target_file = files.get("target")
    if not target_file:
        raise SystemExit(f"case-id {args.case_id!r} has no target file in {args.render_plan}")
    source_csv = source_root / "source_cascade.csv"
    source_state = source_root / "source_voicing_state.json"
    write_csv(base_state, "klon", source_csv)
    write_json(source_state, base_state)
    run([sys.executable, "SAT-TR/tools/run_overdrive_analysis_suite.py",
         "--render-plan", args.render_plan,
         "--render-dir", args.render_dir,
         "--out-root", str(source_root),
         "--sat-renderer-exe", args.renderer,
         "--force-render-sat",
         "--ts-cascade-csv", str(source_csv),
         "--voicing-state", str(source_state),
         "--sat-render-mode", "voiced",
         "--fit-only", "--skip-hammerstein", "--no-fit-plot"], cwd)
    source_score = fit_score(source_root, args.case_id)
    previous_best = previous_verified_score(base_state)
    acceptance_reference = source_score if previous_best is None else min(source_score, previous_best)
    best = {"score": source_score, "label": "source", "state": base_state}
    results = []
    write_running_summary(out_root, source_score, best, results)

    layer_filter = parse_layer_filter(args.layer_filter)
    band_filter = parse_band_filter(args.band_filter)
    candidate_specs = []
    for layer in POST_LAYERS:
        if layer_filter and layer not in layer_filter:
            continue
        for i, band in enumerate(base_state["klon"].get(layer, [])):
            if band_filter and (layer, i) not in band_filter:
                continue
            for delta in deltas:
                candidate_specs.append((layer, i, band, delta))
    if args.max_candidates > 0:
        candidate_specs = candidate_specs[:args.max_candidates]

    print(json.dumps({
        "klon_post_local_candidates": len(candidate_specs),
        "layer_filter": sorted(layer_filter),
        "band_filter": [f"{layer}:{index}" for layer, index in sorted(band_filter)],
        "deltas": deltas,
    }, indent=2), flush=True)

    for layer, i, band, delta in candidate_specs:
        cand = json.loads(json.dumps(base_state))
        cand["klon"][layer][i]["gain_db"] = float(cand["klon"][layer][i].get("gain_db", 0.0)) + delta
        label = f"{layer}_{i}_{band.get('kind','Peak')}_{int(round(float(band.get('freq_hz',0))))}_{delta:+.2f}dB".replace("+", "p").replace("-", "m").replace(".", "p")
        root = out_root / "candidates" / label
        csv_path = root / "candidate_cascade.csv"
        state_json = root / "candidate_voicing_state.json"
        write_csv(cand, "klon", csv_path)
        write_json(state_json, cand)
        ensure_target(root / "renders", source_render_dir, target_file)
        try:
            run([sys.executable, "SAT-TR/tools/run_overdrive_analysis_suite.py",
                 "--render-plan", args.render_plan,
                 "--render-dir", str(root / "renders"),
                 "--out-root", str(root),
                 "--sat-renderer-exe", args.renderer,
                 "--force-render-sat",
                 "--ts-cascade-csv", str(csv_path),
                 "--voicing-state", str(state_json),
                 "--sat-render-mode", "voiced",
                 "--fit-only", "--skip-hammerstein", "--no-fit-plot"], cwd)
            score = fit_score(root, args.case_id)
            row = {"label": label, "score": score, "layer": layer, "band_index": i, "delta_db": delta, "kind": band.get("kind"), "freq_hz": band.get("freq_hz")}
            results.append(row)
            print(json.dumps(row), flush=True)
            if score < best["score"]:
                best = {"score": score, "label": label, "state": cand, "row": row}
            write_running_summary(out_root, source_score, best, results)
        except subprocess.CalledProcessError as exc:
            results.append({"label": label, "error": exc.returncode})
            write_running_summary(out_root, source_score, best, results)

    summary = {
        "source_score": source_score,
        "previous_verified_best_score": previous_best,
        "acceptance_reference_score": acceptance_reference,
        "best_score": best["score"],
        "best_label": best["label"],
        "complete": True,
        "results": sorted(results, key=lambda r: r.get("score", 1e9)),
    }
    write_json(out_root / "summary.json", summary)
    print(json.dumps({k: summary[k] for k in ["source_score", "previous_verified_best_score", "acceptance_reference_score", "best_score", "best_label"]}, indent=2), flush=True)

    if args.apply and best["label"] != "source" and best["score"] < acceptance_reference:
        applied = best["state"]
        applied["klon"]["last_verified_best_score"] = best["score"]
        applied["klon"]["last_verified_best_candidate"] = best.get("row", {})
        applied["klon"]["last_verified_reference_score"] = source_score
        applied["klon"]["last_verified_updated_at_unix"] = time.time()
        write_json(state_path, applied)
        run([sys.executable, "SAT-TR/tools/write_overdrive_voicing_header.py"], cwd)
        print("APPLIED", best["label"], best["score"])
    elif args.apply and best["label"] != "source":
        print(json.dumps({
            "applied": False,
            "reason": "candidate did not beat acceptance reference",
            "candidate": best["label"],
            "candidate_score": best["score"],
            "acceptance_reference_score": acceptance_reference,
        }, indent=2), flush=True)

if __name__ == "__main__":
    main()
