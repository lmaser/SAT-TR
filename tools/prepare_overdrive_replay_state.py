#!/usr/bin/env python3
"""Prepare controlled TS808 replay states for SAT-TR Overdrive tuning.

The replay state preserves the current TS808 nonlinear core by default, but
clears learned cascade voicing layers. This tests whether the optimizer can
recover a similar cascade from a clean residual state without relying on the
historical accepted snapshots.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

PRE_LAYERS = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYERS = ("post_a", "post_ndsp", "post_b")
HISTORY_PREFIXES = ("last_",)


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def neutral_peak(freq: float = 1000.0) -> dict[str, Any]:
    return {"kind": "Peak", "freq_hz": float(freq), "q": 1.0, "gain_db": 0.0, "stages": 1, "amount": "Fixed"}


def clear_history(ts: dict[str, Any]) -> None:
    for key in list(ts.keys()):
        if any(key.startswith(prefix) for prefix in HISTORY_PREFIXES):
            del ts[key]


def prepare_state(state: dict[str, Any], *, mode: str, clear_history_fields: bool) -> dict[str, Any]:
    out = json.loads(json.dumps(state))
    ts = out.setdefault("ts808", {})
    if mode == "core-current-no-cascade":
        for layer in PRE_LAYERS:
            ts[layer] = []
        for layer in POST_LAYERS:
            ts[layer] = []
        ts["residual_pre"] = []
        ts["post_residual"] = []
        ts["base_voicing_enabled"] = False
        ts["post_hicut_enabled"] = False
        ts["residual_matching_enabled"] = True
        ts["active_variant"] = "core-residual"
        ts["active_variant_description"] = "Replay audit: current TS808 core with cleared learned cascade voicing."
    elif mode == "ndsp-neutral-cascade":
        ts["pre_a"] = [neutral_peak(), neutral_peak(), neutral_peak()]
        ts["pre_ndsp"] = []
        ts["pre_b"] = []
        ts["post_a"] = [neutral_peak(), neutral_peak(), neutral_peak()]
        ts["post_ndsp"] = []
        ts["post_b"] = []
        ts["residual_pre"] = list(ts["pre_a"])
        ts["post_residual"] = list(ts["post_a"])
        ts["base_voicing_enabled"] = False
        ts["post_hicut_enabled"] = False
        ts["residual_matching_enabled"] = True
        ts["active_variant"] = "core-residual"
        ts["active_variant_description"] = "Replay audit: current TS808 core with neutral explicit 3EQ cascade."
    else:
        raise SystemExit(f"unsupported replay mode: {mode}")

    if clear_history_fields:
        clear_history(ts)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--mode", choices=["core-current-no-cascade", "ndsp-neutral-cascade"], default="core-current-no-cascade")
    ap.add_argument("--keep-history", action="store_true")
    args = ap.parse_args()

    source = Path(args.source)
    out = Path(args.out)
    state = read_json(source)
    replay = prepare_state(state, mode=args.mode, clear_history_fields=not args.keep_history)
    write_json(out, replay)
    ts = replay.get("ts808", {})
    print(json.dumps({
        "wrote": str(out),
        "mode": args.mode,
        "core_preserved": "core" in ts,
        "layer_counts": {k: len(ts.get(k, [])) for k in (*PRE_LAYERS, *POST_LAYERS)},
        "variant": ts.get("active_variant"),
    }, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
