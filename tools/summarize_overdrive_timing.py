#!/usr/bin/env python3
"""Summarize SAT-TR overdrive iteration timing JSONL logs."""
from __future__ import annotations

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path


def classify(cmd: list[str]) -> str:
    joined = " ".join(cmd)
    if "run_overdrive_analysis_suite.py" in joined:
        if "--skip-hammerstein" in cmd:
            return "analysis_fast_fit"
        return "analysis_full_guard"
    if "promote_overdrive_plan_fit.py" in joined:
        return "promote_residual"
    if "optimize_overdrive_ts808_core.py" in joined:
        return "optimize_core"
    if "fit_overdrive_ts808_controls.py" in joined:
        return "control_fit"
    if "build_sat_overdrive_renderer.ps1" in joined:
        return "build_renderer"
    if "write_overdrive_voicing_header.py" in joined:
        return "write_header"
    if "set_overdrive_ts808_variant.py" in joined:
        return "set_variant"
    return Path(cmd[1]).name if len(cmd) > 1 else (cmd[0] if cmd else "unknown")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", default="analysis_out/ts808_contract_v2/iteration_timing.jsonl")
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    path = Path(args.log)
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    if not rows:
        print(f"No timing rows in {path}")
        return

    groups = defaultdict(list)
    for row in rows:
        groups[classify(row.get("cmd", []))].append(float(row.get("elapsed_sec", 0.0)))

    print(f"Timing log: {path}")
    print(f"Commands: {len(rows)} total_sec={sum(float(r.get('elapsed_sec', 0.0)) for r in rows):.2f}")
    print("\nBy stage:")
    for name, vals in sorted(groups.items(), key=lambda kv: sum(kv[1]), reverse=True):
        print(f"  {name:22s} count={len(vals):3d} total={sum(vals):8.2f}s median={statistics.median(vals):7.2f}s max={max(vals):7.2f}s")

    print("\nSlowest commands:")
    for row in sorted(rows, key=lambda r: float(r.get("elapsed_sec", 0.0)), reverse=True)[:max(1, args.top)]:
        cmd = row.get("cmd", [])
        label = classify(cmd)
        print(f"  {float(row.get('elapsed_sec', 0.0)):8.2f}s rc={row.get('returncode')} {label}: {' '.join(cmd[:8])}")


if __name__ == "__main__":
    main()
