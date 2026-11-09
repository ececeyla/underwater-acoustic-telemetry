"""Deterministic geometry and filtering checks; no recorded field data."""
import importlib.util
import math
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
import viewer_server as v


class ViewerMathTests(unittest.TestCase):
    def test_cartesian_to_gps_round_trip(self):
        lat, lon = v.local_xy_to_latlon(12.0, -4.0, 1.0, 1.0)
        x, y = v.latlon_to_local_xy(lat, lon, 1.0, 1.0)
        self.assertAlmostEqual(x, 12.0, places=5)
        self.assertAlmostEqual(y, -4.0, places=5)

    def test_three_anchor_solution(self):
        target = (10.0, 12.0)
        points = [(0.0, 0.0), (30.0, 0.0), (0.0, 30.0)]
        anchors = [
            (x, y, math.hypot(x - target[0], y - target[1]))
            for x, y in points
        ]
        result = v.trilaterate(anchors)
        self.assertIsNotNone(result)
        self.assertAlmostEqual(result[0], target[0], places=7)
        self.assertAlmostEqual(result[1], target[1], places=7)

    def test_degenerate_collinear_anchors(self):
        self.assertIsNone(v.trilaterate([(0, 0, 5), (10, 0, 5), (20, 0, 5)]))

    def test_invalid_ping_rejection(self):
        good = {"gpsFix": True, "pingOk": True, "lat": 1.0,
                "lon": 1.0, "pingDistanceM": 12.0}
        bad = dict(good, pingOk=False)
        too_far = dict(good, pingDistanceM=500.0)
        self.assertEqual(v.valid_ping_points([good, bad, too_far], 200.0), [good])

    def test_sync_window_switch(self):
        self.assertIsNone(v.parse_sync_window("off", 30.0))
        self.assertEqual(v.parse_sync_window(None, 30.0), 30.0)
        self.assertEqual(v.parse_sync_window("5", 30.0), 5.0)


if __name__ == "__main__":
    unittest.main()
