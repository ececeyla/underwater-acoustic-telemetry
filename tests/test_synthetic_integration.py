"""End-to-end offline test using deliberately fabricated telemetry records."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
sys.path.insert(0, str(ROOT / "tools"))

import viewer_server as viewer
import generate_synthetic_logs as demo


class SyntheticIntegrationTests(unittest.TestCase):
    def test_generated_node_logs_reach_trilateration(self):
        with tempfile.TemporaryDirectory() as folder:
            demo.write_records(folder)
            previous_dir = viewer.LOG_DIR
            try:
                viewer.LOG_DIR = folder
                records = {
                    name: viewer.read_node_points(name) for name in ("A", "B", "C")
                }
                self.assertTrue(all(len(records[name]) == 1 for name in records))
                fix = viewer.compute_live_target(
                    records, ["A", "B", "C"], sync_window_s=30.0,
                    max_range_m=200.0
                )
            finally:
                viewer.LOG_DIR = previous_dir
            self.assertTrue(fix["available"], msg=str(fix))
            expected_lat, expected_lon = demo.to_latlon(*demo.TARGET_M)
            self.assertAlmostEqual(fix["lat"], expected_lat, places=6)
            self.assertAlmostEqual(fix["lon"], expected_lon, places=6)


if __name__ == "__main__":
    unittest.main()
