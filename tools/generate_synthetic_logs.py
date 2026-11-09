#!/usr/bin/env python3
"""Create clearly synthetic, deterministic-range telemetry logs for the local viewer.

These coordinates, ranges and ping timestamps are FABRICATED DEMO DATA,
not measurements or a reconstruction of the UPC internship deployment.
This script never overwrites existing node logs.
"""
import argparse
import json
import math
from datetime import datetime, timezone
from pathlib import Path

EARTH_RADIUS_M = 6371000.0
REF_LAT_DEG = 1.0  # Intentionally artificial reference coordinates
REF_LON_DEG = 1.0
ANCHORS_M = {"A": (0.0, 0.0), "B": (30.0, 0.0), "C": (0.0, 30.0)}
TARGET_M = (10.0, 12.0)
SPEED_SOUND_M_S = 1500.0


def to_latlon(x_m, y_m):
    lat = REF_LAT_DEG + math.degrees(y_m / EARTH_RADIUS_M)
    lon = REF_LON_DEG + math.degrees(
        x_m / (EARTH_RADIUS_M * math.cos(math.radians(REF_LAT_DEG)))
    )
    return lat, lon


def build_records(timestamp):
    records = {}
    for node, (x, y) in ANCHORS_M.items():
        lat, lon = to_latlon(x, y)
        rng = math.hypot(x - TARGET_M[0], y - TARGET_M[1])
        records[node] = {
            "receivedAt": timestamp,
            "from": "synthetic-fixture",
            "data": {
                "nodeId": node,
                "latitude": lat,
                "longitude": lon,
                "gpsFix": True,
                "pingOk": True,
                "pingRttMs": rng * 2000.0 / SPEED_SOUND_M_S,
                "pingDistanceM": rng,
                "gsmRegistered": False,
                "gsmDataConnected": False,
                "acousticModemHealthy": True,
            },
        }
    return records


def write_records(directory):
    directory = Path(directory)
    timestamp = datetime.now(timezone.utc).isoformat()
    records = build_records(timestamp)
    directory.mkdir(parents=True, exist_ok=True)
    targets = {name: directory / f"{name}.log" for name in records}
    existing = [str(path) for path in targets.values() if path.exists()]
    if existing:
        raise FileExistsError("Refusing to overwrite existing logs: " + ", ".join(existing))
    for node, path in targets.items():
        path.write_text(json.dumps(records[node]) + "\n", encoding="utf-8")
    return list(targets.values())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True, type=Path,
                        help="Explicit output directory, e.g. server/logs")
    arguments = parser.parse_args()
    for path in write_records(arguments.out_dir):
        print("Created synthetic fixture:", path)
