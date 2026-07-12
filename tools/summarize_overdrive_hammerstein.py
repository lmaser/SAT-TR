#!/usr/bin/env python3
"""Summarize Hammerstein branch diagnostics into band/order reports.

This script does not tune SAT-TR. It turns hammerstein_branch_summary.csv into
readable JSON/CSV/PNG so core mismatches are visible before POST matching.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean, median

CRITICAL_GROUPS = {
    "noise": {"white", "pink", "brown"},
    "tones": {"stepsine", "sweep"},
    "intermod": {"multitone", "twotone_lowmid", "twotone_himid", "tritone_mid", "tritone_himid"},
    "level_sweeps": {"white_sweep_level", "pink_sweep_level", "brown_sweep_level"},
}


def fnum(value: str) -> float | None:
    try:
        out = float(value)
        return out if math.isfinite(out) else None
    except Exception:
        return None


def group_for_kind(kind: str) -> str:
    for name, kinds in CRITICAL_GROUPS.items():
        if kind in kinds:
            return name
    return "other"


def stats(values: list[float]) -> dict:
    if not values:
        return {"count": 0}
    abs_values = [abs(v) for v in values]
    return {
        "count": len(values),
        "mean_db": mean(values),
        "median_db": median(values),
        "mean_abs_db": mean(abs_values),
        "max_abs_db": max(abs_values),
        "min_db": min(values),
        "max_db": max(values),
    }


def summarize(csv_path: Path) -> dict:
    rows = list(csv.DictReader(csv_path.open(newline="", encoding="utf-8")))
    if not rows or rows[0].get("skipped") == "True":
        return {"csv": str(csv_path), "skipped": True, "rows": len(rows)}

    orders = []
    for key in rows[0].keys():
        if key.startswith("order_") and key.endswith("_branch_median_delta_db"):
            orders.append(int(key.split("_")[1]))
    orders = sorted(set(orders))

    by_order = {}
    by_order_group = {}
    by_kind = {}
    level_trend = {}
    for order in orders:
        key = f"order_{order}_branch_median_delta_db"
        vals = [v for v in (fnum(r.get(key, "")) for r in rows) if v is not None]
        by_order[str(order)] = stats(vals)
        for group in sorted(set(group_for_kind(r.get("kind", "")) for r in rows)):
            gvals = [fnum(r.get(key, "")) for r in rows if group_for_kind(r.get("kind", "")) == group]
            by_order_group.setdefault(str(order), {})[group] = stats([v for v in gvals if v is not None])
        for kind in sorted(set(r.get("kind", "") for r in rows)):
            kvals = [fnum(r.get(key, "")) for r in rows if r.get("kind", "") == kind]
            by_kind.setdefault(kind, {})[str(order)] = stats([v for v in kvals if v is not None])

        level_pairs = []
        for r in rows:
            lvl = fnum(r.get("level_dbfs_rms", ""))
            val = fnum(r.get(key, ""))
            if lvl is not None and val is not None:
                level_pairs.append((lvl, val))
        level_pairs.sort()
        if level_pairs:
            low = [v for lvl, v in level_pairs if lvl <= -36.0]
            high = [v for lvl, v in level_pairs if lvl >= -15.0]
            level_trend[str(order)] = {
                "low_level": stats(low),
                "high_level": stats(high),
                "high_minus_low_mean_db": (mean(high) - mean(low)) if low and high else None,
            }

    weighted_score = 0.0
    weight_sum = 0.0
    weights = {1: 0.7, 2: 1.15, 3: 1.35, 5: 1.20, 7: 1.0}
    group_weights = {"intermod": 1.30, "tones": 1.15, "noise": 1.0, "level_sweeps": 1.15, "other": 1.0}
    for order in orders:
        for group, st in by_order_group[str(order)].items():
            if not st.get("count"):
                continue
            w = weights.get(order, 1.0) * group_weights.get(group, 1.0)
            weighted_score += w * float(st["mean_abs_db"])
            weight_sum += w

    return {
        "csv": str(csv_path),
        "skipped": False,
        "rows": len(rows),
        "orders": orders,
        "weighted_order_group_mean_abs_db": weighted_score / max(weight_sum, 1.0e-12),
        "by_order": by_order,
        "by_order_group": by_order_group,
        "by_kind": by_kind,
        "level_trend": level_trend,
        "interpretation": {
            "median_delta_db": "positive means SAT branch is lower than target for that order/segment; negative means SAT branch is hotter than target",
            "weighted_order_group_mean_abs_db": "diagnostic core/Hammer mismatch; lower is better, not directly comparable to static residual score",
        },
    }


def write_flat_csv(summary: dict, out: Path) -> None:
    rows = []
    for order, groups in summary.get("by_order_group", {}).items():
        for group, st in groups.items():
            row = {"order": order, "group": group}
            row.update(st)
            rows.append(row)
    if not rows:
        return
    with (out / "hammerstein_order_group_summary.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_plot(summary: dict, out: Path) -> None:
    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except Exception:
        return
    groups = sorted({g for v in summary.get("by_order_group", {}).values() for g in v.keys()})
    orders = [str(o) for o in summary.get("orders", [])]
    if not groups or not orders:
        return
    data = np.zeros((len(orders), len(groups)), dtype=float)
    for oi, order in enumerate(orders):
        for gi, group in enumerate(groups):
            data[oi, gi] = float(summary["by_order_group"].get(order, {}).get(group, {}).get("mean_abs_db", 0.0))
    fig, ax = plt.subplots(figsize=(max(7, len(groups) * 1.2), 4.8))
    im = ax.imshow(data, aspect="auto", cmap="magma")
    ax.set_xticks(range(len(groups)), groups, rotation=35, ha="right")
    ax.set_yticks(range(len(orders)), [f"H{o}" for o in orders])
    ax.set_title("Hammerstein SAT vs target mean abs delta by order/group")
    ax.set_ylabel("Branch order")
    fig.colorbar(im, ax=ax, label="mean abs delta (dB)")
    fig.tight_layout()
    fig.savefig(out / "hammerstein_order_group_heatmap.png", dpi=150)
    plt.close(fig)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    summary = summarize(Path(args.csv))
    (out / "hammerstein_order_summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8", newline="\n")
    write_flat_csv(summary, out)
    write_plot(summary, out)
    print(json.dumps({"wrote": str(out), "weighted_order_group_mean_abs_db": summary.get("weighted_order_group_mean_abs_db")}, indent=2))


if __name__ == "__main__":
    main()
