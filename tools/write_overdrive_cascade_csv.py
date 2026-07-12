#!/usr/bin/env python3
"""Export SAT-TR Overdrive voicing cascade layers to renderer/promoter CSV."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

LAYER_NAMES = ("pre_a", "pre_ndsp", "pre_b", "post_a", "post_ndsp", "post_b")


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default="SAT-TR/tools/overdrive_voicing_state.json")
    ap.add_argument("--pedal", choices=("ts808", "klon"), default="ts808")
    ap.add_argument("--out", required=True)
    ap.add_argument("--include-empty", action="store_true", help="Write neutral 0 dB rows for empty layers.")
    args = ap.parse_args()

    state = read_json(Path(args.state))
    ts = state[args.pedal]
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    with out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["layer", "kind", "freq_hz", "gain_db", "q", "stages", "amount"])
        writer.writeheader()
        for layer in LAYER_NAMES:
            bands = list(ts.get(layer, []))
            if not bands and args.include_empty:
                bands = [{"kind": "Peak", "freq_hz": 1000.0, "gain_db": 0.0, "q": 1.0, "stages": 1, "amount": "Fixed"}]
            for band in bands:
                writer.writerow({
                    "layer": layer,
                    "kind": band.get("kind", "Peak"),
                    "freq_hz": float(band.get("freq_hz", 1000.0)),
                    "gain_db": float(band.get("gain_db", 0.0)),
                    "q": float(band.get("q", 1.0)),
                    "stages": int(float(band.get("stages", 1))),
                    "amount": band.get("amount", "Classic"),
                })
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
