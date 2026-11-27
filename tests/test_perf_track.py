import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("perf_track", ROOT / "tools/perf_track.py")
perf_track = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf_track)

# As app::bench_line writes it (tests/frame_bench_test.cpp), after the log's own prefix.
LINE = ("[2026-11-26 03:40:01.123] [info] bench: label=qeytoqrg frames=600 mean_ms=4.100 p50_ms=4.000 "
        "p95_ms=5.000 p99_ms=6.000 max_ms=9.000 hitches=1 resident_mib=1500.5 gpu_mib=800.0")


class ParseTests(unittest.TestCase):
    def test_the_last_bench_line_is_read(self):
        text = "noise\n" + LINE.replace("4.100", "1.0") + "\nmore\n" + LINE + "\n"
        r = perf_track.parse_bench(text)
        self.assertEqual(r["label"], "qeytoqrg")
        self.assertEqual(r["mean_ms"], 4.1)
        self.assertEqual(r["resident_mib"], 1500.5)
        self.assertNotIn("commit_mib", r)

    def test_the_memory_checkpoints_after_it_are_not_the_bench_line(self):
        text = LINE + "\n[info] memory checkpoint: bench: resident 958.80 MiB\n" \
                      "[info] memory GPU: bench: heap 1 (device-local), allocation bytes 571.90 MiB\n"
        self.assertEqual(perf_track.parse_bench(text)["mean_ms"], 4.1)

    def test_no_bench_line(self):
        self.assertIsNone(perf_track.parse_bench("[info] frame time: 1 ms\n"))

    def test_runs_are_combined_by_median(self):
        runs = [perf_track.parse_bench(LINE.replace("mean_ms=4.100", f"mean_ms={v}")) for v in ("3", "9", "4")]
        self.assertEqual(perf_track.combine(runs)["mean_ms"], 4.0)


class CompareTests(unittest.TestCase):
    def history(self, means, scene="viewer:qeytoqrg"):
        return [{"scene": scene, "mean_ms": str(m), "p99_ms": "6", "resident_mib": "1500", "gpu_mib": "800"}
                for m in means]

    def test_a_rise_past_share_and_floor_is_a_regression(self):
        _, reg = perf_track.compare(self.history([4.0, 4.1, 3.9]), "viewer:qeytoqrg",
                                    {"mean_ms": 5.0, "p99_ms": 6.0, "resident_mib": 1500.0, "gpu_mib": 800.0})
        self.assertEqual([r[0] for r in reg], ["mean_ms"])
        self.assertEqual(reg[0][2], 4.0)

    def test_small_frames_need_the_absolute_floor_too(self):
        # 0.2 -> 0.4 ms doubles, but stays under the 0.25 ms floor.
        _, reg = perf_track.compare(self.history([0.2, 0.2, 0.2]), "viewer:qeytoqrg", {"mean_ms": 0.4})
        self.assertEqual(reg, [])

    def test_too_little_history_or_another_scene_compares_nothing(self):
        _, reg = perf_track.compare(self.history([1.0, 1.0]), "viewer:qeytoqrg", {"mean_ms": 50.0})
        self.assertEqual(reg, [])
        _, reg = perf_track.compare(self.history([1.0] * 5, scene="viewer:qeynos2"), "viewer:qeytoqrg",
                                    {"mean_ms": 50.0})
        self.assertEqual(reg, [])

    def test_only_the_last_window_counts(self):
        hist = self.history([10.0] * 10 + [4.0] * 7)
        _, reg = perf_track.compare(hist, "viewer:qeytoqrg", {"mean_ms": 5.0}, window=7)
        self.assertEqual([r[0] for r in reg], ["mean_ms"])

    def test_a_client_scene_is_flagged_on_memory_only(self):
        hist = self.history([4.0] * 3, scene="client:qeytoqrg")
        _, reg = perf_track.compare(hist, "client:qeytoqrg",
                                    {"mean_ms": 9.0, "p99_ms": 20.0, "resident_mib": 1700.0, "gpu_mib": 800.0})
        self.assertEqual([r[0] for r in reg], ["resident_mib"])

    def test_history_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "sub" / "perf.csv"
            perf_track.append_rows(path, [{"scene": "viewer:a", "mean_ms": 1.5, "commit": "abc"}])
            perf_track.append_rows(path, [{"scene": "viewer:a", "mean_ms": 2.5}])
            rows = perf_track.read_history(path)
            self.assertEqual([r["mean_ms"] for r in rows], ["1.5", "2.5"])
            self.assertEqual(rows[0]["commit"], "abc")

    def test_split_host(self):
        self.assertEqual(perf_track.split_host("10.0.0.1:5998"), ("10.0.0.1", 5998))
        self.assertEqual(perf_track.split_host("example.test"), ("example.test", 5999))


if __name__ == "__main__":
    unittest.main()
