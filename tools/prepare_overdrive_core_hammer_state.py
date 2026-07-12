#!/usr/bin/env python3
"""Prepare a TS808 audit state with PRE/core active and POST neutral.

This is a diagnostic state, not a promoted voicing. It answers the specific
question: "if the current PRE cascade feeds the current nonlinear core, how
close is the Hammerstein/dynamic behavior before residual POST matching?"
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

PRE_LAYERS = ("pre_a", "pre_ndsp", "pre_b")
POST_LAYERS = ("post_a", "post_ndsp", "post_b")
CASCADE_LAYERS = (*PRE_LAYERS, *POST_LAYERS)


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def zero_gain_band(band: dict[str, Any]) -> dict[str, Any]:
    out = dict(band)
    out["gain_db"] = 0.0
    return out


def write_cascade_csv(path: Path, state: dict[str, Any]) -> None:
    ts = state.get("ts808", {})
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["layer", "kind", "freq_hz", "gain_db", "q", "stages", "amount"])
        writer.writeheader()
        for layer in CASCADE_LAYERS:
            for band in ts.get(layer, []):
                writer.writerow({
                    "layer": layer,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 1000.0)),
                    "gain_db": float(band.get("gain_db", 0.0)),
                    "q": float(band.get("q", 1.0)),
                    "stages": int(float(band.get("stages", 1))),
                    "amount": band.get("amount", "Classic"),
                })


def prepare_core_hammer_state(state: dict[str, Any]) -> dict[str, Any]:
    out = json.loads(json.dumps(state))
    ts = out.setdefault("ts808", {})
    for layer in POST_LAYERS:
        ts[layer] = [zero_gain_band(band) for band in ts.get(layer, [])]
    ts["post_residual"] = [zero_gain_band(band) for band in ts.get("post_residual", ts.get("post_a", []))]
    ts["base_voicing_enabled"] = False
    ts["post_hicut_enabled"] = False
    ts["residual_matching_enabled"] = True
    ts["active_variant"] = "core-hammer-audit"
    ts["active_variant_description"] = "Diagnostic: current PRE/core with POST residual layers neutralized."
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--state-out", required=True)
    ap.add_argument("--cascade-out", required=True)
    args = ap.parse_args()

    state = read_json(Path(args.source))
    audit_state = prepare_core_hammer_state(state)
    state_out = Path(args.state_out)
    cascade_out = Path(args.cascade_out)
    write_json(state_out, audit_state)
    write_cascade_csv(cascade_out, audit_state)
    ts = audit_state.get("ts808", {})
    print(json.dumps({
        "state_out": str(state_out),
        "cascade_out": str(cascade_out),
        "pre_counts": {layer: len(ts.get(layer, [])) for layer in PRE_LAYERS},
        "post_counts_neutralized": {layer: len(ts.get(layer, [])) for layer in POST_LAYERS},
        "residual_matching_enabled": ts.get("residual_matching_enabled"),
    }, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
