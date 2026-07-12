#!/usr/bin/env python3
"""Write a small TS808 render plan for Overdrive INPUT/DRIVE analysis.

This does not render audio. It creates a JSON checklist with deterministic file
names and metadata so analysis can treat INPUT, DRIVE and OUTPUT as separate
variables instead of guessing from filenames.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path


def pct_label(value: float) -> str:
    return str(int(round(value * 100.0))).zfill(3)


def in_label(db: float) -> str:
    if db == 0:
        return "in_p0"
    prefix = "p" if db > 0 else "m"
    return f"in_{prefix}{abs(db):g}".replace(".", "p")


PRESETS = {
    "quick": {
        "drive_points": "1.00",
        "input_points_db": "0",
        "description": "Single max-drive case for fast smoke tests.",
    },
    "release": {
        "drive_points": "0.40,0.70,1.00",
        "input_points_db": "-3,0,3",
        "description": "Final voicing pass: drive curve plus local input sensitivity around unity.",
    },
    "fine-input": {
        "drive_points": "0.40,0.70,1.00",
        "input_points_db": "-3,-2,-1,0,1,2,3",
        "description": "Dense local input sweep for calibrating gain/drive interaction.",
    },
    "wide": {
        "drive_points": "0.40,0.70,1.00",
        "input_points_db": "-24,-12,0,6",
        "description": "Broad diagnostic sweep for low-level and hot-input behaviour.",
    },
}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    ap.add_argument("--pedal", choices=["ts808", "klon"], default="ts808",
                    help="Analysis target. klon maps to SAT-TR Overdrive B and uses isolated file names.")
    ap.add_argument("--batch-file", default="overdrive_id_batch.wav")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="release",
                    help="Case grid preset. Use wide only for broad dynamic diagnostics.")
    ap.add_argument("--drive-points", default=None,
                    help="Comma-separated DRIVE values. Overrides the preset drive list.")
    ap.add_argument("--input-points-db", default=None,
                    help="Comma-separated input dB values. Overrides the preset input list.")
    ap.add_argument("--input-sweep-drive", type=float, default=1.0,
                    help="DRIVE value used for the dedicated input sweep.")
    ap.add_argument("--knee", type=float, default=0.0)
    ap.add_argument("--type", type=float, default=None)
    ap.add_argument("--sat-output-db", type=float, default=0.0)
    ap.add_argument("--target-output-db", type=float, default=0.0)
    args = ap.parse_args()
    if args.out is None:
        args.out = f"SAT-TR/tools/overdrive_id_renders/render_plan_{args.pedal}.json"
    if args.type is None:
        args.type = 1.0 if args.pedal == "klon" else 0.0

    preset = PRESETS[args.preset]
    drive_points_text = args.drive_points or preset["drive_points"]
    input_points_text = args.input_points_db or preset["input_points_db"]
    drive_points = [float(x.strip()) for x in drive_points_text.split(",") if x.strip()]
    input_points = [float(x.strip()) for x in input_points_text.split(",") if x.strip()]

    cases = []
    seen = set()
    seen_settings = set()

    def add_case(kind: str, drive: float, input_db: float) -> None:
        settings_key = (round(float(drive), 6), round(float(input_db), 6))
        if settings_key in seen_settings:
            return
        seen_settings.add(settings_key)
        case_id = f"{args.pedal}_{kind}_drv{pct_label(drive)}_{in_label(input_db)}"
        if case_id in seen:
            return
        seen.add(case_id)
        cases.append({
            "id": case_id,
            "description": f"{args.pedal.upper()} Overdrive identification case",
            "source_batch_file": args.batch_file,
            "files": {
                "sat_raw": f"{case_id}__sat_raw.wav",
                "sat_voiced": f"{case_id}__sat_voiced.wav",
                "target": f"{case_id}__target.wav"
            },
            "sat_settings": {
                "plugin": "SAT-TR",
                "model": "Overdrive B" if args.pedal == "klon" else "Overdrive",
                "engine_model": "klon" if args.pedal == "klon" else "ts808",
                "type": args.type,
                "drive": drive,
                "knee": args.knee,
                "raw_for_sat_raw": True,
                "raw_for_sat_voiced": False,
                "host_or_plugin_input_db": input_db,
                "output_db": args.sat_output_db
            },
            "target_settings": {
                "model": "Klon reference" if args.pedal == "klon" else "TS808 reference",
                "type_or_tone_note": "use equivalent Klon tone setting" if args.pedal == "klon" else "use equivalent TS808 reference tone setting",
                "drive": drive,
                "host_or_plugin_input_db": input_db,
                "output_db": args.target_output_db
            },
            "analysis_focus": kind
        })

    for drive in drive_points:
        add_case("drive", drive, 0.0)
    for input_db in input_points:
        add_case("input", args.input_sweep_drive, input_db)

    plan = {
        "version": 1,
        "target": f"SAT-TR Overdrive {args.pedal.upper()} INPUT/DRIVE plan",
        "pedal": args.pedal,
        "preset": args.preset,
        "preset_description": preset["description"],
        "drive_points": drive_points,
        "input_points_db": input_points,
        "notes": [
            "Render every case from the same dry batch WAV.",
            "Do not change OS/sample-rate/routing between cases.",
            "INPUT and DRIVE are intentionally tracked separately.",
            "For final voicing, release is preferred over wide unless low-level dynamics are being diagnosed."
        ],
        "cases": cases
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_text(json.dumps(plan, indent=2), encoding="utf-8", newline="\n")
    os.replace(tmp, out)
    print(f"Wrote render plan: {out}")
    print("Render files required:")
    for case in cases:
        print(f"  [{case['id']}]")
        print(f"    {case['files']['sat_raw']}")
        print(f"    {case['files']['sat_voiced']}")
        print(f"    {case['files']['target']}")


if __name__ == "__main__":
    main()
